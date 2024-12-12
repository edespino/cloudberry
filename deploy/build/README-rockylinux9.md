# Building Apache Cloudberry (Incubating): A Development Environment Guide

> **Important**: This guide is intended for developers interested in exploring and potentially contributing to Apache Cloudberry. The build environment described here is optimized for development and testing purposes only. Please consult the official Apache Cloudberry documentation for comprehensive production deployment guidelines and critical system configurations, including required kernel parameter settings.

## Who Should Use This Guide?

This guide is designed for:
- Developers interested in contributing to Apache Cloudberry
- PostgreSQL developers wanting to explore Cloudberry's extensions
- Database enthusiasts interested in learning about distributed query processing
- Anyone considering joining the Apache Cloudberry community

The build process described here enables development activities such as:
- Debugging and testing new features
- Exploring the codebase with development tools
- Running test suites and validation checks
- Making and testing code modifications

---

## Before You Begin

If you're new to Apache Cloudberry or PostgreSQL development:
1. Consider building PostgreSQL first to understand the basic workflow
2. Join the project's mailing lists to connect with other developers
3. Review the project's issue tracker to understand current development priorities
4. Be prepared for longer build times and iterative testing as you explore the codebase

---

## Build Process Overview

The build process for Apache Cloudberry (Incubating) closely resembles that of PostgreSQL. If you have previously set up development environments for PostgreSQL, you'll find the steps for Cloudberry very familiar. 

For those new to Cloudberry or PostgreSQL, I recommend starting with a PostgreSQL build first. The PostgreSQL development community has established excellent documentation and tooling to guide you through the process. Familiarizing yourself with PostgreSQL's build process will make transitioning to Cloudberry significantly easier.

## Key Differences Between Cloudberry and PostgreSQL

While the overall process is similar, there are a few additional considerations when working with Cloudberry due to its distributed architecture:

- **Extra Dependencies**: Certain libraries and tools are required, such as `xerces-c` for the Orca query optimizer.
- **Distributed Nature**: Cloudberry builds on PostgreSQL to support massively parallel processing, which introduces extra configuration steps.

Once you are acclimated to PostgreSQL, setting up Cloudberry should feel straightforward. The following sections will guide you through the build and runtime setup process for Cloudberry.

---

## Prerequisites

### Provision a Rocky Linux 9 VM

- Use any platform to create a virtual machine or container:
  - **AWS EC2**: These instructions were validated using the Rocky-9-EC2-Base-9.5-20241118.0.x86_64 AMI, but should work with any Rocky Linux 9 variant.
  - **VirtualBox**: Use the official Rocky Linux 9 ISO or Vagrant boxes.
  - **Docker**: These instructions were validated using `rockylinux/rockylinux:9`, but should work with any Rocky Linux 9 based container.

    ```bash
    docker run -it --shm-size=2gb -h cdw rockylinux/rockylinux:9
    ```
    The hostname 'cdw' (Coordinator Data Warehouse) is just an example of how we started the container for testing.
    To ensure test suites run successfully, you may need to increase the container's shared memory using --shm-size=2gb. Test failures can occur when the Cloudberry cluster lacks sufficient shared memory resources.
  - **Other cloud platforms**: Use an equivalent Rocky Linux 9 image.
- Ensure the VM or container has:
  - Internet connectivity for package installation.
  - SSH or console access for user interaction.
  - Sufficient resources (CPU, memory, and storage) for a development environment.

> **Note**: Specific steps to provision the environment are not covered in this guide as they vary by platform. This guide assumes you have successfully created a VM or container and can log in as the default user (e.g., `rocky` for Rocky Linux on AWS).

### System Requirements

Minimum requirements for development environment:
- CPU: 4 cores recommended (2 cores minimum)
- RAM: 8GB recommended (4GB minimum)
- Storage: 20GB free space recommended
- Network: Broadband internet connection for package downloads

### Install `sudo` (if missing)

If `sudo` is not already installed, run the following command to install it:
```bash
dnf install -y sudo
```

> **Note**: In environments like Docker, the `root` user will be able to use `sudo` without a password prompt once it is installed.

---

### Step 1: Install Required Packages

This step installs essential development tools, libraries, and dependencies required for building Apache Cloudberry.

#### Install Basic System Packages

