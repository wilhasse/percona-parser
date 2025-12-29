# Testing Guide

This document describes the test suite for the Percona InnoDB Parser project.

## Quick Start

Run all tests:
```bash
./tests/run_all_tests.sh
```

Run with verbose output:
```bash
./tests/run_all_tests.sh --verbose
```

Run with JSON output:
```bash
./tests/run_all_tests.sh --json
```

## Prerequisites

1. **MySQL/Percona Server running**:
   ```bash
   sudo systemctl start mysql
   ```

2. **Project built**:
   ```bash
   mkdir -p build && cd build
   cmake .. && make -j$(nproc)
   ```

3. **Sudo access** (for MySQL stop/start during some tests)

## Test Suite Overview

The test suite (`tests/run_all_tests.sh`) runs 13 comprehensive tests:

| # | Test Name | Script | Description |
|---|-----------|--------|-------------|
| 1 | COMPRESSED_TABLES | `test_compressed.sh` | ROW_FORMAT=COMPRESSED decompression |
| 2 | ENCRYPTED_TABLES | `test_encrypted.sh` | Percona keyring encryption/decryption |
| 3 | ENCRYPTED_COMPRESSED | `test_encrypted_compressed.sh` | Combined encryption + compression |
| 4 | TYPE_DECODING | `test_types_decode.sh` | DECIMAL, DATE, TIME, ENUM, SET parsing |
| 5 | CHARSET_DECODING | `test_charset_decode.sh` | latin1 + utf8mb4 text decoding |
| 6 | JSON_DECODING | `test_json_decode.sh` | JSON binary column parsing |
| 7 | SECONDARY_INDEX | `test_secondary_index.sh` | Secondary index parsing + index selection |
| 8 | LOB_DECODING | `test_lob_decode.sh` | LONGTEXT/LONGBLOB external pages |
| 9 | ZLOB_DECODING | `test_zlob_decode.sh` | Compressed LOB (ROW_FORMAT=COMPRESSED) |
| 10 | SDI_REBUILD | `test_sdi_rebuild.sh` | Mode 5 full rebuild with SDI for MySQL import |
| 11 | SDI_EXTERNAL | `test_sdi_external.sh` | SDI rebuild with external SDI BLOB pages |
| 12 | CFG_IMPORT | `test_cfg_import.sh` | Generate .cfg from SDI for instant-column import |
| 13 | INDEX_ID_REMAP | `test_index_id_remap.sh` | Remap index IDs for import into target table |

## Individual Test Details

### 1. Compressed Tables (`test_compressed.sh`)

Tests Mode 2 decompression of ROW_FORMAT=COMPRESSED tables.

**What it tests**:
- Decompress 8KB compressed pages to 16KB
- Verify page checksums with `innochecksum`
- Validate readable data in decompressed pages

**Expected result**: Decompression succeeds, file expands from ~64KB to ~128KB.

### 2. Encrypted Tables (`test_encrypted.sh`)

Tests Mode 1 decryption with Percona keyring.

**What it tests**:
- Decrypt AES-256-CBC encrypted tablespace
- Extract keyring metadata from .ibd header
- Verify decrypted data is readable

**Skipped if**: Encryption not configured on MySQL server.

### 3. Encrypted + Compressed (`test_encrypted_compressed.sh`)

Tests combined decryption and decompression (Mode 1 + Mode 2).

**What it tests**:
- Decrypt encrypted compressed tablespace
- Decompress after decryption
- Validate final output

### 4. Type Decoding (`test_types_decode.sh`)

Tests Mode 3 parsing of various MySQL data types.

**Column types tested**:
- DECIMAL(10,2), DECIMAL(18,4)
- DATE, DATETIME, TIMESTAMP
- TIME
- ENUM, SET
- YEAR
- BIT

**Expected result**: All values parsed correctly to JSONL output.

### 5. Charset Decoding (`test_charset_decode.sh`)

Tests charset-aware string decoding (RES-30).

**What it tests**:
- latin1 encoded strings (accented characters)
- utf8mb4 encoded strings (multi-byte, emoji)
- Correct UTF-8 output regardless of source charset

### 6. JSON Decoding (`test_json_decode.sh`)

Tests MySQL JSON binary format parsing (RES-31).

**What it tests**:
- JSON objects and arrays
- Nested structures
- Various JSON value types (string, number, boolean, null)

### 7. Secondary Index (`test_secondary_index.sh`)

Validates secondary index parsing using `--index`.

**What it tests**:
- Parses a non-PRIMARY index (idx_ab)
- Confirms output matches MySQL ordering for index columns

### 8. LOB Decoding (`test_lob_decode.sh`)

Tests external LOB page parsing for DYNAMIC tables.

**What it tests**:
- LONGTEXT columns with data > 768 bytes
- LONGBLOB columns with binary data
- External page traversal (first page + data pages)

### 9. ZLOB Decoding (`test_zlob_decode.sh`)

Tests compressed LOB parsing for COMPRESSED tables.

**What it tests**:
- ZLOB first page structure
- Compressed external data pages
- zlib decompression of LOB data

### 10. SDI Rebuild (`test_sdi_rebuild.sh`)

Tests Mode 5 experimental full rebuild with `--sdi-json` flag (RES-22).

