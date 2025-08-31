# Building the InnoDB Reader Library

This guide covers building the InnoDB Reader library and executable from source.

## Prerequisites

### Required Software
- CMake 3.10 or higher
- C++17 compatible compiler (GCC 7+, Clang 5+)
- Make or Ninja build system
- Git

### Required Libraries
- OpenSSL development files
- zlib development files (for decompression support)
- pthread library (usually included with system)

### Percona Server Build
The project requires a built Percona Server source tree. The directory structure must be:

```
parent_directory/
├── percona-server/       # Percona Server source
│   ├── build/           # Built binaries and libraries
│   └── ...
└── percona-parser/      # This project
    └── ...
```

## Building Percona Server

1. **Clone Percona Server:**
```bash
git clone https://github.com/percona/percona-server.git
cd percona-server
git checkout 8.0  # Or your desired version
```

2. **Configure and build:**
```bash
mkdir build && cd build
cmake .. \
    -DWITH_BOOST=/path/to/boost \
    -DDOWNLOAD_BOOST=1 \
    -DWITH_BOOST_VERSION=1.77.0 \
    -DWITHOUT_ROCKSDB=1 \
    -DWITHOUT_TOKUDB=1
make -j$(nproc)
```

Note: The full build takes significant time (30-60 minutes). Only the core libraries are needed for this project.

## Building InnoDB Reader

### Quick Build

```bash
# Ensure you're in the correct directory structure
cd /path/to/parent_directory

# Clone the repository (if not already done)
git clone https://github.com/yourusername/percona-parser.git
cd percona-parser

# Install RapidJSON (required for parsing)
cd ~
git clone https://github.com/Tencent/rapidjson.git
cd -

# Build everything (library + executable)
mkdir build && cd build
cmake ..
make -j4
```

### Build Options

The CMake configuration supports several options:

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_EXECUTABLE` | ON | Build the ib_parser command-line tool |
| `BUILD_SHARED_LIB` | ON | Build shared library (.so) |
| `BUILD_STATIC_LIB` | OFF | Build static library (.a) |
| `CMAKE_BUILD_TYPE` | Release | Build type (Debug/Release) |
| `CMAKE_INSTALL_PREFIX` | /usr/local | Installation directory |

### Build Configurations

**Build only the library:**
```bash
cmake .. -DBUILD_EXECUTABLE=OFF
make
```

**Build only the executable:**
```bash
cmake .. -DBUILD_SHARED_LIB=OFF -DBUILD_STATIC_LIB=OFF
make ib_parser
```

**Build with static library:**
```bash
cmake .. -DBUILD_STATIC_LIB=ON
make
```

**Debug build (with verbose output):**
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
make
```

**Custom installation prefix:**
```bash
cmake .. -DCMAKE_INSTALL_PREFIX=/opt/ibd-reader
make
sudo make install
```

### Output Files

After successful build:

| File | Size | Description |
|------|------|-------------|
| `build/ib_parser` | ~8MB | Command-line executable |
| `build/libibd_reader.so` | ~8MB | Shared library |
| `build/libibd_reader.a` | ~25MB | Static library (if built) |

## Installation

### System-wide Installation

```bash
sudo make install
```

This installs:
- Library to `/usr/local/lib/`
- Headers to `/usr/local/include/`
- Executable to `/usr/local/bin/`

After installation, update the library cache:
```bash
sudo ldconfig
```

### Local Installation

```bash
make install DESTDIR=~/ibd-reader
```

### Using pkg-config

After installation, you can use pkg-config:

```bash
# Compile flags
pkg-config --cflags ibd-reader

# Link flags
pkg-config --libs ibd-reader

# Example compilation
gcc myprogram.c $(pkg-config --cflags --libs ibd-reader) -o myprogram
```

## Building Examples

### C Example

```bash
cd examples/c
gcc -o example example.c \
    -I../../lib \
    -L../../build -libd_reader \
    -Wl,-rpath,../../build
./example
```

### Go Example

```bash
cd examples/go
go build

# Run with library path
LD_LIBRARY_PATH=../../build ./ibd-reader-example -i input.ibd -o output.ibd

# Or use the wrapper script
./run_example.sh -i input.ibd -o output.ibd
```

## Troubleshooting

### Common Build Issues

**CMake cannot find Percona Server:**
```bash
cmake .. \
    -DMYSQL_SOURCE_DIR=/absolute/path/to/percona-server \
    -DMYSQL_BUILD_DIR=/absolute/path/to/percona-server/build
```

**Missing RapidJSON:**
```bash
# Install RapidJSON first
cd ~
git clone https://github.com/Tencent/rapidjson.git

# Or specify the path
cmake .. -DRAPIDJSON_INCLUDE_DIR=/path/to/rapidjson/include
```

**Linking errors:**
Ensure Percona Server is fully built:
```bash
cd ../percona-server/build
make mysys
make strings  
make innodb_zipdecompress
make keyring_file
```

**Missing dependencies on Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    libssl-dev \
    zlib1g-dev \
    pkg-config
```

**Missing dependencies on RHEL/CentOS/Rocky:**
```bash
sudo yum install -y \
    gcc-c++ \
    cmake3 \
    openssl-devel \
    zlib-devel \
    pkgconfig
```

**Missing dependencies on macOS:**
```bash
brew install cmake openssl zlib pkg-config
```

### Build Verification

**Check library symbols:**
```bash
nm -D build/libibd_reader.so | grep ibd_
```

**Check library dependencies:**
```bash
ldd build/libibd_reader.so
```

**Test the executable:**
```bash
# Show help
./build/ib_parser

