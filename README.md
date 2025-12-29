# Percona InnoDB Parser

Offline tooling to decrypt, decompress, and parse MySQL 8 / Percona Server
InnoDB single-table tablespaces (.ibd) for recovery and research.

## Capabilities

- Decrypt encrypted .ibd files with Percona keyring metadata.
- Decompress ROW_FORMAT=COMPRESSED pages (zlib).
- Parse clustered or secondary index leaf records into pipe, CSV, or JSONL output (`--index`).
- Decode LOB/ZLOB, JSON binary columns, charset-aware text, and DATETIME.
- Experimental rebuild to 16KB pages with SDI rebuild (in-page or external).
- Generate .cfg from SDI for IMPORT TABLESPACE (instant/row-version tables).
- Remap index IDs during rebuild for imports into different target tables.
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
./build/ib_parser 3 table.ibd table_sdi.json --index=idx_ab --format=jsonl
```

Decompress or rebuild:
```bash
./build/ib_parser 2 compressed.ibd decompressed.ibd
./build/ib_parser 5 compressed.ibd rebuilt.ibd --sdi-json=table_sdi.json --cfg-out=table.cfg
./build/ib_parser 5 source.ibd rebuilt.ibd --sdi-json=source_sdi.json \\
  --target-sdi-json=target_sdi.json --cfg-out=target.cfg
./build/ib_parser 5 source.ibd rebuilt.ibd --sdi-json=source_sdi.json \\
  --target-sdi-json=target_sdi.json --index-id-map=index_id.map --cfg-out=target.cfg
./build/ib_parser 5 source.ibd rebuilt.ibd --sdi-json=source_sdi.json \\
  --target-sdi-json=target_sdi.json --use-target-sdi-root --cfg-out=target.cfg
```

## Limitations

- Requires SDI JSON (via `ibd2sdi`) for column definitions.
- Single-table .ibd only (not ibdata1/system, undo, or temp tablespaces).
- MySQL 8+ format; older layouts are not supported.
- Parses one index at a time; default PRIMARY (use `--index` for secondary).
- .cfg generation requires SDI JSON; import without .cfg may fail on instant tables.
- Importing into a different table requires index-id remap via `--target-sdi-json`.
- SDI root mismatches emit warnings; use `--use-target-sdi-root` or `--target-sdi-root`.
- If the target tablespace is not readable, provide `--target-ibd` for SDI root lookup.

## Docs and Tests

- Build and setup: [docs/Building.md](docs/Building.md)
- Testing guide: [docs/Testing.md](docs/Testing.md)
- InnoDB parsing guide: [docs/Parsing-innodb-tablespaces.md](docs/Parsing-innodb-tablespaces.md)
- SDI rebuild test: [tests/test_sdi_rebuild.sh](tests/test_sdi_rebuild.sh)

## License

GPL v2 (links against Percona Server sources). This is a research project and
is not affiliated with Percona.
