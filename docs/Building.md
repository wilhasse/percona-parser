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
- zlib development files
- pthread library (usually included with system)

### Percona Server Build
The project requires a built Percona Server source tree. The directory structure should be:

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

Note: The full build takes significant time (30-60 minutes). Only the core libraries are needed.

## Building InnoDB Reader

### Quick Build

```bash
# Clone the repository
git clone https://github.com/yourusername/percona-parser.git
cd percona-parser

# Install RapidJSON
cd ~
git clone https://github.com/Tencent/rapidjson.git

# Build everything (library + executable)
cd percona-parser
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
make
```

**Build static library:**
```bash
cmake .. -DBUILD_STATIC_LIB=ON -DBUILD_SHARED_LIB=OFF
make
```

**Debug build:**
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
make
```

**Custom installation prefix:**
```bash
cmake .. -DCMAKE_INSTALL_PREFIX=/opt/ibd-reader
make
make install
```

### Output Files

After successful build:

| File | Size | Description |
|------|------|-------------|
| `ib_parser` | ~8MB | Command-line executable |
| `libibd_reader.so` | ~8MB | Shared library |
| `libibd_reader.a` | ~25MB | Static library (if built) |

## Installation

### System-wide Installation

```bash
sudo make install
```

This installs:
- Library to `/usr/local/lib/`
- Headers to `/usr/local/include/`
- Executable to `/usr/local/bin/`

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
go build example.go
./example
```

## Cross-Compilation

### For ARM64

```bash
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=cmake/arm64-toolchain.cmake \
    -DMYSQL_SOURCE_DIR=/path/to/arm64/percona-server \
    -DMYSQL_BUILD_DIR=/path/to/arm64/percona-server/build
make
```

### For Windows (using MinGW)

```bash
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake \
    -DCMAKE_SYSTEM_NAME=Windows
make
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
cmake .. -DRAPIDJSON_INCLUDE_DIR=/path/to/rapidjson/include
```

**Linking errors:**
Ensure Percona Server is fully built:
```bash
cd ../percona-server/build
make mysys
make strings
make innodb_zipdecompress
```

**Missing dependencies on Ubuntu/Debian:**
```bash
sudo apt-get install \
    build-essential \
    cmake \
    libssl-dev \
    zlib1g-dev
```

**Missing dependencies on RHEL/CentOS:**
```bash
sudo yum install \
    gcc-c++ \
    cmake3 \
    openssl-devel \
    zlib-devel
```

### Verifying the Build

**Check library symbols:**
```bash
nm -D libibd_reader.so | grep ibd_
```

**Check library dependencies:**
```bash
ldd libibd_reader.so
```

**Run tests:**
```bash
./ib_parser 2 test_input.ibd test_output.ibd
```

## Advanced Build Options

### Custom Compiler

```bash
CC=clang CXX=clang++ cmake ..
```

### Sanitizers (Debug)

```bash
cmake .. \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address -fsanitize=undefined"
```

### Static Linking Everything

```bash
cmake .. \
    -DBUILD_STATIC_LIB=ON \
    -DBUILD_SHARED_LIB=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-static"
```

### Optimized Release Build

```bash
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O3 -march=native"
```

## Docker Build

Create a Dockerfile:

```dockerfile
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    zlib1g-dev

# Build Percona Server dependencies
RUN git clone https://github.com/percona/percona-server.git /percona-server
WORKDIR /percona-server
RUN mkdir build && cd build && \
    cmake .. -DDOWNLOAD_BOOST=1 -DWITH_BOOST_VERSION=1.77.0 && \
    make -j$(nproc) mysys strings innodb_zipdecompress

# Build InnoDB Reader
COPY . /ibd-reader
WORKDIR /ibd-reader
RUN mkdir build && cd build && \
    cmake .. && \
    make -j$(nproc)

CMD ["/ibd-reader/build/ib_parser"]
```

Build and run:
```bash
docker build -t ibd-reader .
docker run --rm -v $(pwd):/data ibd-reader \
    /ibd-reader/build/ib_parser 2 /data/input.ibd /data/output.ibd
```

## Continuous Integration

### GitHub Actions Example

```yaml
name: Build

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@v2
    
    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y cmake libssl-dev zlib1g-dev
    
    - name: Build Percona Server libs
      run: |
        git clone https://github.com/percona/percona-server.git ../percona-server
        cd ../percona-server
        mkdir build && cd build
        cmake .. -DDOWNLOAD_BOOST=1
        make -j2 mysys strings innodb_zipdecompress
    
    - name: Build InnoDB Reader
      run: |
        mkdir build && cd build
        cmake ..
        make -j2
    
    - name: Test
      run: |
        ./build/ib_parser --help
```