/*-------------------------------------------------------------------------
 *
 * smgr.c
 *	  public interface routines to storage manager switch.
 *
 *	  All file system operations in POSTGRES dispatch through these
 *	  routines.
 *
 * Portions Copyright (c) 2006-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/smgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/aomd.h"
#include "access/relation.h"
#include "access/xact.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "commands/defrem.h"
#include "postmaster/autovacuum.h"
#include "postmaster/postmaster.h"
#include "access/xlog.h"
#include "lib/ilist.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/md.h"
#include "storage/smgr.h"
#include "utils/faultinjector.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/relfilenodemap.h"

/*
 * Hook for plugins to collect statistics from storage functions
 * For example, disk quota extension will use these hooks to
 * detect active tables.
 */
file_create_hook_type file_create_hook = NULL;
file_extend_hook_type file_extend_hook = NULL;
file_truncate_hook_type file_truncate_hook = NULL;
file_unlink_hook_type file_unlink_hook = NULL;

smgr_get_impl_hook_type smgr_get_impl_hook = NULL;

/* Hook for plugins to get control in smgr */
smgr_init_hook_type smgr_init_hook = NULL;
smgr_hook_type smgr_hook = NULL;
smgr_shutdown_hook_type smgr_shutdown_hook = NULL;
#define SMGR_MAX_ID UINT8_MAX
static f_smgr smgrsw[SMGR_MAX_ID + 1] = {
	/* magnetic disk */
	{
		.smgr_name = "heap",
		.smgr_init = mdinit,
		.smgr_shutdown = NULL,
		.smgr_open = mdopen,
		.smgr_close = mdclose,
		.smgr_create = mdcreate,
		.smgr_exists = mdexists,
		.smgr_unlink = mdunlink,
		.smgr_extend = mdextend,
		.smgr_prefetch = mdprefetch,
		.smgr_read = mdread,
		.smgr_write = mdwrite,
		.smgr_writeback = mdwriteback,
		.smgr_nblocks = mdnblocks,
		.smgr_truncate = mdtruncate,
		.smgr_immedsync = mdimmedsync,
	},
	/*
	 * Relation files that are different from heap, characterised by:
	 *     1. variable blocksize
	 *     2. block numbers are not consecutive
	 *     3. shared buffers are not used
	 * Append-optimized relation files currently fall in this category.
	 */
	{
		.smgr_name = "ao",
		.smgr_init = mdinit,
		.smgr_shutdown = NULL,
		.smgr_open = mdopen,
		.smgr_close = mdclose,
		.smgr_create = mdcreate,
		.smgr_exists = mdexists,
		.smgr_unlink = mdunlink_ao,
		.smgr_extend = mdextend,
		.smgr_prefetch = mdprefetch,
		.smgr_read = mdread,
		.smgr_write = mdwrite,
		.smgr_writeback = mdwriteback,
		.smgr_nblocks = mdnblocks,
		.smgr_truncate = mdtruncate,
		.smgr_immedsync = mdimmedsync,
	}
};

static File	AORelOpenSegFile(__attribute__((unused))Oid reloid, const char *filePath, int fileFlags)
{
	return PathNameOpenFile(filePath, fileFlags);
}

static File	AORelOpenSegFileXlog(RelFileNode node, int32 segmentFileNum, int fileFlags)
{
	char	   *dbPath;
	char		path[MAXPGPATH];
	dbPath = GetDatabasePath(node.dbNode,
							 node.spcNode);

	if (segmentFileNum == 0)
		snprintf(path, MAXPGPATH, "%s/%u", dbPath, node.relNode);
	else
		snprintf(path, MAXPGPATH, "%s/%u.%u", dbPath, node.relNode, segmentFileNum);
	pfree(dbPath);

	return PathNameOpenFile(path, fileFlags);
}

static const f_smgr_ao smgrswao[] = {
	/* regular file */
	{
		.smgr_FileClose = FileClose,
		.smgr_FileDiskSize = FileDiskSize,
		.smgr_FileTruncate = FileTruncate,
		.smgr_AORelOpenSegFile = AORelOpenSegFile,
		.smgr_AORelOpenSegFileXlog = AORelOpenSegFileXlog,
		.smgr_FileWrite = FileWrite,
		.smgr_FileRead = FileRead,
		.smgr_FileSize = FileSize,
		.smgr_FileSync = FileSync,
	},
};


