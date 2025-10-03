# Workflow Debug Shell Guide

This guide explains how to use the interactive debug shell feature in the Apache Cloudberry Build workflow.

## Overview

The workflow includes an interactive tmate session that allows you to SSH into the CI container just before test execution. This is useful for debugging test failures, exploring the environment, or running tests manually.

## Enabling the Debug Shell

### Option 1: Manual Workflow Trigger

1. Go to **Actions** → **Apache Cloudberry Build** → **Run workflow**
2. Select your branch from the dropdown
3. Check **"Enable interactive debug shell"**
4. (Optional) Set **"Debug shell timeout in minutes"** (default: 30, max: 360)
5. Click **"Run workflow"**

### Option 2: PR Label

1. Add the label `ci:debug` to your pull request
2. The debug shell will activate automatically on the next workflow run

## Connecting to the Debug Session

1. Navigate to the workflow run in GitHub Actions
2. Click on the **"Setup debug shell (tmate)"** step
3. Expand the **"Debug Session Information"** section
4. Copy the SSH connection string (format: `ssh xxxxx@xxx.tmate.io`)
5. Connect from your terminal:
   ```bash
   ssh xxxxx@xxx.tmate.io
   ```

## Commands to Execute Once Connected

### Step-by-Step Commands

```bash
# 1. Switch to gpadmin user
su - gpadmin

# 2. Navigate to source directory
cd ${SRC_DIR}

# 3. Source Cloudberry environment
source /usr/local/cloudberry-db/greenplum_path.sh

# 4. Source demo cluster environment
source gpAux/gpdemo/gpdemo-env.sh

# 5. Set your test variables
export PGOPTIONS="-c optimizer=off"
export MAKE_TARGET="installcheck-good"
export MAKE_DIRECTORY="-C src/test/regress"

# 6. Run the test
make ${MAKE_TARGET} ${MAKE_DIRECTORY}
```

### Quick Copy-Paste Version

```bash
su - gpadmin -c "
cd \${SRC_DIR} && \
source /usr/local/cloudberry-db/greenplum_path.sh && \
source gpAux/gpdemo/gpdemo-env.sh && \
export PGOPTIONS='-c optimizer=off' && \
export MAKE_TARGET='installcheck-good' && \
export MAKE_DIRECTORY='-C src/test/regress' && \
make \${MAKE_TARGET} \${MAKE_DIRECTORY}
"
```

## Environment Details

When you connect, the following are already set up:

- ✅ Container: `apache/incubator-cloudberry:cbdb-build-rocky9-latest`
- ✅ Cloudberry RPM installed
- ✅ Demo cluster created and running
- ✅ Environment variables configured
- ✅ `gpadmin` user ready
- ✅ All previous workflow steps completed

## Useful Commands

### Check Cluster Status
```bash
su - gpadmin -c "source /usr/local/cloudberry-db/greenplum_path.sh && gpstate"
```

### View Environment Variables
```bash
env | grep -E '(SRC_DIR|GITHUB|CLOUDBERRY)'
```

### Check Available Tests
```bash
ls -la src/test/
```

## Exiting the Debug Session

To exit the debug session and continue the workflow:

```bash
exit
```

Or simply close your terminal. The workflow will automatically continue after you disconnect.

## Timeout Behavior

- Default timeout: **30 minutes**
- Maximum timeout: **360 minutes** (6 hours)
- After timeout, the session automatically terminates and the workflow continues
- You can set a custom timeout via the workflow dispatch input

## Security Notes

- The tmate session is accessible via the connection string shown in the logs
- Connection details are only visible in GitHub Actions logs (requires GitHub login for public repos)
- The session is not restricted to the workflow actor in the current implementation
- Only share connection details with trusted team members

## Troubleshooting

### Can't connect to tmate session
- Check that the "Setup debug shell (tmate)" step is running (not completed)
- Verify you copied the correct SSH connection string
- Ensure your local SSH client is working

### Session times out too quickly
- Increase the timeout via workflow dispatch input
- Maximum allowed is 360 minutes

### Commands fail as root
- Switch to `gpadmin` user first with `su - gpadmin`
- Most Cloudberry operations require the `gpadmin` user

## Example Workflow

1. Enable debug shell via workflow dispatch
2. Wait for workflow to reach "Setup debug shell (tmate)" step
3. Connect via SSH
4. Run: `su - gpadmin`
5. Run: `cd ${SRC_DIR}`
6. Source environments and run your tests
7. Investigate any failures
8. Type `exit` to continue the workflow
