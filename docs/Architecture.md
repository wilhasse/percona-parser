# Architecture and Source Code Overview

This document provides a detailed explanation of the Percona InnoDB Parser's architecture and source code organization.

## Project Structure

```
percona-parser/
├── src/                    # Source files
│   ├── ib_parser.cc       # Main entry point
│   ├── decompress.cc      # Decompression logic
│   ├── decrypt.cc         # Decryption routines
│   ├── parser.cc          # Page parsing
│   ├── ibd_enc_reader.cc  # Encryption info reader
│   └── ...
├── lib/                    # Header files
│   ├── decompress.h
│   ├── decrypt.h
│   ├── parser.h
│   └── ...
├── build/                  # Build output
├── tests/                  # Test scripts
├── examples/               # Usage examples
└── docs/                   # Documentation
```

## Core Components

### Main Entry Point (`ib_parser.cc`)

The main executable that unifies all functionality. It provides four operational modes:

1. **Mode 1**: Decrypt only
2. **Mode 2**: Decompress only  
3. **Mode 3**: Parse only (with table definition)
4. **Mode 4**: Decrypt then decompress

The main function dispatches to helper routines:
- `do_decrypt_main()` - Handles decryption workflow
- `do_decompress_main()` - Handles decompression workflow
- `do_decrypt_then_decompress_main()` - Combined operation
- Each routine includes page-by-page loops calling the appropriate processing functions

### Decompression Module

#### `decompress.cc` / `decompress.h`
Implements logic for decompressing ROW_FORMAT=COMPRESSED tables:

- **`decompress_page_inplace()`**: Decompresses a single page or copies if not compressed
- **`decompress_ibd()`**: Main routine that reads each page from input and writes uncompressed pages to output
- **Page size detection**: Determines physical and logical page sizes from FSP header
- **Compression handling**: Uses zlib for actual decompression of INDEX pages
- **Mixed output**: INDEX pages expanded to logical size, metadata pages kept at physical size

Key characteristics:
- Preserves page metadata during decompression
- Handles both compressed and uncompressed pages appropriately
- Results in files with mixed page sizes (by design)

### Decryption Module

#### `decrypt.cc` / `decrypt.h`
Contains routines for Percona Server encryption handling:

- **`get_master_key()`**: Loads and de-obfuscates master key from keyring using `MyKeyringLookup`
- **`read_tablespace_key_iv()`**: Extracts tablespace key and IV from .ibd header
- **`decrypt_page_inplace()`**: Performs AES-based page decryption on uncompressed data
- **`decrypt_ibd_file()`**: Iterates over pages and decrypts them to destination path

The decryption process:
1. Reads master key from keyring file
2. Extracts tablespace-specific key from file header
3. Decrypts each page using AES encryption
4. Verifies checksums for data integrity

### Parsing Module

#### `parser.cc` / `parser.h`
Handles InnoDB page structure parsing and record extraction:

- **Table definition loading**: Reads table schema from JSON files
- **Primary index discovery**: Identifies the primary index ID
- **Page iteration**: Processes pages sequentially
- **Record extraction**: Prints record contents in readable format

Relies on:
- `tables_dict.h` structures for table definitions
- Helper functions in `undrop_for_innodb.cc` for record parsing

#### `undrop_for_innodb.cc` / `undrop_for_innodb.h`
Adapts record parsing routines from the "undrop-for-innodb" project:

- **`check_for_a_record()`**: Validates record structure
- **`process_ibrec()`**: Outputs table rows in simple format
- **Record format handling**: Supports various InnoDB record formats

#### `tables_dict.cc` / `tables_dict.h`
Initializes and manages table definition arrays:
- Field definitions
- Data types
- Column metadata
- Index structures

### Encryption Support

#### `ibd_enc_reader.cc` / `ibd_enc_reader.h`
Handles encrypted header interpretation:

- **`Tablespace_key_iv` struct**: Stores encryption metadata
- **`decode_ibd_encryption_info()`**: Interprets encrypted header blob
- **Hex dump utilities**: For debugging encrypted data
- **Version handling**: Supports different encryption format versions

#### `my_keyring_lookup.cc` / `my_keyring_lookup.h`
Implements keyring access:

- **`MyKeyringLookup` class**: Helper to fetch master keys from `Keys_container`
- **Key de-obfuscation**: Handles Percona's key obfuscation
- **Keyring file parsing**: Reads and interprets keyring format

#### `keyring_stubs.cc`
Provides lightweight stand-ins for MySQL server functions:
- Allows keyring code to link correctly without full server
- Implements minimal required MySQL API functions
- Reduces binary size and dependencies

### Utility Components