/*
 * Each backend has a hashtable that stores all extant SMgrRelation objects.
 * In addition, "unowned" SMgrRelation objects are chained together in a list.
 */
static HTAB *SMgrRelationHash = NULL;

static dlist_head unowned_relns;

/* local function prototypes */
static void smgrshutdown(int code, Datum arg);

void smgr_register(const f_smgr *smgr, SMgrImpl smgr_impl)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		ereport(ERROR, (errmsg("smgr_register not in shared_preload_libraries")));
	}
	
	if (smgr_impl > SMGR_MAX_ID)
	{
		elog(ERROR, "smgr_impl is out of range");
	}

	if (smgr->smgr_name == NULL)
	{
		elog(ERROR, "smgr_name is not set");
	}

	// check if the smgr_impl is already registered, avoid conflict with existing smgr_impl
	if (smgrsw[smgr_impl].smgr_name != NULL)
	{
		elog(ERROR, "smgr_impl is already registered");
	}

	smgrsw[smgr_impl] = *smgr;
}

const f_smgr *smgr_get(SMgrImpl smgr_impl)
{
	if (smgr_impl > SMGR_MAX_ID)
	{
		elog(ERROR, "smgr_impl is out of range");
	}

	if (smgrsw[smgr_impl].smgr_name == NULL)
	{
		elog(ERROR, "smgr_impl is not registered");
	}

	return &smgrsw[smgr_impl];
}

/*
 * smgr_get_impl() is used to get the smgr id of the relation.
 *
 * FIXME: For PAX_AM_OID, Cloudberry reserves this value for ORCA, a
 * predefined value is used here to reserve the smgr id for PAX_AM_OID.
 * should we add a hook to get the smgr id of the relation?
 */
SMgrImpl smgr_get_impl(const Relation rel)
{
	SMgrImpl smgr_impl = SMGR_MD;

	if (RelationIsAppendOptimized(rel))
	{
		smgr_impl = SMGR_AO;
	}
	else if (RelationIsPax(rel))
	{
		smgr_impl = SMGR_PAX;
	}
	else
	{
		if (smgr_get_impl_hook)
		{
			(*smgr_get_impl_hook)(rel, &smgr_impl);
			Assert(smgr_impl >= SMGR_MD && smgr_impl <= SMGR_MAX_ID);
			Assert(smgrsw[smgr_impl].smgr_name != NULL);
		}
	}


	return smgr_impl;
}

/*
 *	smgrinit(), smgrshutdown() -- Initialize or shut down storage
 *								  managers.
 *
 * Note: smgrinit is called during backend startup (normal or standalone
 * case), *not* during postmaster start.  Therefore, any resources created
 * here or destroyed in smgrshutdown are backend-local.
 */
void
smgrinit(void)
{
	int			i;

	for (i = 0; i <= SMGR_MAX_ID; i++)
	{
		if (smgrsw[i].smgr_init)
			smgrsw[i].smgr_init();
	}

	if (smgr_init_hook)
		(*smgr_init_hook)();

	/* register the shutdown proc */
	on_proc_exit(smgrshutdown, 0);
}

/*
 * on_proc_exit hook for smgr cleanup during backend shutdown
 */
static void
smgrshutdown(int code, Datum arg)
{
	int			i;

	for (i = 0; i <= SMGR_MAX_ID; i++)
	{
		if (smgrsw[i].smgr_shutdown)
			smgrsw[i].smgr_shutdown();
	}
}

const struct f_smgr_ao *
smgrAOGetDefault(void) {
	return &smgrswao[0];
}

/*
 *	smgropen() -- Return an SMgrRelation object, creating it if need be.
 *
 *		This does not attempt to actually open the underlying file.
 */
