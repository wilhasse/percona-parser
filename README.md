The project builds a standalone tool named **`ib_parser`**. `CMakeLists.txt` defines the build using a C++17 toolchain and combines sources for decryption, decompression and parsing. Key pieces of that configuration are the lists of decrypt-related sources and decompress-related sources, plus the main executable definition, as seen in lines establishing the variable lists and the final `MYSQL_ADD_EXECUTABLE` call

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
