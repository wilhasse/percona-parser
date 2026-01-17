# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Build (from project root)
mkdir -p build && cd build && cmake .. && make -j4

# Build with specific options
cmake .. -DBUILD_EXECUTABLE=ON -DBUILD_SHARED_LIB=ON -DBUILD_STATIC_LIB=OFF
cmake .. -DCMAKE_BUILD_TYPE=Debug  # Debug build

# Verify build
./build/ib_parser                   # Show usage
ldd build/libibd_reader.so          # Check library dependencies
```

## Testing

```bash
# Run tests (requires MySQL running: sudo systemctl start mysql)
./tests/test_compressed.sh          # Test decompression
./tests/test_import_only.sh         # Test import behavior (expects failure)

# Cleanup test databases if MySQL won't start
sudo rm -rf /var/lib/mysql/test_compression* /var/lib/mysql/test_rowformat
sudo systemctl restart mysql
```

## ib_parser Usage

```bash
# Mode 1: Decrypt only
./build/ib_parser 1 <key_id> <server_uuid> <keyring_file> <input.ibd> <output.ibd>

# Mode 2: Decompress only
./build/ib_parser 2 <input.ibd> <output.ibd>

# Mode 3: Parse with table definition
./build/ib_parser 3 <table.ibd> <table_definition.json> [options]

# Mode 3 options:
#   --index=NAME|ID     Select index by name or numeric ID
#   --list-indexes      List available indexes and exit
#   --format=pipe|csv|jsonl  Output format (default: pipe)
#   --output=PATH       Write output to file instead of stdout
#   --with-meta         Include row metadata (page_no, offset, deleted flag)
#   --lob-max-bytes=N   Maximum LOB bytes to read (default: 4MB)
#   --raw-integers      Skip InnoDB sign-bit decoding (for test files)
#   --skip-xdes         Skip extent descriptor free-page validation
#   --debug             Enable verbose debug output

# Mode 4: Decrypt then decompress
./build/ib_parser 4 <key_id> <server_uuid> <keyring_file> <input.ibd> <output.ibd>
```

## Architecture

**Core modules:**
- `ib_parser.cc` - Main entry point dispatching to modes 1-4
- `decompress.cc/h` - Page decompression using zlib; handles physical→logical size expansion
- `decrypt.cc/h` - AES decryption using keys from Percona keyring
- `parser.cc/h` - InnoDB page parsing and record extraction

**Encryption/keyring support:**
- `ibd_enc_reader.cc/h` - Encrypted header interpretation, tablespace key extraction
- `my_keyring_lookup.cc/h` - Master key loading from `Keys_container`
- `keyring_stubs.cc` - Minimal MySQL API stubs for standalone linking

**Library API:**
- `lib/ibd_reader_api.cc/h` - C API wrapper exposing core functionality as shared library

**Data flow:**
- Decompression: Read FSP header → detect page sizes → INDEX pages expanded (8KB→16KB), metadata kept at physical size
- Decryption: Load master key from keyring → extract tablespace key from .ibd header → AES decrypt each page

## Dependencies

Requires Percona Server source built at `../percona-server/`. Build only required libraries:

```bash
# Install system dependencies (Debian/Ubuntu)
sudo apt-get install -y libkrb5-dev libreadline-dev libncurses-dev \
  libcurl4-openssl-dev libnuma-dev libaio-dev bison

# Clone and build Percona Server (partial build ~15-30 min)
cd /path/to/parent
git clone https://github.com/percona/percona-server.git
cd percona-server
git submodule update --init --recursive
mkdir build && cd build
cmake .. -DDOWNLOAD_BOOST=1 -DWITH_BOOST=../boost -DWITHOUT_ROCKSDB=1 -DWITHOUT_TOKUDB=1
make -j4 mysys strings innodb_zipdecompress lz4_lib perconaserverclient
```

RapidJSON required at `~/rapidjson/include`:
```bash
cd ~ && git clone https://github.com/Tencent/rapidjson.git
```

## Known Limitations

Decompressed files cannot be imported back to MySQL - they retain COMPRESSED metadata while containing uncompressed data (ROW_FORMAT mismatch error 1808 is expected).

## Pre-commit

Build and verify tests pass before committing.