The following command installs the primary packages required for Cloudberry development:
```bash
sudo dnf install -y apr-devel autoconf bison bzip2 bzip2-devel cmake3 createrepo_c ed flex gcc gcc-c++ git glibc-langpack-en initscripts iproute java-11-openjdk java-11-openjdk-devel krb5-devel less libcurl-devel libevent-devel libuuid-devel libxml2-devel libzstd-devel lz4 lz4-devel m4 nc net-tools openldap-devel openssh-clients openssh-server openssl-devel pam-devel passwd perl perl-Env perl-ExtUtils-Embed perl-Test-Simple perl-core pinentry python3-devel python3-lxml python3-psutil python3-pytest python3-pyyaml readline-devel rpm-build rpm-sign rpmdevtools rsync tar unzip util-linux-ng wget which zlib-devel
```

#### Install CodeReady Builder (CRB) Packages

The CRB repository provides additional development tools and libraries. On Rocky Linux, this repository is disabled by default and must be explicitly enabled.

```bash
sudo dnf install -y --enablerepo=crb libuv-devel libyaml-devel perl-IPC-Run
```

> **Note**: In Red Hat Enterprise Linux (RHEL), this repository is called "PowerTools."

### Step 2: Create and Configure the 'gpadmin' User

To prepare the environment for Apache Cloudberry (Incubating) development, we need to create and configure a dedicated `gpadmin` user.

1. **Create a user 'gpadmin' with matching group, home directory, and bash shell**:
   ```bash
   sudo useradd -U -m -s /bin/bash gpadmin
   ```

2. **Grant password-less sudo access to `gpadmin`**:
   ```bash
   echo 'gpadmin ALL=(ALL) NOPASSWD:ALL' | sudo tee /etc/sudoers.d/90-gpadmin
   ```

3. **Verify the `gpadmin` user setup**:
     ```bash
     sudo -u gpadmin sudo whoami
     ```
     If the output is `root`, the configuration is correct.

#### Setting Up the gpadmin User Environment

#### (Optional) Development Environment Enhancements

Optional steps to enhance gpadmin's development environment with Vim, Tmux, and Oh My Bash configurations

```bash
sudo dnf install -y vim tmux

sudo -u gpadmin bash <<'EOF'
# Set up Vim configuration
wget -nv -q https://gist.githubusercontent.com/simonista/8703722/raw/d08f2b4dc10452b97d3ca15386e9eed457a53c61/.vimrc -O /home/gpadmin/.vimrc

# Set up Tmux configuration
wget -nv -q https://raw.githubusercontent.com/tony/tmux-config/master/.tmux.conf -O /home/gpadmin/.tmux.conf

# Install Oh My Bash
bash -c "$(curl -fsSL https://raw.githubusercontent.com/ohmybash/oh-my-bash/master/tools/install.sh)" --unattended
EOF
```

#### Required Configuration

This script performs three main tasks as the gpadmin user:

- Updates .bashrc to source Cloudberry environment variables
- Sets up SSH key pair for passwordless login (if not already present)
- Configures proper SSH directory permissions for security

The script uses a heredoc (EOF) block to execute multiple commands under the gpadmin user context. This will be used multiple time througout these instructions.

```bash
sudo -u gpadmin bash <<'EOF'
# Add Cloudberry environment setup to .bashrc
echo -e '\n# Add Cloudberry entries
if [ -f /usr/local/cloudberry-db/greenplum_path.sh ]; then
  source /usr/local/cloudberry-db/greenplum_path.sh
fi

# US English with UTF-8 character encoding
export LANG=en_US.UTF-8
' >> /home/gpadmin/.bashrc

# Set up SSH for passwordless access
mkdir -p /home/gpadmin/.ssh
if [ ! -f /home/gpadmin/.ssh/id_rsa ]; then
  ssh-keygen -t rsa -b 2048 -C 'apache-cloudberry-dev' -f /home/gpadmin/.ssh/id_rsa -N ""
fi
cat /home/gpadmin/.ssh/id_rsa.pub >> /home/gpadmin/.ssh/authorized_keys

# Set proper SSH directory permissions
chmod 700 /home/gpadmin/.ssh
chmod 600 /home/gpadmin/.ssh/authorized_keys
chmod 644 /home/gpadmin/.ssh/id_rsa.pub
EOF
```

### Step 3: System Configuration

#### Setting Resource Limits