SMgrRelation
smgropen(RelFileNode rnode, BackendId backend, SMgrImpl which, Relation rel)
{
	RelFileNodeBackend brnode;
	SMgrRelation reln;
	bool		found;

	/* GPDB: don't support MyBackendId as a possible backend. */
	Assert(backend == InvalidBackendId || backend == TempRelBackendId);

	if (SMgrRelationHash == NULL)
	{
		/* First time through: initialize the hash table */
		HASHCTL		ctl;

		ctl.keysize = sizeof(RelFileNodeBackend);
		ctl.entrysize = sizeof(SMgrRelationData);
		SMgrRelationHash = hash_create("smgr relation table", 400,
									   &ctl, HASH_ELEM | HASH_BLOBS);
		dlist_init(&unowned_relns);
	}

	/* Look up or create an entry */
	memset(&brnode, 0, sizeof(RelFileNodeBackend));
	brnode.node = rnode;
	brnode.backend = backend;
	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  (void *) &brnode,
									  HASH_ENTER, &found);

	/* Initialize it if not present before */
	if (!found)
	{
		/* hash_search already filled in the lookup key */
		reln->smgr_owner = NULL;
		reln->smgr_targblock = InvalidBlockNumber;
		for (int i = 0; i <= MAX_FORKNUM; ++i)
			reln->smgr_cached_nblocks[i] = InvalidBlockNumber;
		reln->smgr_which = which; /* GPDB add SMGR_AO*/

		/* it has no owner yet */
		dlist_push_tail(&unowned_relns, &reln->node);
		reln->smgr = &smgrsw[reln->smgr_which];

		reln->smgr_ao = &smgrswao[0];

		/*
		 * hook for other storage managers.
		 */
		if (smgr_hook)
			(*smgr_hook) (reln, backend, which, rel);

		Assert(reln->smgr);
		Assert(reln->smgr_ao);

		(*reln->smgr).smgr_open(reln);
	}

	return reln;
}

/*
 * smgrsetowner() -- Establish a long-lived reference to an SMgrRelation object
 *
 * There can be only one owner at a time; this is sufficient since currently
 * the only such owners exist in the relcache.
 */
void
smgrsetowner(SMgrRelation *owner, SMgrRelation reln)
{
	/* We don't support "disowning" an SMgrRelation here, use smgrclearowner */
	Assert(owner != NULL);

	/*
	 * First, unhook any old owner.  (Normally there shouldn't be any, but it
	 * seems possible that this can happen during swap_relation_files()
	 * depending on the order of processing.  It's ok to close the old
	 * relcache entry early in that case.)
	 *
	 * If there isn't an old owner, then the reln should be in the unowned
	 * list, and we need to remove it.
	 */
	if (reln->smgr_owner)
		*(reln->smgr_owner) = NULL;
	else
		dlist_delete(&reln->node);

	/* Now establish the ownership relationship. */
	reln->smgr_owner = owner;
	*owner = reln;
}

/*
 * smgrclearowner() -- Remove long-lived reference to an SMgrRelation object
 *					   if one exists
 */
void
smgrclearowner(SMgrRelation *owner, SMgrRelation reln)
{
	/* Do nothing if the SMgrRelation object is not owned by the owner */
	if (reln->smgr_owner != owner)
		return;

	/* unset the owner's reference */
	*owner = NULL;

	/* unset our reference to the owner */
	reln->smgr_owner = NULL;

	/* add to list of unowned relations */
	dlist_push_tail(&unowned_relns, &reln->node);
}

/*
 *	smgrexists() -- Does the underlying file for a fork exist?
 */
bool
smgrexists(SMgrRelation reln, ForkNumber forknum)
{
	return (*reln->smgr).smgr_exists(reln, forknum);
}

/*
 *	smgrclose() -- Close and delete an SMgrRelation object.
 */
void
smgrclose(SMgrRelation reln)
{
	SMgrRelation *owner;
	ForkNumber	forknum;

	for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		(*reln->smgr).smgr_close(reln, forknum);

	owner = reln->smgr_owner;

	if (!owner)
		dlist_delete(&reln->node);

	if (hash_search(SMgrRelationHash,
					(void *) &(reln->smgr_rnode),
					HASH_REMOVE, NULL) == NULL)
		elog(ERROR, "SMgrRelation hashtable corrupted");

	/*
	 * Unhook the owner pointer, if any.  We do this last since in the remote
	 * possibility of failure above, the SMgrRelation object will still exist.
	 */
	if (owner)
		*owner = NULL;
}

/*
 *	smgrcloseall() -- Close all existing SMgrRelation objects.
 */
void
smgrcloseall(void)
{
	HASH_SEQ_STATUS status;
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	hash_seq_init(&status, SMgrRelationHash);

	while ((reln = (SMgrRelation) hash_seq_search(&status)) != NULL)
		smgrclose(reln);
}

/*
 *	smgrclosenode() -- Close SMgrRelation object for given RelFileNode,
 *					   if one exists.
 *
 * This has the same effects as smgrclose(smgropen(rnode)), but it avoids
 * uselessly creating a hashtable entry only to drop it again when no
 * such entry exists already.
 */
