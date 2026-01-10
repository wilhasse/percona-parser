# Parsing InnoDB Tablespaces with innodb-parser (Lessons Learned)

# Overview

This guide summarizes how we parse MySQL 8 / Percona Server InnoDB .ibd files with innodb-parser, what works, what does not, and the key lessons learned while rebuilding compressed tablespaces for re-import.

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

The JSON is used as the schema source for innodb-parser.

## 3) Parse rows

```bash
./build/ib_parser 3 mytable.ibd mytable_sdi.json --format=jsonl --output=rows.jsonl
./build/ib_parser 3 mytable.ibd mytable_sdi.json --index=idx_ab --format=jsonl
./build/ib_parser 3 mytable.ibd mytable_sdi.json --list-indexes
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

For imports into a *different* table (index IDs differ), remap using target SDI:

```bash
./build/ib_parser 5 source.ibd rebuilt.ibd \
  --sdi-json=source_sdi.json \
  --target-sdi-json=target_sdi.json \
  --cfg-out=rebuilt.cfg
```

Optional: provide an explicit index-id mapping file (one `old=new` per line):

```bash
./build/ib_parser 5 source.ibd rebuilt.ibd \
  --sdi-json=source_sdi.json \
  --target-sdi-json=target_sdi.json \
  --index-id-map=index_id.map \
  --cfg-out=rebuilt.cfg
```

Dry-run validation (no rebuild) to compare SDI index ids/roots before import:

```bash
./build/ib_parser 5 source.ibd rebuilt.ibd \
  --sdi-json=source_sdi.json \
  --target-sdi-json=target_sdi.json \
  --validate-remap
```

If the target SDI root page differs, `ib_parser` will warn. By default the
source SDI root page is used. To force the target root page (when known), pass:

```bash
./build/ib_parser 5 source.ibd rebuilt.ibd \
  --sdi-json=source_sdi.json \
  --target-sdi-json=target_sdi.json \
  --use-target-sdi-root
```

If the target space_id differs, `ib_parser` will warn and keep the source
space_id unless you opt in. To remap space_id, pass:

```bash
./build/ib_parser 5 source.ibd rebuilt.ibd \
  --sdi-json=source_sdi.json \
  --target-sdi-json=target_sdi.json \
  --use-target-space-id
```

When target SDI JSON contains a relative tablespace path, set `MYSQL_DATADIR`
or `IB_PARSER_DATADIR` so the tool can locate the target .ibd header for root
comparison. You can also pass `--target-ibd=PATH` (pointing to a readable copy)
or supply `--target-sdi-root=N` / `--target-space-id=N` to override manually.

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
7. **Secondary indexes are supported**
   * Use `--index=NAME|ID` to parse a secondary index; default is PRIMARY.
8. **Index IDs must match the target table**
   * When importing into a different table, use `--target-sdi-json` to remap index IDs.

# Current Capabilities Summary

**Decryption & Decompression:**
* Decrypt encrypted tablespaces using Percona keyring (Mode 1, 4).
* Decompress ROW_FORMAT=COMPRESSED tablespaces (Mode 2, 4).

**Parsing (Mode 3):**
* Parse clustered index (PRIMARY) or secondary index leaf records via `--index=NAME|ID`.
* List available indexes with `--list-indexes`.
* Decode all MySQL data types: INT, DECIMAL, DATE, TIME, DATETIME, TIMESTAMP, YEAR, ENUM, SET.
* Decode LOB/ZLOB external references (LONGTEXT, LONGBLOB).
* Decode JSON binary format.
* Charset-aware text decoding (latin1, utf8mb4, etc.).
* Output formats: pipe-separated (default), CSV, JSON Lines.
* Include row metadata (page/offset) with `--with-meta`.

**Rebuild for Import (Mode 5):**
* Rebuild compressed tablespaces to 16KB DYNAMIC format.
* Restore SDI from JSON (in-page or external BLOB pages).
* Remap index IDs using `--target-sdi-json` for cross-table imports.
* Generate `.cfg` files for IMPORT TABLESPACE via `--cfg-out`.
* Validate remap without rebuilding using `--validate-remap` (dry-run diff).
* Detect and warn on SDI root page mismatches; use `--use-target-sdi-root` to override.
* Detect and warn on space_id mismatches; use `--use-target-space-id` to remap.

# References in the repo

* `tests/run_all_tests.sh` (comprehensive test suite)
* `tests/test_sdi_rebuild.sh` (end-to-end rebuild + import)
* `tests/test_sdi_external.sh` (external SDI BLOB rebuild)
* `tests/test_index_id_remap.sh` (target SDI remap + import)
* `tests/test_validate_remap.sh` (dry-run validation with SDI diff)
* `tests/test_cfg_import.sh` (CFG generation for instant columns)
* `tests/test_secondary_index.sh` (secondary index parsing)
* `tests/test_types_decode.sh`, `test_charset_decode.sh`, `test_json_decode.sh`
* `tests/test_lob_decode.sh`, `test_zlob_decode.sh`
* `docs/Architecture.md`, `docs/Testing.md`