#### `mysql_crc32c.cc` / `mysql_crc32c.h`
Software implementation of CRC32C algorithm:
- Used for verifying encryption info checksums
- Page checksum validation
- Data integrity verification

## Library API (`ibd_reader_api.cc`)

The C API wrapper that exposes functionality as a shared library:

```c
// Core functions
ibd_init()                    // Initialize library
ibd_cleanup()                 // Cleanup resources
ibd_reader_create()           // Create reader instance
ibd_reader_destroy()          // Destroy reader instance

// Operations
ibd_decompress_file()         // Decompress a file
ibd_decrypt_file()            // Decrypt a file
ibd_decrypt_and_decompress()  // Combined operation

// Configuration
ibd_reader_set_debug()        // Enable debug output
ibd_reader_get_error()        // Get last error message
```

## Data Flow

### Decompression Flow
```
Input .ibd file
    ↓
Read FSP header (determine page sizes)
    ↓
For each page:
    ├─ If INDEX page (type 17855)
    │     └─ Decompress with zlib (8KB → 16KB)
    └─ If metadata page
          └─ Copy as-is (keep at 8KB)
    ↓
Write to output file (mixed page sizes)
```

### Decryption Flow
```
Keyring file + Master key ID
    ↓
Load and de-obfuscate master key
    ↓
Read encryption info from .ibd header
    ↓
Extract tablespace key and IV
    ↓
For each page:
    └─ Decrypt with AES
    ↓
Write decrypted pages to output
```

### Combined Flow (Decrypt + Decompress)
```
Encrypted compressed .ibd
    ↓
Decrypt all pages first
    ↓
Decompress INDEX pages
    ↓
Output unencrypted, uncompressed file
```

## Page Types and Handling

| Page Type | Value | Compressed Handling | Description |
|-----------|-------|-------------------|-------------|
| FIL_PAGE_TYPE_ALLOCATED | 0 | Keep at physical size | Freshly allocated |
| FIL_PAGE_UNDO_LOG | 2 | Keep at physical size | Undo log |
| FIL_PAGE_INODE | 3 | Keep at physical size | Index node |
| FIL_PAGE_IBUF_FREE_LIST | 4 | Keep at physical size | Insert buffer free list |
| FIL_PAGE_TYPE_FSP_HDR | 8 | Keep at physical size | File space header |
| FIL_PAGE_INDEX | 17855 | Decompress to logical size | B-tree node (INDEX) |

## Memory Management

### Buffer Allocation
- Page buffers allocated per operation
- Reused across pages when possible
- Freed after operation completion

### File I/O
- Sequential reading for optimal performance
- Page-aligned writes
- Buffered I/O with configurable buffer sizes

## Error Handling

### Error Codes
```c
typedef enum {
    IBD_SUCCESS = 0,
    IBD_ERROR_INVALID_ARGS = 1,
    IBD_ERROR_FILE_NOT_FOUND = 2,
    IBD_ERROR_CANNOT_OPEN_FILE = 3,
    IBD_ERROR_CANNOT_READ_FILE = 4,
    IBD_ERROR_CANNOT_WRITE_FILE = 5,
    IBD_ERROR_INVALID_PAGE = 6,
    IBD_ERROR_DECOMPRESSION_FAILED = 7,
    IBD_ERROR_DECRYPTION_FAILED = 8,
    IBD_ERROR_OUT_OF_MEMORY = 9
} ibd_result_t;
```

### Error Propagation
- Functions return error codes
- Error messages stored in reader context
- Retrieved via `ibd_reader_get_error()`

## Build System Integration

### CMake Configuration
The `CMakeLists.txt` handles:
- Finding Percona Server libraries
- Linking required components
- Building both library and executable
- Installation rules

### Dependencies
Required Percona Server libraries:
- `libmysys.a` - MySQL system library
- `libstrings.a` - String handling
- `libinnodb_zipdecompress.a` - Compression support
- `libkeyring_file.a` - Keyring functionality

## Testing Infrastructure

### Test Scripts
- `test_compressed.sh` - Tests compression/decompression
- `test_import_only.sh` - Tests MySQL import (expects failure)

### Test Data
Located in `tests/ibd_files/`:
- Compressed table samples
- Encrypted file samples
- Test keyring files

## Known Limitations

### Decompression
1. **Metadata Retention**: Decompressed files retain COMPRESSED row format in metadata
2. **Mixed Page Sizes**: Output has INDEX pages at 16KB, metadata at 8KB
3. **Import Failure**: Cannot import decompressed files back to MySQL

### Current Implementation
- Single-threaded processing
- Sequential page handling
- Memory usage proportional to file size

## Future Enhancements

Potential improvements:
- Parallel page processing
- Streaming decompression
- Metadata correction for reimport
- Additional compression algorithms
- Direct MySQL integration