void
smgrclosenode(RelFileNodeBackend rnode)
{
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  (void *) &rnode,
									  HASH_FIND, NULL);
	if (reln != NULL)
		smgrclose(reln);
}

/*
 *	smgrcreate() -- Create a new relation.
 *
 *		Given an already-created (but presumably unused) SMgrRelation,
 *		cause the underlying disk file or other storage for the fork
 *		to be created.
 */
void
smgrcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	(*reln->smgr).smgr_create(reln, forknum, isRedo);

	if (file_create_hook)
		(*file_create_hook)(reln->smgr_rnode);
}

/*
 *		smgrcreate_ao() -- Create a new AO relation segment.
 *		Given a RelFileNode, cause the underlying disk file for the
 *		AO segment to be created.
 *
 *		If isRedo is true, it is okay for the underlying file to exist
 *		already because we are in a WAL replay sequence.
 */
void
smgrcreate_ao(RelFileNodeBackend rnode, int32 segmentFileNum, bool isRedo)
{
	mdcreate_ao(rnode, segmentFileNum, isRedo);
	if (file_create_hook)
		(*file_create_hook)(rnode);
}

/*
 *	smgrdosyncall() -- Immediately sync all forks of all given relations
 *
 *		All forks of all given relations are synced out to the store.
 *
 *		This is equivalent to FlushRelationBuffers() for each smgr relation,
 *		then calling smgrimmedsync() for all forks of each relation, but it's
 *		significantly quicker so should be preferred when possible.
 */
void
smgrdosyncall(SMgrRelation *rels, int nrels)
{
	int			i = 0;
	ForkNumber	forknum;

	if (nrels == 0)
		return;

	FlushRelationsAllBuffers(rels, nrels);

	/*
	 * Sync the physical file(s).
	 */
	for (i = 0; i < nrels; i++)
	{
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		{
			if ((*rels[i]->smgr).smgr_exists(rels[i], forknum))
				(*rels[i]->smgr).smgr_immedsync(rels[i], forknum);
		}
	}
}

/*
 *	smgrdounlinkall() -- Immediately unlink all forks of all given relations
 *
 *		All forks of all given relations are removed from the store.  This
 *		should not be used during transactional operations, since it can't be
 *		undone.
 *
 *		If isRedo is true, it is okay for the underlying file(s) to be gone
 *		already.
 */
void
smgrdounlinkall(SMgrRelation *rels, int nrels, bool isRedo)
{
	int			i = 0;
	RelFileNodeBackend *rnodes;
	ForkNumber	forknum;

	if (nrels == 0)
		return;

	/*
	 * Get rid of any remaining buffers for the relations.  bufmgr will just
	 * drop them without bothering to write the contents.
	 */
	DropRelFileNodesAllBuffers(rels, nrels);

	/*
	 * create an array which contains all relations to be dropped, and close
	 * each relation's forks at the smgr level while at it
	 */
	rnodes = palloc(sizeof(RelFileNodeBackend) * nrels);
	for (i = 0; i < nrels; i++)
	{
		RelFileNodeBackend rnode = rels[i]->smgr_rnode;

		rnodes[i] = rnode;
		/* Close the forks at smgr level */
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			(*rels[i]->smgr).smgr_close(rels[i], forknum);
	}

	/*
	 * It'd be nice to tell the stats collector to forget them immediately,
	 * too. But we can't because we don't know the OIDs.
	 */

	/*
	 * Send a shared-inval message to force other backends to close any
	 * dangling smgr references they may have for these rels.  We should do
	 * this before starting the actual unlinking, in case we fail partway
	 * through that step.  Note that the sinval messages will eventually come
	 * back to this backend, too, and thereby provide a backstop that we
	 * closed our own smgr rel.
	 */
	for (i = 0; i < nrels; i++)
		CacheInvalidateSmgr(rnodes[i]);

	/*
	 * Delete the physical file(s).
	 *
	 * Note: smgr_unlink must treat deletion failure as a WARNING, not an
	 * ERROR, because we've already decided to commit or abort the current
	 * xact.
	 */

	for (i = 0; i < nrels; i++)
	{
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			(*rels[i]->smgr).smgr_unlink(rnodes[i], forknum, isRedo);
	}

	if (file_unlink_hook)
		for (i = 0; i < nrels; i++)
			(*file_unlink_hook)(rnodes[i]);

	pfree(rnodes);
}


