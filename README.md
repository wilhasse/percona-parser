# Percona InnoDB Parser

A standalone tool for parsing, decrypting, and decompressing InnoDB files from Percona Server. This tool enables offline processing of InnoDB tablespaces including encrypted and compressed tables.

## Features

- **Decrypt** encrypted InnoDB tables using Percona Server keyring files
- **Decompress** compressed InnoDB tables (ROW_FORMAT=COMPRESSED)
- **Parse** InnoDB page structures and extract records
- **Combined operations** - decrypt and decompress in a single pass
- **Library API** for integration into other applications (C/C++, Go, Python)

## Current Status

### Working Features
- ✅ Decryption of encrypted InnoDB files
- ✅ Decompression of compressed tables (zlib)
- ✅ Page-level parsing and record extraction
- ✅ Combined decrypt+decompress operations
- ✅ Library API with language bindings

### Known Limitations
- ⚠️ Decompressed files cannot be directly imported back to MySQL due to ROW_FORMAT metadata mismatch
- ⚠️ The decompressed file retains COMPRESSED metadata in headers while containing uncompressed data
- ⚠️ Mixed page sizes in decompressed output (INDEX pages at logical size, metadata at physical size)

## Quick Start

### Prerequisites

- Built Percona Server source tree (see [Building Guide](docs/Building.md))
- CMake 3.10+, C++17 compiler
- OpenSSL, zlib development libraries

### Build

```bash
# Clone and build
git clone https://github.com/yourusername/percona-parser.git
cd percona-parser
mkdir build && cd build
cmake ..
make -j4
```

### Usage

```bash
# Decompress a compressed table
./build/ib_parser 2 compressed.ibd decompressed.ibd

# Decrypt an encrypted table
./build/ib_parser 1 <key_id> <server_uuid> keyring.file encrypted.ibd decrypted.ibd

# Parse and extract records
./build/ib_parser 3 table.ibd table_definition.json

# Parse and extract records as JSONL
./build/ib_parser 3 table.ibd table_definition.json --format=jsonl --output=rows.jsonl

# Include page/record metadata and internal columns (debug)
IB_PARSER_DEBUG=1 ./build/ib_parser 3 table.ibd table_definition.json --with-meta

# Decrypt and decompress
./build/ib_parser 4 <key_id> <server_uuid> keyring.file encrypted_compressed.ibd output.ibd
```

### Validation Harness

See `scripts/validate_parse.sh` for a repeatable comparison against MySQL SELECT output (and optional undrop-for-innodb output).

## Library Usage

The project provides a C library (`libibd_reader.so`) for integration into your applications.

### Basic Example

```c
#include "ibd_reader_api.h"

// Initialize and create reader
ibd_init();
ibd_reader_t reader = ibd_reader_create();

// Decompress a file
ibd_decompress_file(reader, "compressed.ibd", "output.ibd");

// Decrypt a file
ibd_decrypt_file(reader, "encrypted.ibd", "decrypted.ibd", 
                "/path/to/keyring", 1, "server-uuid");

// Cleanup
ibd_reader_destroy(reader);
ibd_cleanup();
```

### Language Bindings
- **C/C++**: Native API (see [API Reference](docs/Api_reference.md))
- **Go**: CGO bindings in `examples/go/`
- **Python**: ctypes example in [Examples](docs/Examples.md#python-example)

## Documentation

- **[Building Guide](docs/Building.md)** - Detailed build instructions
- **[Architecture](docs/Architecture.md)** - Source code organization and internals
- **[Library Usage](docs/Library_usage.md)** - Using the library API
- **[API Reference](docs/Api_reference.md)** - Complete API documentation
- **[Examples](docs/Examples.md)** - Code examples in multiple languages
- **[Testing Guide](docs/Testing.md)** - Running and understanding tests

## Testing

Run the included test scripts to verify functionality:

```bash
# Test compressed table decompression
./tests/test_compressed.sh

# Test import behavior (demonstrates limitations)
./tests/test_import_only.sh
```

See [Testing Guide](docs/Testing.md) for detailed test documentation.

## Troubleshooting

### Common Issues

1. **MySQL Import Error**: `ERROR 1808: Schema mismatch` - This is expected. Decompressed files cannot be imported due to metadata retention.

2. **Build Issues**: Ensure Percona Server is fully built first. See [Building Guide](docs/Building.md).

3. **Library Not Found**: Set `LD_LIBRARY_PATH` to the build directory when using the shared library.

## Contributing

When contributing:
1. Build and test all changes locally
2. Run both test scripts to verify functionality
3. Update documentation for any API changes
4. Follow existing code style

## License

This is an independent open-source project under development. It links against Percona Server libraries (GPL v2) and incorporates adapted code from the undrop-for-innodb project.

The project is provided "as is" without warranty of any kind. See LICENSE file for details.

**Note**: This is a personal research/development project and is not affiliated with or endorsed by Percona.
