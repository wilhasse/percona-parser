# Parsing InnoDB Tablespaces with percona-parser (Lessons Learned)

# Overview

This guide summarizes how we parse MySQL 8 / Percona Server InnoDB .ibd files with percona-parser, what works, what does not, and the key lessons learned while rebuilding compressed tablespaces for re-import.

The focus is single-table tablespaces (one .ibd per table). System tablespace (ibdata1), undo, and temp tablespaces are out of scope.

# Mental Model

* InnoDB stores data in 16KB logical pages. Compressed tables use smaller physical pages on disk (for example 8KB with ROW_FORMAT=COMPRESSED).
* The clustered index (PRIMARY) contains the full row. Parsing clustered index leaf pages is enough to recover rows.
* SDI (Serialized Dictionary Information) stores schema metadata inside the tablespace. Tools like ibd2sdi can extract SDI as JSON.
* Import validation checks both page structure and schema matching. Missing or mismatched schema often triggers "clustered index validation failed".

# Workflow: Extract Rows

## 1) Acquire .ibd

Typical flow to export a table from MySQL:

```bash
mysql -uroot mydb -e "FLUSH TABLES mytable FOR EXPORT;"
# copy the .ibd file from the datadir
mysql -uroot mydb -e "UNLOCK TABLES;"
```

## 2) Extract SDI

```bash
ibd2sdi mytable.ibd > mytable_sdi.json
```

The JSON is used as the schema source for percona-parser.

## 3) Parse rows

```bash
./build/ib_parser 3 mytable.ibd mytable_sdi.json --format=jsonl --output=rows.jsonl
```

Helpful flags:

* `IB_PARSER_DEBUG=1` shows internal columns (DB_TRX_ID, DB_ROLL_PTR).
* `--with-meta` adds page and record offsets to the output.

# Workflow: Compressed and Encrypted

## Compressed tables

* Mode 2 decompresses compressed pages for inspection and parsing.
* Mode 3 can parse directly from compressed .ibd files (it uses page decompression internally for index pages).

```bash
./build/ib_parser 2 compressed.ibd decompressed.ibd
./build/ib_parser 3 compressed.ibd mytable_sdi.json --format=jsonl
```

## Encrypted tables

Use Mode 1 (decrypt) or Mode 4 (decrypt + decompress):

```bash
./build/ib_parser 1 <key_id> <server_uuid> keyring.file encrypted.ibd decrypted.ibd
./build/ib_parser 4 <key_id> <server_uuid> keyring.file enc_comp.ibd output.ibd
```

# Workflow: Rebuild for Import (Option B)

The experimental path to import a compressed table back into MySQL is:


1. Rebuild into 16KB pages and restore SDI from JSON
2. Create a matching table in MySQL
3. DISCARD TABLESPACE
4. Copy the rebuilt .ibd into the datadir
5. IMPORT TABLESPACE

Command:

```bash
./build/ib_parser 5 compressed.ibd rebuilt.ibd --sdi-json=mytable_sdi.json
```

Import steps (example):

```bash
mysql -uroot -e "CREATE DATABASE test_import;"
mysql -uroot test_import -e "CREATE TABLE mytable (...matching schema...) ROW_FORMAT=DYNAMIC;"
mysql -uroot test_import -e "ALTER TABLE mytable DISCARD TABLESPACE;"
# stop mysql, copy rebuilt ibd, chown mysql:mysql
mysql -uroot test_import -e "ALTER TABLE mytable IMPORT TABLESPACE;"
```

# Verification Tools

* `innochecksum` should pass on rebuilt files.
* `ibd2sdi rebuilt.ibd` should extract JSON.
* A simple `SELECT` after import is the final proof.

# Lessons Learned and Gotchas


1. **Schema must match SDI**
   * Import fails if the table definition differs. Even a missing column can trigger clustered index validation errors.
2. **SDI rebuild is required for import**
   * Without SDI, ibd2sdi fails and MySQL import can crash or reject the file.
   * Rebuilding SDI from JSON fixed this in our tests.
3. **Page size and flags matter**
   * Compressed tablespaces store 8KB physical pages. Rebuild must expand to 16KB and clear ZIP_SSIZE in the FSP flags.
4. **Missing .cfg is tolerated but not ideal**
   * MySQL may recompute AUTO_INCREMENT and warn about missing .cfg.
   * Instant / row-version tables can still fail without a .cfg.
5. **SDI record format is special**
   * SDI records store type + id + trx/roll pointers + zlib-compressed JSON.
   * Rebuilding SDI requires correct record headers and directory slots.
6. **External SDI BLOB pages are handled**
   * Rebuild emits SDI BLOB pages when the SDI record does not fit in-page.
7. **Only clustered index rows are parsed**
   * Secondary indexes are not parsed or reconstructed.

# Current Capabilities Summary

* Decrypt, decompress, and parse clustered index leaf rows.
* Decode LOB/ZLOB, JSON binary, charset-aware text, DATETIME.
* Rebuild compressed tablespaces to 16KB with SDI restored (in-page or external).

# Open Work / Next Steps

* Harden .cfg generation for instant/row-version edge cases.
* Add secondary index parsing or reconstruction.
* Improve diagnostics for index-id mismatches during import.

# References in the repo

* `tests/test_sdi_rebuild.sh` (end-to-end rebuild + import)
* `tests/test_sdi_external.sh` (external SDI BLOB rebuild)
* `tests/test_types_decode.sh`, `test_charset_decode.sh`, `test_json_decode.sh`
* `docs/Architecture.md`, `docs/Testing.md`