/*
 *	smgrextend() -- Add a new block to a file.
 *
 *		The semantics are nearly the same as smgrwrite(): write at the
 *		specified position.  However, this is to be used for the case of
 *		extending a relation (i.e., blocknum is at or beyond the current
 *		EOF).  Note that we assume writing a block beyond current EOF
 *		causes intervening file space to become filled with zeroes.
 *		failure we clean up by truncating.
 */
void
smgrextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   char *buffer, bool skipFsync)
{
	(*reln->smgr).smgr_extend(reln, forknum, blocknum,
										 buffer, skipFsync);

	/*
	 * Normally we expect this to increase nblocks by one, but if the cached
	 * value isn't as expected, just invalidate it so the next call asks the
	 * kernel.
	 */
	if (reln->smgr_cached_nblocks[forknum] == blocknum)
		reln->smgr_cached_nblocks[forknum] = blocknum + 1;
	else
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;

	if (file_extend_hook)
		(*file_extend_hook)(reln->smgr_rnode);
}

/*
 *	smgrprefetch() -- Initiate asynchronous read of the specified block of a relation.
 *
 *		In recovery only, this can return false to indicate that a file
 *		doesn't	exist (presumably it has been dropped by a later WAL
 *		record).
 */
bool
smgrprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
	return (*reln->smgr).smgr_prefetch(reln, forknum, blocknum);
}

/*
 *	smgrread() -- read a particular block from a relation into the supplied
 *				  buffer.
 *
 *		This routine is called from the buffer manager in order to
 *		instantiate pages in the shared buffer cache.  All storage managers
 *		return pages in the format that POSTGRES expects.
 */
void
smgrread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 char *buffer)
{
	(*reln->smgr).smgr_read(reln, forknum, blocknum, buffer);
}

/*
 *	smgrwrite() -- Write the supplied buffer out.
 *
 *		This is to be used only for updating already-existing blocks of a
 *		relation (ie, those before the current EOF).  To extend a relation,
 *		use smgrextend().
 *
 *		This is not a synchronous write -- the block is not necessarily
 *		on disk at return, only dumped out to the kernel.  However,
 *		provisions will be made to fsync the write before the next checkpoint.
 *
 *		skipFsync indicates that the caller will make other provisions to
 *		fsync the relation, so we needn't bother.  Temporary relations also
 *		do not require fsync.
 */
void
smgrwrite(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		  char *buffer, bool skipFsync)
{
	(*reln->smgr).smgr_write(reln, forknum, blocknum,
										buffer, skipFsync);
}


/*
 *	smgrwriteback() -- Trigger kernel writeback for the supplied range of
 *					   blocks.
 */
void
smgrwriteback(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			  BlockNumber nblocks)
{
	(*reln->smgr).smgr_writeback(reln, forknum, blocknum,
											nblocks);
}

/*
 *	smgrnblocks() -- Calculate the number of blocks in the
 *					 supplied relation.
 */
BlockNumber
smgrnblocks(SMgrRelation reln, ForkNumber forknum)
{
	BlockNumber result;

	/* Check and return if we get the cached value for the number of blocks. */
	result = smgrnblocks_cached(reln, forknum);
	if (result != InvalidBlockNumber)
		return result;

	result = (*reln->smgr).smgr_nblocks(reln, forknum);

	reln->smgr_cached_nblocks[forknum] = result;

	return result;
}

/*
 *	smgrnblocks_cached() -- Get the cached number of blocks in the supplied
 *							relation.
 *
 * Returns an InvalidBlockNumber when not in recovery and when the relation
 * fork size is not cached.
 */
BlockNumber
smgrnblocks_cached(SMgrRelation reln, ForkNumber forknum)
{
	/*
	 * For now, we only use cached values in recovery due to lack of a shared
	 * invalidation mechanism for changes in file size.
	 */
	if (InRecovery && reln->smgr_cached_nblocks[forknum] != InvalidBlockNumber)
		return reln->smgr_cached_nblocks[forknum];

	return InvalidBlockNumber;
}

/*
 *	smgrtruncate() -- Truncate the given forks of supplied relation to
 *					  each specified numbers of blocks
 *
 * The truncation is done immediately, so this can't be rolled back.
 *
 * The caller must hold AccessExclusiveLock on the relation, to ensure that
 * other backends receive the smgr invalidation event that this function sends
 * before they access any forks of the relation again.
 */
