# IB_Parser Test Suite

This directory contains test scripts for validating the ib_parser tool's functionality with compressed, encrypted, and combined table formats.

## Test Status Overview

| Test Script | Status | Description | Requirements |
|------------|--------|-------------|--------------|
| `demo_encryption.sh` | ✅ **Working** | Quick demo of encryption/decryption | MySQL 8.0+ with keyring plugin |
| `test_compressed.sh` | ⚠️ **Partial** | Tests compressed table decompression | MySQL 8.0+ |
| `test_encrypted.sh` | ✅ **Working** | Tests encrypted table decryption | Percona Server 8.0+ with encryption |
| `test_encrypted_compressed.sh` | ⚠️ **Partial** | Tests combined encryption + compression | Percona Server 8.0+ |
| `test_types_decode.sh` | ✅ **Working** | Generates fixture and validates type decoding | MySQL 8.0+ + ibd2sdi |
| `test_lob_decode.sh` | ✅ **Working** | Validates external LOB (TEXT/BLOB) reconstruction | MySQL 8.0+ + ibd2sdi |
| `test_zlob_decode.sh` | ✅ **Working** | Validates compressed LOB (ZLOB) reconstruction | MySQL 8.0+ + ibd2sdi |
| `run_all_tests.sh` | ✅ **Working** | Runs all test scripts sequentially | All of the above |

### Status Legend:
- ✅ **Working**: Test runs successfully and validates functionality
- ⚠️ **Partial**: Processing works but MySQL import fails due to metadata limitations
- ❌ **Not Working**: Test fails or feature not implemented

## Quick Start

### 1. Simple Encryption Demo (5 minutes)
```bash
./demo_encryption.sh
```
Shows before/after comparison of encrypted vs decrypted data.

### 2. Run All Tests
```bash
./run_all_tests.sh
```
Executes all test scenarios and reports results.

## Individual Test Descriptions

### `demo_encryption.sh`
**What it does:**
- Creates an encrypted table with sensitive data
- Exports and decrypts using ib_parser
- Compares encrypted (unreadable) vs decrypted (readable) content
- Visual proof that decryption works

**How to run:**
```bash
./demo_encryption.sh
```

**Expected output:**
- Encrypted file: 0 secrets found (data is encrypted)
- Decrypted file: All secrets visible (decryption successful)

---

### `test_compressed.sh`
**What it does:**
- Creates a compressed table (ROW_FORMAT=COMPRESSED)
- Exports the .ibd file
- Decompresses using ib_parser
- Attempts to import back to MySQL (currently fails due to metadata)
- Verifies decompression via text inspection

**How to run:**
```bash
./test_compressed.sh
```

**Current limitations:**
- Decompression works for data pages
- MySQL import fails due to tablespace ID and metadata mismatches
- Verification done through text inspection instead

---

### `test_encrypted.sh`
**What it does:**
- Enables Percona Server encryption
- Creates encrypted table with test data
- Exports and decrypts using ib_parser with keyring
- Verifies decryption by searching for expected text

**How to run:**
```bash
./test_encrypted.sh
```

**Expected output:**
- Successfully finds master key from keyring
- Decrypted file contains readable text
- Original encrypted file has no readable content

---

### `test_encrypted_compressed.sh`
**What it does:**
- Creates table with both encryption AND compression
- Tests ib_parser's ability to handle combined formats
- First decrypts, then decompresses the data

**How to run:**
```bash
./test_encrypted_compressed.sh
```

**Current status:**
- Decryption step works
- Decompression of already-decrypted data works
- MySQL import fails (same limitation as compressed tables)

---

### `run_all_tests.sh`
**What it does:**
- Runs all test scripts in sequence
- Provides summary of pass/fail results
- Good for regression testing

**How to run:**
```bash
./run_all_tests.sh
```

---

### `test_types_decode.sh`
**What it does:**
- Creates a table with DATE/TIME/DATETIME/TIMESTAMP/YEAR/DECIMAL/ENUM/SET/BIT
- Exports fixture files to `tests/types_test.ibd` and `tests/types_test_sdi.json`
- Parses with `ib_parser` and compares JSONL output to MySQL

