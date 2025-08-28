# Percona InnoDB Parser

A standalone tool for parsing, decrypting, and decompressing InnoDB files from Percona Server.

## Build Requirements

This project requires a built Percona Server source tree alongside it. The directory structure should be:
```
parent_directory/
├── percona-server/       # Percona Server source code (built)
│   ├── build/           # Build output directory with libraries
│   └── ...
└── percona-parser/      # This project
    └── ...
```

## Building the Project

### Prerequisites

1. **Build Percona Server first:**
   ```bash
   cd percona-server
   mkdir build && cd build
   cmake .. -DWITH_BOOST=/path/to/boost -DDOWNLOAD_BOOST=1 -DWITH_BOOST_VERSION=1.77.0
   make -j$(nproc)
   ```

2. **Install RapidJSON:**
   ```bash
   cd ~
   git clone https://github.com/Tencent/rapidjson.git
   ```

### Building percona-parser

1. **Clone and enter the project:**
   ```bash
   cd percona-parser
   ```

2. **Create build directory and configure:**
   ```bash
   mkdir build && cd build
   cmake ..
   ```

   The CMakeLists.txt automatically detects:
   - Percona Server source at `../percona-server`
   - Percona Server build at `../percona-server/build`
   - RapidJSON at `~/rapidjson/include`

3. **Build options:**
   
   Build everything (default):
   ```bash
   make -j4
   ```
   
   Build only the executable:
   ```bash
   cmake .. -DBUILD_SHARED_LIB=OFF -DBUILD_STATIC_LIB=OFF
   make ib_parser
   ```
   
   Build only the shared library:
   ```bash
   cmake .. -DBUILD_EXECUTABLE=OFF
   make
   ```
   
   Build static library:
   ```bash
   cmake .. -DBUILD_STATIC_LIB=ON
   make
   ```

### Build Outputs

After successful build, you'll have:
- `ib_parser` - Command-line executable (7.9MB)
- `libibd_reader.so` - Shared library for C/C++/Go integration (8MB)
- `libibd_reader.a` - Static library (if built)

### Troubleshooting Build Issues

If you encounter compilation errors:
- Ensure Percona Server is fully built before attempting to build this project
- Check that all required libraries are present in `percona-server/build/archive_output_directory/`
- Verify RapidJSON is installed in the expected location

## Library Usage (Quick Start)

The project provides a C library (`libibd_reader.so`) for integrating InnoDB file operations into your applications.