Database systems like Apache Cloudberry require specific system resource limits to operate efficiently. These limits should be configured for the `gpadmin` user who runs the database processes.

##### Create Resource Limits Configuration

Create user limits configuration file:

```bash
sudo tee /etc/security/limits.d/90-db-limits.conf << 'EOF'
# Core dump file size limits for gpadmin
gpadmin soft core unlimited
gpadmin hard core unlimited
# Open file limits for gpadmin
gpadmin soft nofile 524288
gpadmin hard nofile 524288
# Process limits for gpadmin
gpadmin soft nproc 131072
gpadmin hard nproc 131072
EOF
```

##### Understanding the Limits

The configuration sets three types of resource limits:

1. **Core Dumps** (`core`):
   - Set to `unlimited` to enable complete core dumps
   - Essential for debugging and troubleshooting
   - Both soft and hard limits are unrestricted

2. **Open Files** (`nofile`):
   - Set to `524288` (512K) files
   - Required for handling many concurrent database connections
   - Critical for distributed operations and large workloads

3. **Process Limits** (`nproc`):
   - Set to `131072` (128K) processes
   - Enables parallel query execution
   - Supports Cloudberry's distributed architecture

##### Verify Resource Limits
```bash
# Check current limits
sudo -u gpadmin ulimit -a
```

---

## Software retrieval and compilation

From here on out we execute commands as the `gpadmin` user

```bash
sudo su - gpadmin
```

### Step 1: Download, Build, and Install Apache Xerces-C

Apache Xerces-C is a required dependency for enabling the Orca query optimizer in Cloudberry. The following steps download the source code, verify its integrity, build the library, and install it.

#### Set Variables (Helper)
To streamline the commands and make them reusable, define the following helper variables:

```bash
XERCES_LATEST_RELEASE=3.3.0
XERCES_INSTALL_PREFIX="/usr/local/xerces-c"
```
Note: These variables are used throughout the build process to specify the version of Apache Xerces-C being installed (XERCES_LATEST_RELEASE) and its installation directory (XERCES_INSTALL_PREFIX). This ensures consistency and simplifies the commands.

#### Download and Verify the Source Package
```bash
wget -nv "https://dlcdn.apache.org//xerces/c/3/sources/xerces-c-${XERCES_LATEST_RELEASE}.tar.gz"
echo "$(curl -sL https://dlcdn.apache.org//xerces/c/3/sources/xerces-c-${XERCES_LATEST_RELEASE}.tar.gz.sha256)" | sha256sum -c -
```

**Note**: Ensure the SHA-256 checksum validation passes (output: `xerces-c-3.3.0.tar.gz: OK`). If it fails, do not proceed and verify the source package's integrity.

#### Extract, Configure, Build, Test, and Install
```bash
tar xf "xerces-c-${XERCES_LATEST_RELEASE}.tar.gz"
rm "xerces-c-${XERCES_LATEST_RELEASE}.tar.gz"
cd xerces-c-${XERCES_LATEST_RELEASE}

./configure --prefix="${XERCES_INSTALL_PREFIX}-${XERCES_LATEST_RELEASE}" | tee configure-$(date "+%Y.%m.%d-%H.%M.%S").log
make -j$(nproc) | tee make-$(date "+%Y.%m.%d-%H.%M.%S").log
make check | tee make-check-$(date "+%Y.%m.%d-%H.%M.%S").log
sudo make install | tee make-install-$(date "+%Y.%m.%d-%H.%M.%S").log
sudo ln -s ${XERCES_INSTALL_PREFIX}-${XERCES_LATEST_RELEASE} ${XERCES_INSTALL_PREFIX}
```

**Notes**:
- The `make` command is run in parallel (`-j$(nproc)`) to leverage all available CPU cores for faster builds. The `nproc` command dynamically retrieves the number of cores.
- During `make check`, failures listed as **XFAIL** (expected failures) are acceptable and do not indicate a problem with the build.
- The output of the commands are saved to timestamped log files for future reference or troubleshooting.

### Step 2: Clone the Apache Cloudberry Repository

Clone the source code for Apache Cloudberry into the `gpadmin` user's home directory:

```bash
git clone https://github.com/apache/cloudberry.git ~/cloudberry
cd ~/cloudberry
```

### Step 3: Configure the Build Process

