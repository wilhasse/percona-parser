#!/bin/bash

# Quick test to show the ROW_FORMAT error when importing a decompressed file

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PARSER_DIR="$(dirname "$SCRIPT_DIR")"

echo "Testing import of decompressed file to show ROW_FORMAT error"
echo "=============================================================="

# Clean up completely first
echo "Cleaning up old test data..."
mysql -uroot -e "DROP DATABASE IF EXISTS test_rowformat;" 2>/dev/null || true
sudo systemctl stop mysql
sudo rm -rf /var/lib/mysql/test_rowformat*
sudo systemctl start mysql
sleep 3

# Create fresh database
mysql -uroot -e "CREATE DATABASE test_rowformat;"

# Create an uncompressed table 
echo "Creating uncompressed table..."
mysql -uroot test_rowformat <<EOF
CREATE TABLE test_table (
    id INT PRIMARY KEY AUTO_INCREMENT,
    data VARCHAR(255),
    number INT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ROW_FORMAT=DYNAMIC;
EOF

# Discard tablespace
echo "Discarding tablespace..."
mysql -uroot test_rowformat -e "ALTER TABLE test_table DISCARD TABLESPACE;"

# Copy the previously decompressed file (if it exists)
if [ -f "tests/ibd_files/test_compressed_decompressed.ibd" ]; then
    echo "Using existing decompressed file..."
    sudo cp tests/ibd_files/test_compressed_decompressed.ibd /var/lib/mysql/test_rowformat/test_table.ibd
    sudo chown mysql:mysql /var/lib/mysql/test_rowformat/test_table.ibd
    sudo chmod 640 /var/lib/mysql/test_rowformat/test_table.ibd
else
    echo "No decompressed file found, running compressed test to create one..."
    # First check if we already have a compressed file from previous test
    if [ -f "tests/ibd_files/test_compressed_compressed.ibd" ]; then
        echo "Found compressed file, decompressing it..."
        cd "$PARSER_DIR"
        ./build/ib_parser 2 tests/ibd_files/test_compressed_compressed.ibd tests/ibd_files/test_compressed_decompressed.ibd
    else
        echo "Running full test to create compressed file..."
        cd "$PARSER_DIR"
        KEEP_FILES=1 ./tests/test_compressed.sh
    fi
    sudo cp tests/ibd_files/test_compressed_decompressed.ibd /var/lib/mysql/test_rowformat/test_table.ibd  
    sudo chown mysql:mysql /var/lib/mysql/test_rowformat/test_table.ibd
    sudo chmod 640 /var/lib/mysql/test_rowformat/test_table.ibd
fi

echo ""
echo "Attempting import - this should show the ROW_FORMAT error:"
echo "==========================================================="
mysql -uroot test_rowformat -e "ALTER TABLE test_table IMPORT TABLESPACE;" 2>&1 || true

echo ""
echo "The above error is expected - it shows that the decompressed file"
echo "still contains COMPRESSED metadata in its headers."