**What it tests**:
- Rebuild compressed .ibd to uncompressed 16KB pages
- Inject SDI from ibd2sdi JSON
- Verify with `ibd2sdi` and `innochecksum`
- MySQL `IMPORT TABLESPACE` success
- Query imported data

**Expected result**: All 55 test rows recovered and queryable.

### 11. SDI External (`test_sdi_external.sh`)

Validates SDI rebuild when SDI data is stored externally (SDI BLOB pages).

**What it tests**:
- Create a wide compressed table to force external SDI
- Rebuild with `--sdi-json`
- Verify `ibd2sdi` reads the rebuilt file
- Confirm SDI BLOB pages via `innochecksum -S`

### 12. CFG Import (`test_cfg_import.sh`)

Validates `.cfg` generation with instant columns and `IMPORT TABLESPACE`.

**What it tests**:
- Rebuild compressed .ibd to uncompressed pages
- Generate `.cfg` via `--cfg-out`
- Import into an instant-column table

### 13. Index-ID Remap (`test_index_id_remap.sh`)

Validates index-id remapping when importing into a different table.

**What it tests**:
- Extract SDI from source and target tables
- Rebuild with `--target-sdi-json` to remap index IDs
- Import rebuilt tablespace into the target table

## Test Output

### Normal Mode

```
IB_PARSER COMPREHENSIVE TEST SUITE
===========================================

Running: COMPRESSED_TABLES
Log file: tests/logs/COMPRESSED_TABLES_20251229_121500.log
✓ COMPRESSED_TABLES PASSED (15s)

Running: ENCRYPTED_TABLES
...

==========================================
TEST SUITE SUMMARY
==========================================
Total tests run: 13
Passed: 13
Failed: 0
Duration: 180s

✓ ALL TESTS PASSED!
```

### JSON Mode

```json
{
  "suite": "ib_parser",
  "timestamp": "2025-12-29T12:15:00-03:00",
  "duration_seconds": 180,
  "status": "passed",
  "summary": {
    "total": 10,
    "passed": 10,
    "failed": 0
  },
  "tests": [
    {"name":"COMPRESSED_TABLES","status":"passed","duration_seconds":15,...},
    ...
  ]
}
```

## Log Files

Test logs are saved to `tests/logs/`:
```
tests/logs/
├── COMPRESSED_TABLES_20251229_121500.log
├── ENCRYPTED_TABLES_20251229_121515.log
├── ...
└── SDI_REBUILD_20251229_121800.log
```

View logs for failed tests:
```bash
cat tests/logs/FAILED_TEST_*.log
```

## Running Individual Tests

```bash
# Run specific test directly
./tests/test_compressed.sh
./tests/test_sdi_rebuild.sh
./tests/test_sdi_external.sh
./tests/test_secondary_index.sh

# Run with environment variable for verbose output
VERBOSE=1 ./tests/test_types_decode.sh
```

## Test Fixtures

Test data files in `tests/`:

| File | Description |
|------|-------------|
| `compressed_test.ibd` | ROW_FORMAT=COMPRESSED table (55 rows) |
| `compressed_test_sdi.json` | SDI JSON for compressed_test |
| `lob_fixture.ibd` | DYNAMIC table with LOB columns |
| `lob_test_sdi.json` | SDI JSON for lob_fixture |
| `zlob_fixture.ibd` | COMPRESSED table with LOB columns |
| `zlob_test_sdi.json` | SDI JSON for zlob_fixture |

## Common Issues

### MySQL Won't Start

```bash
# Clean up test databases
sudo systemctl stop mysql
sudo rm -rf /var/lib/mysql/test_*
sudo systemctl start mysql
```

### Permission Denied

```bash
chmod +x tests/*.sh
```

### Build Not Found

```bash
make -C build -j$(nproc)
```

### Encryption Tests Skipped

Encryption tests require Percona Server with keyring configured. They are automatically skipped if not available.

## Adding New Tests

1. Create script: `tests/test_<feature>.sh`
2. Make executable: `chmod +x tests/test_<feature>.sh`
3. Add to `run_all_tests.sh`:
   ```bash
   # Test N: Description
   TOTAL_TESTS=$((TOTAL_TESTS + 1))
   if run_test "TEST_NAME" "$SCRIPT_DIR/test_<feature>.sh"; then
       PASSED_TESTS=$((PASSED_TESTS + 1))
   else
       FAILED_TESTS=$((FAILED_TESTS + 1))
   fi
   ```
4. Update header list in `run_all_tests.sh`

### Test Script Template

```bash
#!/bin/bash
set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

echo "Testing <feature>..."

# Setup
mysql -uroot -e "DROP DATABASE IF EXISTS test_db;"
mysql -uroot -e "CREATE DATABASE test_db;"

# Test logic
./build/ib_parser ...

# Verify
if [ condition ]; then
    echo -e "${GREEN}✓ Test passed${NC}"
    exit 0
else
    echo -e "${RED}✗ Test failed${NC}"
    exit 1
fi
```

## CI Integration

For CI pipelines, use JSON output:
```bash
./tests/run_all_tests.sh --json > test_results.json
if jq -e '.status == "passed"' test_results.json > /dev/null; then
    echo "Tests passed"
    exit 0
else
    echo "Tests failed"
    exit 1
fi
```