void
smgrtruncate(SMgrRelation reln, ForkNumber *forknum, int nforks, BlockNumber *nblocks)
{
	int			i;

	/*
	 * Get rid of any buffers for the about-to-be-deleted blocks. bufmgr will
	 * just drop them without bothering to write the contents.
	 */
	DropRelFileNodeBuffers(reln, forknum, nforks, nblocks);

	/*
	 * Send a shared-inval message to force other backends to close any smgr
	 * references they may have for this rel.  This is useful because they
	 * might have open file pointers to segments that got removed, and/or
	 * smgr_targblock variables pointing past the new rel end.  (The inval
	 * message will come back to our backend, too, causing a
	 * probably-unnecessary local smgr flush.  But we don't expect that this
	 * is a performance-critical path.)  As in the unlink code, we want to be
	 * sure the message is sent before we start changing things on-disk.
	 */
	CacheInvalidateSmgr(reln->smgr_rnode);

	/* Do the truncation */
	for (i = 0; i < nforks; i++)
	{
		/* Make the cached size is invalid if we encounter an error. */
		reln->smgr_cached_nblocks[forknum[i]] = InvalidBlockNumber;

		(*reln->smgr).smgr_truncate(reln, forknum[i], nblocks[i]);

		/*
		 * We might as well update the local smgr_cached_nblocks values. The
		 * smgr cache inval message that this function sent will cause other
		 * backends to invalidate their copies of smgr_fsm_nblocks and
		 * smgr_vm_nblocks, and these ones too at the next command boundary.
		 * But these ensure they aren't outright wrong until then.
		 */
		reln->smgr_cached_nblocks[forknum[i]] = nblocks[i];
	}
	if (file_truncate_hook)
		(*file_truncate_hook)(reln->smgr_rnode);
}

/*
 *	smgrimmedsync() -- Force the specified relation to stable storage.
 *
 *		Synchronously force all previous writes to the specified relation
 *		down to disk.
 *
 *		This is useful for building completely new relations (eg, new
 *		indexes).  Instead of incrementally WAL-logging the index build
 *		steps, we can just write completed index pages to disk with smgrwrite
 *		or smgrextend, and then fsync the completed index file before
 *		committing the transaction.  (This is sufficient for purposes of
 *		crash recovery, since it effectively duplicates forcing a checkpoint
 *		for the completed index.  But it is *not* sufficient if one wishes
 *		to use the WAL log for PITR or replication purposes: in that case
 *		we have to make WAL entries as well.)
 *
 *		The preceding writes should specify skipFsync = true to avoid
 *		duplicative fsyncs.
 *
 *		Note that you need to do FlushRelationBuffers() first if there is
 *		any possibility that there are dirty buffers for the relation;
 *		otherwise the sync is not very meaningful.
 */
void
smgrimmedsync(SMgrRelation reln, ForkNumber forknum)
{
	(*reln->smgr).smgr_immedsync(reln, forknum);
}
/*
 * is the relation heap relation?
 */
bool
smgr_is_heap_relation(SMgrRelation reln)
{
    return (reln->smgr == &smgrsw[SMGR_MD]);
}

/*
 * AtEOXact_SMgr
 *
 * This routine is called during transaction commit or abort (it doesn't
 * particularly care which).  All transient SMgrRelation objects are closed.
 *
 * We do this as a compromise between wanting transient SMgrRelations to
 * live awhile (to amortize the costs of blind writes of multiple blocks)
 * and needing them to not live forever (since we're probably holding open
 * a kernel file descriptor for the underlying file, and we need to ensure
 * that gets closed reasonably soon if the file gets deleted).
 */
void
AtEOXact_SMgr(void)
{
	dlist_mutable_iter iter;

	/*
	 * Zap all unowned SMgrRelations.  We rely on smgrclose() to remove each
	 * one from the list.
	 */
	dlist_foreach_modify(iter, &unowned_relns)
	{
		SMgrRelation rel = dlist_container(SMgrRelationData, node,
										   iter.cur);

		Assert(rel->smgr_owner == NULL);

		smgrclose(rel);
	}
}

const char *smgr_get_name(SMgrImpl impl)
{
	if (impl > SMGR_MAX_ID)
		return "invalid";
	return smgrsw[impl].smgr_name ? smgrsw[impl].smgr_name : "unknown";
}