### Basic Usage

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
- **C/C++**: Native API (see [docs/Api_reference.md](docs/Api_reference.md))
- **Go**: CGO bindings in `examples/go/` (use `run_example.sh` wrapper or set `LD_LIBRARY_PATH`)
- **Python**: ctypes example in [docs/Examples.md](docs/Examples.md#python-example)

**Runtime Note**: When using the shared library, ensure it's accessible at runtime by either:
- Using the provided wrapper scripts (`run_example.sh`)
- Setting `LD_LIBRARY_PATH` to the build directory
- Installing the library system-wide

### Documentation
- **[Library Usage Guide](docs/Library_usage.md)** - Detailed usage instructions
- **[API Reference](docs/Api_reference.md)** - Complete API documentation
- **[Building Guide](docs/Building.md)** - Build from source instructions
- **[Examples](docs/Examples.md)** - Complete code examples

### Quick Example Files
- `examples/c/example.c` - C demonstration
- `examples/go/example.go` - Go demonstration

## Command-Line Usage

After building, the `ib_parser` executable supports four modes:

```bash
# Mode 1: Decrypt only
./ib_parser 1 <master_key_id> <server_uuid> <keyring_file> <ibd_path> <dest_path>

# Mode 2: Decompress only  
./ib_parser 2 <compressed.ibd> <decompressed.ibd>

# Mode 3: Parse only (requires table definition JSON)
./ib_parser 3 <input.ibd> <table_def.json>

# Mode 4: Decrypt then Decompress in a single pass
./ib_parser 4 <master_key_id> <server_uuid> <keyring_file> <encrypted.ibd> <output.ibd>
```

## Project Overview

The project builds a standalone tool named **`ib_parser`** that combines sources for decryption, decompression and parsing

### Decompression

- `decompress.cc` implements logic for reading InnoDB pages and producing uncompressed output. It determines page size from the FSP header and provides `decompress_page_inplace` along with `decompress_ibd`. The `decompress_page_inplace` function decompresses a single page (or simply copies if not compressed).
  The main `decompress_ibd()` routine reads each page from an input file and writes uncompressed pages to an output file.
- `decompress.h` declares these helper functions

### Decryption

- `decrypt.cc` contains routines for key management and page/file decryption.
  `get_master_key()` loads and de-obfuscates a master key from a keyring using `MyKeyringLookup`.
  `read_tablespace_key_iv()` extracts the tablespace key and IV from a `.ibd` header.
  `decrypt_page_inplace()` performs AES-based page decryption on uncompressed data.
  For entire files, `decrypt_ibd_file()` iterates over pages and decrypts them to a destination path.
- `decrypt.h` exposes these functions for other modules

### Keyring / CRC helpers

- `my_keyring_lookup.cc` implements `MyKeyringLookup`, a small helper to fetch a master key from a `Keys_container` in the keyring library, with its interface defined in `my_keyring_lookup.h`.
- `keyring_stubs.cc` provides lightweight stand‑ins for MySQL server functions so the keyring code links correctly.
- `mysql_crc32c.cc` contains a software implementation of the CRC32C algorithm used when verifying encryption info checksums, with declarations in `mysql_crc32c.h`.

### Encryption-info reader

- `ibd_enc_reader.h` defines the `Tablespace_key_iv` struct and `decode_ibd_encryption_info()` for interpreting an encrypted header blob.
- `ibd_enc_reader.cc` implements that decoding and includes utilities such as a hex dump printer.

### Parser & Undrop utilities

- `parser.cc` contains page parsing logic. It can load table definitions from JSON, discover the primary index ID, and iterate over pages to print record contents. It relies on `tables_dict.h` structures and helper functions in `undrop_for_innodb.cc`.
- `tables_dict.cc` initializes table definition arrays for use when parsing records with structures defined in `tables_dict.h`.
- `undrop_for_innodb.cc` adapts record parsing routines from the “undrop-for-innodb” project, providing functions like `check_for_a_record()` and `process_ibrec()` to output table rows in a simple format.
- Header files `parser.h` and `undrop_for_innodb.h` declare these parser-related functions.

### Main entry

- `ib_parser.cc` unifies all functionality. A command‑line mode selects among:
  1. decrypt only,
  2. decompress only,
  3. parse only,
  4. decrypt then decompress.
     Modes are dispatched from `main()` in lines handling the `switch` statement. Earlier in the file are helper routines such as `do_decrypt_main`, `do_decompress_main`, and `do_decrypt_then_decompress_main` to run each workflow, including page-by-page loops that call `decrypt_page_inplace` and `decompress_page_inplace`

### Command-line usage

After building (for example):

```
cmake -B build -S .
cmake --build build -j$(nproc)
```

you can run the executable with different modes:

```
# decrypt only
./ib_parser 1 <master_key_id> <server_uuid> <keyring_file> <ibd_path> <dest_path>   
# decompress only
./ib_parser 2 <in_file.ibd> <out_file>                                             
# parse records
./ib_parser 3 <in_file.ibd> <table_def.json>                                       
# decrypt+decompress
./ib_parser 4 <master_key_id> <server_uuid> <keyring_file> <ibd_path> <dest_path>   
```

This tool is designed for offline processing of InnoDB tablespaces: retrieving encryption keys, decrypting page data, optionally decompressing it, and even parsing records once the page is in plain form. The various modules interact through the shared headers and are linked together into the single `ib_parser` program.