**How to run:**
```bash
./test_types_decode.sh
```

**Notes:**
- Requires `python3` for JSON normalization during compare

---

### `test_lob_decode.sh`
**What it does:**
- Creates a table with LONGTEXT and LONGBLOB stored externally
- Exports fixture files to `tests/lob_test.ibd` and `tests/lob_test_sdi.json`
- Parses with `ib_parser --lob-max-bytes` and compares JSONL output to MySQL

**How to run:**
```bash
./test_lob_decode.sh
```

**Notes:**
- Requires `python3` for JSON normalization during compare

---

### `test_zlob_decode.sh`
**What it does:**
- Creates a compressed table (ROW_FORMAT=COMPRESSED, KEY_BLOCK_SIZE=8) with external LOBs
- Exports fixture files to `tests/zlob_test.ibd` and `tests/zlob_test_sdi.json`
- Parses with `ib_parser --lob-max-bytes` and compares JSONL output to MySQL

**How to run:**
```bash
./test_zlob_decode.sh
```

**Notes:**
- Requires `python3` for JSON normalization during compare

## Utility Tools

### `ibd_text_inspector.sh`
Text search and entropy analysis tool for .ibd files.

**Usage:**
```bash
./ibd_text_inspector.sh <ibd_file> [search_term1] [search_term2] ...
```

**Features:**
- Searches for text patterns in .ibd files
- Calculates entropy to detect encryption
- Shows page-by-page results
- Useful for verifying decryption/decompression

**Example:**
```bash
# Check if file contains readable text
./ibd_text_inspector.sh decrypted.ibd "SECRET" "CONFIDENTIAL"

# Analyze file entropy
./ibd_text_inspector.sh encrypted.ibd
```

## Prerequisites

### MySQL/Percona Server Setup
1. MySQL 8.0+ or Percona Server 8.0+
2. Root access without password (or modify scripts)
3. Keyring plugin for encryption tests:
```sql
INSTALL PLUGIN keyring_file SONAME 'keyring_file.so';
```

### Build Requirements
```bash
cd /home/cslog/percona-parser
mkdir build && cd build
cmake ..
make
```

## Test Data Locations

- **MySQL data directory**: `/var/lib/mysql/`
- **Keyring file**: `/var/lib/mysql-keyring/keyring`
- **Test databases created**: 
  - `test_compression`
  - `test_encryption`
  - `encryption_demo`

## Known Limitations

1. **MySQL Import After Processing**:
   - Processed files cannot be imported back to MySQL
   - Tablespace IDs and metadata don't match
   - Verification done through text inspection instead

2. **Compression Support**:
   - Only INDEX page type fully supported for decompression
   - Other page types copied but not fully decompressed

3. **Combined Encryption+Compression**:
   - Requires two-step processing (decrypt then decompress)
   - Same import limitations as compressed tables

## Troubleshooting

### "Can't find master key from keyring"
Install keyring plugin:
```sql
INSTALL PLUGIN keyring_file SONAME 'keyring_file.so';
```

### "Permission denied" errors
Use sudo for MySQL data files:
```bash
sudo ./test_encrypted.sh
```

### "No such key in container"
Try different master_key_id (1 or 2):
```bash
../build/ib_parser 1 2 "$SERVER_UUID" keyring encrypted.ibd decrypted.ibd
```

### Tests hang or timeout
Check MySQL is running:
```bash
sudo systemctl status mysql
sudo systemctl start mysql
```

## Further Documentation

See `ENCRYPTION_TESTING_GUIDE.md` for detailed step-by-step instructions on:
- Setting up encryption from scratch
- Manual testing procedures
- Understanding how encryption/decryption works
- Advanced troubleshooting

## Contributing

When adding new tests:
1. Follow naming convention: `test_<feature>.sh`
2. Include status check and colored output
3. Add cleanup steps
4. Update this README with test description and status
5. Handle both success and failure cases gracefully