#### Prepare Environment

The build process requires the necessary libraries (e.g., Xerces-C) to be available at the expected locations for configuration and runtime. Prepare the environment using the following commands:

```bash
sudo rm -rf /usr/local/cloudberry-db
sudo chmod a+w /usr/local
mkdir -p /usr/local/cloudberry-db/lib
sudo cp -v /usr/local/xerces-c/lib/libxerces-c.so \
           /usr/local/xerces-c/lib/libxerces-c-3.*.so \
           /usr/local/cloudberry-db/lib
sudo chown -R gpadmin.gpadmin /usr/local/cloudberry-db
```

#### Run `configure`

The `configure` command sets up the build environment for Apache Cloudberry. This configuration includes several development features and extensions.

```bash
cd ~/cloudberry
export LD_LIBRARY_PATH=/usr/local/cloudberry-db/lib:LD_LIBRARY_PATH
./configure --prefix=/usr/local/cloudberry-db \
            --disable-external-fts \
            --enable-debug \
            --enable-cassert \
            --enable-debug-extensions \
            --enable-gpcloud \
            --enable-ic-proxy \
            --enable-mapreduce \
            --enable-orafce \
            --enable-orca \
            --enable-pxf \
            --enable-tap-tests \
            --with-gssapi \
            --with-ldap \
            --with-libxml \
            --with-lz4 \
            --with-openssl \
            --with-pam \
            --with-perl \
            --with-pgport=5432 \
            --with-python \
            --with-pythonsrc-ext \
            --with-ssl=openssl \
            --with-openssl \
            --with-uuid=e2fs \
            --with-includes=/usr/local/xerces-c/include \
            --with-libraries=/usr/local/cloudberry-db/lib | tee ~/cloudberry/configure-$(date "+%Y.%m.%d-%H.%M.%S").log
```

**Note**: The output of the `configure` command is saved to a timestamped log file for future reference or troubleshooting.

### Step 3: Build and Install Apache Cloudberry and Contrib extensions

#### Compilation

```bash
# Use the following command to compile the core components of Apache Cloudberry:
make -j$(nproc) --directory=~/cloudberry | tee ~/cloudberry/make-$(date "+%Y.%m.%d-%H.%M.%S").log

# Compile additional contrib modules, which provide optional features and extensions:
make -j$(nproc) --directory=~/cloudberry/contrib | tee ~/cloudberry/make-contrib-$(date "+%Y.%m.%d-%H.%M.%S").log
```

#### Installation

```bash
# Install the core components to the specified installation directory:
make install --directory=~/cloudberry | tee ~/cloudberry/make-install-$(date "+%Y.%m.%d-%H.%M.%S").log

# Install the contrib modules to the specified installation directory:
make install --directory=~/cloudberry/contrib | tee ~/cloudberry/make-contrib-install-$(date "+%Y.%m.%d-%H.%M.%S").log
```

### Step 4: Verify Installation
After installation, verify the setup with these steps:

#### Check Cloudberry Version
```bash
/usr/local/cloudberry-db/bin/postgres --gp-version
/usr/local/cloudberry-db/bin/postgres --version
```

#### Verify Library Dependencies
```bash
ldd /usr/local/cloudberry-db/bin/postgres
```

#### Check Library Extensions
```bash
ls -al /usr/local/cloudberry-db/share/postgresql/extension
```

#### Check Core Utilities
```bash
ls -l /usr/local/cloudberry-db/bin/
```
Expected output should show critical binaries like postgres, initdb, etc.

---

## Troubleshooting
### Common Issues

1. Configure fails with missing dependencies
   - Verify all required packages are installed
   - Check the configure log file for specific errors
   - Ensure CRB repository is properly enabled

2. Build fails with compilation errors
   - Check make logs for specific errors
   - Ensure sufficient system resources are available
   - Verify Xerces-C installation is correct

3. Library loading issues
   - Verify LD_LIBRARY_PATH includes required directories
   - Check library permissions

For detailed error messages, review the timestamped log files created during the installation process.

---

You have successfully built and installed Apache Cloudberry on Rocky Linux 9. The installation directory is `/usr/local/cloudberry-db`.


## Cloudberry Development Cluster Setup Guide

This guide walks through setting up a Cloudberry demo cluster, and testing basic functionality. The demo cluster includes a coordinator, standby coordinator, and multiple primary/mirror segments all running on a single development host.

