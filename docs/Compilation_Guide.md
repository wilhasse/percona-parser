# Compilation Guide

This guide provides step-by-step instructions for compiling the Percona InnoDB Parser from scratch on a Debian/Ubuntu system.

## Overview

The project requires:
1. System dependencies (development libraries)
2. RapidJSON (header-only JSON library)
3. Percona Server source code (partial build of required libraries only)
4. percona-parser build

**Estimated time**: 20-40 minutes (depending on CPU cores)

## Directory Structure

After setup, the directory structure should look like:

```
/home/user/
├── rapidjson/                  # RapidJSON headers
│   └── include/
│       └── rapidjson/
└── mysql/                      # Or any parent directory
    ├── percona-server/         # Percona Server source
    │   └── build/              # Percona Server build output
    │       └── archive_output_directory/
    │           ├── libmysys.a
    │           ├── libstrings.a
    │           └── ...
    └── percona-parser/         # This project
        └── build/
            ├── ib_parser
            └── libibd_reader.so
```

## Step 1: Install System Dependencies

Install required development libraries:

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    zlib1g-dev \
    libkrb5-dev \
    libreadline-dev \
    libncurses-dev \
    libcurl4-openssl-dev \
    libnuma-dev \
    libaio-dev \
    bison \
    pkg-config
```

## Step 2: Clone RapidJSON

RapidJSON is a header-only library used for parsing JSON table definitions:

```bash
cd ~
git clone https://github.com/Tencent/rapidjson.git
```

This creates `~/rapidjson/include/rapidjson/` which is referenced by CMakeLists.txt.

## Step 3: Clone Percona Server

Clone the Percona Server repository:

```bash
cd /home/user/mysql    # Choose your preferred location
git clone https://github.com/percona/percona-server.git
cd percona-server
```

Initialize the required git submodules (this is important!):

```bash
git submodule update --init --recursive
```

This downloads:
- `extra/coredumper` - Core dump handling
- `extra/libkmip` - Key management
- Other required submodules

## Step 4: Configure Percona Server Build

Create and enter the build directory:

```bash
mkdir build && cd build
```

Run CMake with options to download Boost and disable unnecessary components:

```bash
cmake .. \
    -DDOWNLOAD_BOOST=1 \
    -DWITH_BOOST=/home/user/mysql/boost \
    -DWITHOUT_ROCKSDB=1 \
    -DWITHOUT_TOKUDB=1
```

**Parameters explained:**
- `-DDOWNLOAD_BOOST=1`: Automatically download Boost 1.77.0
- `-DWITH_BOOST=<path>`: Directory where Boost will be downloaded
- `-DWITHOUT_ROCKSDB=1`: Skip RocksDB storage engine (not needed)
- `-DWITHOUT_TOKUDB=1`: Skip TokuDB storage engine (not needed)

**Note**: The first cmake run will download Boost (~130MB), which takes a few minutes.

## Step 5: Build Required Percona Server Libraries

Build only the libraries needed by percona-parser (not the full server):

```bash
make -j4 mysys strings innodb_zipdecompress lz4_lib perconaserverclient
```

**Targets explained:**
- `mysys`: MySQL system library (memory, threading, I/O)
- `strings`: String handling functions
- `innodb_zipdecompress`: InnoDB compression/decompression
- `lz4_lib`: LZ4 compression library
- `perconaserverclient`: Client library (includes dependencies)

This partial build takes approximately 5-15 minutes with 4 cores.

**Verify the build:**

```bash
ls -la archive_output_directory/*.a
ls -la ../storage/innobase/*.a
```

You should see:
- `libmysys.a` (~2.3 MB)
- `libstrings.a` (~7.9 MB)
- `libperconaserverclient.a` (~14.6 MB)
- `libinnodb_zipdecompress.a` (~1.1 MB)
- And other supporting libraries

## Step 6: Clone percona-parser (if not already done)

```bash
cd /home/user/mysql
git clone https://github.com/yourusername/percona-parser.git
cd percona-parser
```

## Step 7: Configure percona-parser Build

Verify the paths in `CMakeLists.txt` match your setup:

```cmake
set(MYSQL_SOURCE_DIR "/home/user/mysql/percona-server" ...)
set(MYSQL_BUILD_DIR "/home/user/mysql/percona-server/build" ...)
```

If needed, edit these paths or override them via cmake:

```bash
mkdir build && cd build
cmake .. \
    -DMYSQL_SOURCE_DIR=/path/to/percona-server \
    -DMYSQL_BUILD_DIR=/path/to/percona-server/build
```

Or for default paths:

```bash
mkdir build && cd build
cmake ..
```

## Step 8: Build percona-parser

```bash
make -j4
```

This builds:
- `ib_parser` - Command-line executable (~8.4 MB)
- `libibd_reader.so` - Shared library (~8 MB)

## Step 9: Verify the Build

Check that the binaries were created:

```bash
ls -la ib_parser libibd_reader.so*
```

Test the executable:

```bash
./ib_parser
```

Expected output:
```
Usage:
  ib_parser <mode> [decrypt/decompress args...]

Where <mode> is:
  1 = Decrypt only
  2 = Decompress only
  3 = Parser only
  4 = Decrypt then Decompress in a single pass
...
```

Check library dependencies:

```bash
ldd libibd_reader.so
```

## Troubleshooting

### CMake cannot find Percona Server

Override the paths explicitly:

```bash
cmake .. \
    -DMYSQL_SOURCE_DIR=/absolute/path/to/percona-server \
    -DMYSQL_BUILD_DIR=/absolute/path/to/percona-server/build
```

### Missing git submodules

If cmake fails with errors about missing directories like `extra/coredumper`:

```bash
cd /path/to/percona-server
git submodule update --init --recursive
```

### Missing system library errors

If cmake complains about missing libraries:

| Error | Solution |
|-------|----------|
| `Cannot find KERBEROS` | `sudo apt-get install libkrb5-dev` |
| `Cannot find system readline` | `sudo apt-get install libreadline-dev` |
| `Cannot find CURL` | `sudo apt-get install libcurl4-openssl-dev` |
| `Cannot find system libraries` | Install the package mentioned in the error |

### Linker errors during percona-parser build

Ensure all Percona Server libraries were built:

```bash
cd /path/to/percona-server/build
make mysys strings innodb_zipdecompress lz4_lib perconaserverclient
```

### RapidJSON not found

Ensure RapidJSON is cloned at `~/rapidjson` or update the path in CMakeLists.txt:

```cmake
/home/cslog/rapidjson/include   # Change to your path
```

## Quick Reference

Complete build commands in sequence:

```bash
# 1. Install dependencies
sudo apt-get install -y build-essential cmake git libssl-dev zlib1g-dev \
    libkrb5-dev libreadline-dev libncurses-dev libcurl4-openssl-dev \
    libnuma-dev libaio-dev bison pkg-config

# 2. Clone RapidJSON
cd ~ && git clone https://github.com/Tencent/rapidjson.git

# 3. Clone and build Percona Server libraries
cd /home/user/mysql
git clone https://github.com/percona/percona-server.git
cd percona-server
git submodule update --init --recursive
mkdir build && cd build
cmake .. -DDOWNLOAD_BOOST=1 -DWITH_BOOST=../boost -DWITHOUT_ROCKSDB=1 -DWITHOUT_TOKUDB=1
make -j4 mysys strings innodb_zipdecompress lz4_lib perconaserverclient

# 4. Build percona-parser
cd /home/user/mysql/percona-parser
mkdir build && cd build
cmake ..
make -j4

# 5. Verify
./ib_parser
```