# Test decompression (mode 2)
./build/ib_parser 2 test_compressed.ibd output.ibd
```

**Run included tests:**
```bash
# Ensure MySQL is running
sudo systemctl start mysql

# Run test scripts
./tests/test_compressed.sh
./tests/test_import_only.sh
```

## Advanced Build Options

### Custom Compiler

```bash
CC=clang CXX=clang++ cmake ..
make
```

### Memory Sanitizers (Debug)

```bash
cmake .. \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer"
make
```

### Static Linking Everything

```bash
cmake .. \
    -DBUILD_STATIC_LIB=ON \
    -DBUILD_SHARED_LIB=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    -DCMAKE_FIND_LIBRARY_SUFFIXES=".a"
make
```

### Optimized Release Build

```bash
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O3 -march=native -flto" \
    -DCMAKE_EXE_LINKER_FLAGS="-flto"
make
```

### Cross-Compilation

**For ARM64:**
```bash
cmake .. \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
    -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
make
```

## Docker Build

Create a Dockerfile:

```dockerfile
FROM ubuntu:22.04

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    zlib1g-dev \
    wget

# Install Boost
RUN wget https://boostorg.jfrog.io/artifactory/main/release/1.77.0/source/boost_1_77_0.tar.gz && \
    tar xzf boost_1_77_0.tar.gz && \
    rm boost_1_77_0.tar.gz

# Build minimal Percona Server libraries
RUN git clone --depth 1 --branch 8.0 https://github.com/percona/percona-server.git /percona-server
WORKDIR /percona-server
RUN mkdir build && cd build && \
    cmake .. -DDOWNLOAD_BOOST=0 -DWITH_BOOST=/boost_1_77_0 \
             -DWITHOUT_ROCKSDB=1 -DWITHOUT_TOKUDB=1 && \
    make -j$(nproc) mysys strings innodb_zipdecompress keyring_file

# Install RapidJSON
RUN git clone https://github.com/Tencent/rapidjson.git /rapidjson

# Build InnoDB Reader
COPY . /percona-parser
WORKDIR /percona-parser
RUN mkdir build && cd build && \
    cmake .. -DRAPIDJSON_INCLUDE_DIR=/rapidjson/include && \
    make -j$(nproc)

# Set up runtime
ENV LD_LIBRARY_PATH=/percona-parser/build
WORKDIR /data
ENTRYPOINT ["/percona-parser/build/ib_parser"]
```

Build and run:
```bash
docker build -t ibd-reader .

# Decompress a file
docker run --rm -v $(pwd):/data ibd-reader 2 /data/compressed.ibd /data/output.ibd

# Decrypt a file
docker run --rm -v $(pwd):/data ibd-reader 1 1 "uuid" /data/keyring /data/encrypted.ibd /data/decrypted.ibd
```

## Continuous Integration

### GitHub Actions Example

Create `.github/workflows/build.yml`:

```yaml
name: Build and Test

on:
  push:
    branches: [ main ]
  pull_request:
    branches: [ main ]

jobs:
  build:
    runs-on: ubuntu-latest
    
    steps:
    - uses: actions/checkout@v3
    
    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y cmake libssl-dev zlib1g-dev build-essential git
    
    - name: Install RapidJSON
      run: |
        git clone https://github.com/Tencent/rapidjson.git ~/rapidjson
    
    - name: Checkout and build Percona Server libraries
      run: |
        git clone --depth 1 --branch 8.0 https://github.com/percona/percona-server.git ../percona-server
        cd ../percona-server
        mkdir build && cd build
        cmake .. -DDOWNLOAD_BOOST=1 -DWITH_BOOST_VERSION=1.77.0 \
                 -DWITHOUT_ROCKSDB=1 -DWITHOUT_TOKUDB=1
        make -j2 mysys strings innodb_zipdecompress keyring_file
    
    - name: Build InnoDB Reader
      run: |
        mkdir build && cd build
        cmake .. -DRAPIDJSON_INCLUDE_DIR=~/rapidjson/include
        make -j2
    
    - name: Run basic tests
      run: |
        ./build/ib_parser || true  # Show help
        ls -la build/
        ldd build/libibd_reader.so || true
```

## Performance Considerations

### Compiler Optimizations

For production builds, use:
```bash
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O3 -DNDEBUG -march=native"
make
```

### Debug vs Release

- **Debug**: Includes symbols, assertions, and verbose output
- **Release**: Optimized for performance, minimal output

### Memory Usage

The decompression process requires:
- Input file size in memory
- Output buffer (potentially 2x input for compressed tables)
- Working buffers for zlib decompression

## Known Build Issues

### Issue: Decompressed files cannot be imported to MySQL
**Status**: This is expected behavior, not a build issue  
**Reason**: Decompressed files retain COMPRESSED metadata in headers  
**Impact**: Files can be decompressed for inspection but not for reimport

### Issue: Mixed page sizes in output
**Status**: By design  
**Reason**: INDEX pages are expanded to logical size, metadata pages kept at physical size  
**Impact**: Output files have varying page sizes

## Support

For build issues:
1. Check the [Troubleshooting](#troubleshooting) section
2. Ensure all prerequisites are installed
3. Verify Percona Server is fully built
4. Check CMake output for specific errors
5. Review test results in `tests/` directory