# Testing Guide

This document describes the test suite for the Percona InnoDB Parser project and how to run tests effectively.

## Test Scripts Overview

The project includes two main test scripts that validate core functionality:

### 1. Compressed Table Test (`test_compressed.sh`)

**Purpose**: Tests the decompression functionality for ROW_FORMAT=COMPRESSED tables.

**Location**: `tests/test_compressed.sh`

**What it does**:
1. Creates a test database with a compressed table (ROW_FORMAT=COMPRESSED, KEY_BLOCK_SIZE=8)
2. Inserts 40 rows of test data
3. Exports the compressed .ibd file
4. Runs `ib_parser` mode 2 to decompress the file
5. Attempts to import the decompressed file back to MySQL (expects failure)
6. Verifies decompression success by:
   - Checking file size expansion (8KB → 16KB for INDEX pages)
   - Examining readable text in decompressed file
   - Confirming metadata pages remain at physical size

**Expected output**:
- File size increase from 57344 to 65536 bytes
- MySQL import failure with ROW_FORMAT mismatch error (expected)
- Visible decompressed data in INDEX pages

### 2. Import Test (`test_import_only.sh`)

**Purpose**: Demonstrates the ROW_FORMAT metadata issue when importing decompressed files.

**Location**: `tests/test_import_only.sh`

**What it does**:
1. Creates a fresh database with an uncompressed table (ROW_FORMAT=DYNAMIC)
2. Discards the tablespace
3. Copies a previously decompressed .ibd file
4. Attempts to import the decompressed tablespace
5. Shows the expected ROW_FORMAT error

**Expected output**:
- ERROR 1808: Schema mismatch (Table has ROW_TYPE_DYNAMIC, file has ROW_TYPE_COMPRESSED)
- This error is expected and demonstrates the metadata retention issue

## Running Tests

### Prerequisites

1. **MySQL/Percona Server must be running**:
   ```bash
   sudo systemctl start mysql
   ```

2. **Build the project first**:
   ```bash
   cd /home/cslog/percona-parser
   mkdir -p build && cd build
   cmake ..
   make -j4
   ```

3. **Ensure proper permissions**:
   ```bash
   chmod +x tests/*.sh
   ```

### Running Individual Tests

**Run compression test**:
```bash
./tests/test_compressed.sh
```

**Run import test**:
```bash
./tests/test_import_only.sh
```

### Running All Tests

Create a simple test runner:
```bash
#!/bin/bash
echo "Running all tests..."
./tests/test_compressed.sh
if [ $? -eq 0 ]; then
    echo "✓ Compression test passed"
else
    echo "✗ Compression test failed"
fi

./tests/test_import_only.sh
if [ $? -eq 0 ]; then
    echo "✓ Import test passed"
else
    echo "✗ Import test failed"
fi
```

## Common Issues and Solutions

### MySQL Won't Start

**Error**: "Multiple files found for the same tablespace ID"

**Solution**:
```bash
# Stop MySQL
sudo systemctl stop mysql

# Clean up test databases
sudo rm -rf /var/lib/mysql/test_compression*
sudo rm -rf /var/lib/mysql/test_rowformat

# Restart MySQL
sudo systemctl start mysql
```

### Test Database Already Exists

**Error**: "Schema directory already exists"

**Solution**:
```bash
# Drop test databases
mysql -u root -e "DROP DATABASE IF EXISTS test_compression;"
mysql -u root -e "DROP DATABASE IF EXISTS test_compression_import;"
mysql -u root -e "DROP DATABASE IF EXISTS test_rowformat;"

# Clean up filesystem
sudo rm -rf /var/lib/mysql/test_compression*
sudo rm -rf /var/lib/mysql/test_rowformat
```

### Permission Denied

**Error**: "Permission denied" when running tests

**Solution**:
```bash
chmod +x tests/*.sh
```

### Build Not Found

**Error**: "./build/ib_parser: No such file or directory"

**Solution**:
```bash
cd /home/cslog/percona-parser
mkdir -p build && cd build
cmake ..
make -j4
```

## Test File Management

Test files are stored in `tests/ibd_files/`:
- `test_compressed_compressed.ibd` - Original compressed table file
- `test_compressed_decompressed.ibd` - Decompressed output file
- `encrypted_test.ibd` - Sample encrypted file for encryption tests
- `decrypted_test.ibd` - Decrypted output file

These files are created automatically by tests and can be safely deleted:
```bash
rm -f tests/ibd_files/test_compressed_*.ibd
```

## Understanding Test Output

### Successful Decompression Indicators

1. **Page processing messages**:
   ```
   [Page 4] Page type: 17855 (FIL_PAGE_INDEX)
   [DEBUG] Decompressing page (type=17855, phys=8192->logical=16384)
   [DEBUG] Successfully decompressed INDEX page
   ```

2. **File size comparison**:
   ```
   Compressed file:   57344 bytes
   Decompressed file: 65536 bytes
   ✓ SUCCESS: File expanded by 8192 bytes after decompression
   ```

3. **Readable data verification**:
   - Compressed file shows limited readable text
   - Decompressed file shows expanded readable content

### Expected Failures

The ROW_FORMAT error is **expected and correct**:
```
ERROR 1808 (HY000): Schema mismatch (Table has ROW_TYPE_DYNAMIC row format, 
.ibd file has ROW_TYPE_COMPRESSED row format.)
```

This demonstrates that while decompression succeeds, the metadata issue prevents direct MySQL import.

## Adding New Tests

To add new test cases:

1. Create a new script in `tests/` directory
2. Follow the naming convention: `test_<feature>.sh`
3. Include cleanup at the beginning and end
4. Document expected vs actual behavior
5. Use exit codes: 0 for success, non-zero for failure

Example template:
```bash
#!/bin/bash
set -e

echo "Testing <feature>..."
echo "================================"

# Cleanup
mysql -uroot -e "DROP DATABASE IF EXISTS test_db;" 2>/dev/null || true

# Test logic here
# ...

# Verify results
if [ condition ]; then
    echo "✓ Test passed"
    exit 0
else
    echo "✗ Test failed"
    exit 1
fi
```

## Continuous Testing

For development, you can watch for changes and auto-run tests:
```bash
while inotifywait -e modify src/*.cc src/*.h; do
    make -C build && ./tests/test_compressed.sh
done
```

## Performance Testing

To measure decompression performance:
```bash
time ./build/ib_parser 2 large_compressed.ibd output.ibd
```

Monitor memory usage:
```bash
/usr/bin/time -v ./build/ib_parser 2 compressed.ibd output.ibd
```