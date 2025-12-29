# Percona InnoDB Parser

Offline tooling to decrypt, decompress, and parse MySQL 8 / Percona Server
InnoDB single-table tablespaces (.ibd) for recovery and research.

## Capabilities

- Decrypt encrypted .ibd files with Percona keyring metadata.
- Decompress ROW_FORMAT=COMPRESSED pages (zlib).
- Parse clustered index leaf records into pipe, CSV, or JSONL output.
- Decode LOB/ZLOB, JSON binary columns, charset-aware text, and DATETIME.
- Experimental rebuild to 16KB pages with optional SDI rebuild for import.
- C API for integrations (see `lib/`).

## Quick Start

Build:
```bash
mkdir -p build && cd build
cmake ..
make -j4
```

Extract SDI and parse rows:
```bash
ibd2sdi table.ibd > table_sdi.json
./build/ib_parser 3 table.ibd table_sdi.json --format=jsonl --output=rows.jsonl
```

Decompress or rebuild:
```bash
./build/ib_parser 2 compressed.ibd decompressed.ibd
./build/ib_parser 5 compressed.ibd rebuilt.ibd --sdi-json=table_sdi.json
```

## Limitations

- Requires SDI JSON (via `ibd2sdi`) for column definitions.
- Single-table .ibd only (not ibdata1/system, undo, or temp tablespaces).
- MySQL 8+ format; older layouts are not supported.
- Parses clustered index leaf records only; secondary indexes are not parsed.
- SDI rebuild supports in-page SDI; external SDI BLOB/ZBLOB not yet handled.
- Import without a .cfg may fail on instant/row-version tables or schema drift.

## Docs and Tests

- Build and setup: `docs/Building.md`
- Testing guide: `docs/Testing.md`
- SDI rebuild test: `tests/test_sdi_rebuild.sh`

## License

GPL v2 (links against Percona Server sources). This is a research project and
is not affiliated with Percona.