### Not reqquired for all environments - Initial Container Setup
Container environments typically don't start the SSH daemon process by default. Since Cloudberry relies heavily on SSH for inter-process communication, we need to initialize and start the SSH server:
```bash
if ! pgrep sshd > /dev/null; then
    echo "SSH daemon not running. Starting it now..."
    sudo ssh-keygen -A
    echo "PasswordAuthentication yes" | sudo tee -a /etc/ssh/sshd_config
    sudo /usr/sbin/sshd
else
    echo "SSH daemon is already running"
fi
```

### SSH Configuration
Cloudberry uses SSH for coordinator-segment communication. The following commands ensure SSH is properly configured for the gpadmin user by adding the host to known_hosts and verifying SSH connectivity:
```bash
ssh-keyscan $(hostname) >> ~/.ssh/known_hosts
ssh $(hostname) date
```

### Environment Setup
Load Cloudberry environment variables that set up paths for binaries, libraries, and other essential components:
```bash
source /usr/local/cloudberry-db/greenplum_path.sh
```

### Create Development Cluster
Create a demo cluster that simulates a full Cloudberry deployment on a single machine. This includes 1 coordinator, 1 standby coordinator, 3 primary segments, and 3 mirror segments:
```bash
make create-demo-cluster --directory=~/cloudberry | tee ~/cloudberry/make-create-demo-cluster-$(date "+%Y.%m.%d-%H.%M.%S").log
```

### Configure Cluster Environment
After cluster creation, verify and load cluster-specific variables that point to the coordinator port and data directory:
```bash
source ~/cloudberry/gpAux/gpdemo/gpdemo-env.sh
```

### Validate Cluster Deployment
Verify the cluster is running correctly with these essential commands:
```bash
# Display detailed cluster state including segment status
gpstate

# Test cluster shutdown and startup
gpstop -a
gpstart -a

# Confirm Cloudberry version and build
psql template1 -c 'SELECT version()'

# View segment configuration showing primary/mirror relationships
psql template1 -c 'SELECT * from gp_segment_configuration'

# Check available PostgreSQL extensions
psql template1 -c 'SELECT * FROM pg_available_extensions'
```

### Extension Testing Example: pg_stat_statements
This example demonstrates how to enable and test the pg_stat_statements extension, which provides statistics about SQL query execution:

```bash
# Create a database for testing
createdb gpadmin

# Enable the extension by adding it to shared libraries
echo "shared_preload_libraries='pg_stat_statements'" >> $COORDINATOR_DATA_DIRECTORY/postgresql.conf

# Restart cluster to load the new library (-r flag means "restart")
gpstop -ar

# Create the extension in the database
psql gpadmin -e -c 'CREATE EXTENSION pg_stat_statements'

# Run test queries to generate statistics
psql gpadmin --echo-queries <<EOF
-- Create a sample table
CREATE TABLE employees (
    id SERIAL PRIMARY KEY,
    name TEXT,
    department TEXT,
    salary NUMERIC
);

-- Insert sample data
INSERT INTO employees (name, department, salary)
VALUES
('Alice', 'HR', 60000),
('Bob', 'Engineering', 80000),
('Charlie', 'Marketing', 70000);

-- Query the data to generate some statistics
SELECT * FROM employees WHERE department = 'Engineering';
SELECT AVG(salary) FROM employees;

-- View most frequently called queries
SELECT query, calls, total_exec_time AS total_time, rows
FROM pg_stat_statements
ORDER BY calls DESC
LIMIT 5;

-- View most time-consuming queries
SELECT query, calls, total_exec_time AS total_time, rows
FROM pg_stat_statements
ORDER BY total_exec_time DESC
LIMIT 5;
EOF
```

### Destroy Development Cluster
To clean up and start fresh, you can destroy the demo cluster:
```bash
make destroy-demo-cluster --directory=~/cloudberry | tee ~/cloudberry/make-create-demo-cluster-$(date "+%Y.%m.%d-%H.%M.%S").log
```
This command removes all cluster data and configuration, allowing you to create a new clean cluster if needed.

## Troubleshooting

### SSH Connection Issues
When running `create-demo-cluster`, the process may hang if SSH host verification hasn't been completed. This typically manifests as a stalled process waiting for user input to verify the host identity. 

This is why we run:
```bash
ssh-keyscan $(hostname) >> ~/.ssh/known_hosts
```
before creating the cluster. This command automatically adds the host's SSH keys to the known_hosts file, preventing interactive prompts during cluster creation.

If you still encounter SSH issues:
1. Verify that the SSH daemon is running
2. Check that the known_hosts file exists and has the correct permissions
3. Test SSH connectivity with `ssh $(hostname) date` before proceeding with cluster creation

## Validate Basic Functionality
Run the installcheck test suite to verify basic functionality. It's recommended to test with both Orca (the query optimizer) enabled and disabled:

```bash
# Run tests with Orca optimizer enabled
PGOPTIONS='-c optimizer=on' make --directory=~/cloudberry installcheck

# Run tests with Orca optimizer disabled
PGOPTIONS='-c optimizer=off' make --directory=~/cloudberry installcheck
```

> **Important**: Even though Orca is the default Cloudberry optimizer, you must explicitly set `optimizer=on` when running installcheck. Without this setting, the `explain` test will fail due to missing the explicit configuration option.

### Test Results
The installcheck target provides a basic test of functionality. During execution, you'll see output like this:

```bash
test tablespace                   ... ok         3236 ms (diff   76 ms)
parallel group (20 tests):  pg_lsn oid txid name char varchar int2 regproc text ...
     boolean                      ... ok          862 ms (diff   56 ms)
     char                         ... ok          419 ms (diff   87 ms)
     explain                      ... FAILED      310 ms (diff  139 ms)
```

At the end, you'll see a summary, for example:
```
========================
 1 of 658 tests failed. 
========================
```

If any tests fail:
- `regression.diffs`: Shows differences between actual and expected results
- `regression.out`: Contains the complete test execution output

The files will be located in `/home/gpadmin/cloudberry/src/test/regress/`.

> **Note**: installcheck is just one of several test schedules available. This guide focuses on basic development environment setup and validation.

### Troubleshooting Test Failures
If a test fails, you may need to examine the log files from various cluster components. You can locate the data directories containing these logs by querying the segment configuration:

```bash
psql -P pager=off template1 -c 'SELECT * from gp_segment_configuration'
```

This shows the complete cluster configuration. The `content = -1` identifies coordinator nodes:
```
 dbid | content | role | preferred_role | mode | status | port | hostname | address |                                  datadir                                   | warehouseid 
------+---------+------+----------------+------+--------+------+----------+---------+----------------------------------------------------------------------------+-------------
    1 |      -1 | p    | p              | n    | u      | 7000 | mdw      | mdw     | /home/gpadmin/cloudberry/gpAux/gpdemo/datadirs/qddir/demoDataDir-1         |           0
    8 |      -1 | m    | m              | s    | u      | 7001 | mdw      | mdw     | /home/gpadmin/cloudberry/gpAux/gpdemo/datadirs/standby                     |           0
    2 |       0 | p    | p              | s    | u      | 7002 | mdw      | mdw     | /home/gpadmin/cloudberry/gpAux/gpdemo/datadirs/dbfast1/demoDataDir0        |           0
    5 |       0 | m    | m              | s    | u      | 7005 | mdw      | mdw     | /home/gpadmin/cloudberry/gpAux/gpdemo/datadirs/dbfast_mirror1/demoDataDir0 |           0
    3 |       1 | p    | p              | s    | u      | 7003 | mdw      | mdw     | /home/gpadmin/cloudberry/gpAux/gpdemo/datadirs/dbfast2/demoDataDir1        |           0
    6 |       1 | m    | m              | s    | u      | 7006 | mdw      | mdw     | /home/gpadmin/cloudberry/gpAux/gpdemo/datadirs/dbfast_mirror2/demoDataDir1 |           0
    4 |       2 | p    | p              | s    | u      | 7004 | mdw      | mdw     | /home/gpadmin/cloudberry/gpAux/gpdemo/datadirs/dbfast3/demoDataDir2        |           0
    7 |       2 | m    | m              | s    | u      | 7007 | mdw      | mdw     | /home/gpadmin/cloudberry/gpAux/gpdemo/datadirs/dbfast_mirror3/demoDataDir2 |           0
```

Each datadir contains log files that can help diagnose test failures. Review the logs in the relevant component's directory based on which test failed.
