#!/bin/bash

# Test script for COMPRESSED tables
# This script creates a compressed table, exports it, decompresses with ib_parser,
# and imports it back as a normal table

set -e  # Exit on error

echo "==========================================="
echo "Testing COMPRESSED Table Processing"
echo "==========================================="

# Configuration
DB_USER="root"
DB_PASS=""  # Add password if needed
DB_NAME="test_compression"
TABLE_NAME="test_compressed"
IMPORT_TABLE="test_normal_from_compressed"
MYSQL_DATA_DIR="/var/lib/mysql"  # Adjust if different
PARSER_DIR="/home/cslog/percona-parser"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}Step 1: Creating test database and compressed table${NC}"
mysql -u$DB_USER -e "DROP DATABASE IF EXISTS $DB_NAME;"
mysql -u$DB_USER -e "CREATE DATABASE $DB_NAME;"

mysql -u$DB_USER $DB_NAME <<EOF
CREATE TABLE $TABLE_NAME (
    id INT PRIMARY KEY AUTO_INCREMENT,
    data VARCHAR(255),
    number INT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8;

-- Insert test data
INSERT INTO $TABLE_NAME (data, number) VALUES 
    ('First compressed row', 100),
    ('Second compressed row', 200),
    ('Third compressed row', 300),
    ('Fourth compressed row', 400),
    ('Fifth compressed row', 500);

-- Insert more rows to make compression effective
INSERT INTO $TABLE_NAME (data, number) 
SELECT CONCAT('Generated row ', id), id * 10 
FROM $TABLE_NAME;

INSERT INTO $TABLE_NAME (data, number) 
SELECT CONCAT('More data ', id), id * 20 
FROM $TABLE_NAME;

INSERT INTO $TABLE_NAME (data, number) 
SELECT CONCAT('Even more data ', id), id * 30 
FROM $TABLE_NAME;

SELECT COUNT(*) as row_count FROM $TABLE_NAME;
EOF

echo -e "${YELLOW}Step 2: Flushing table and getting .ibd file path${NC}"
mysql -u$DB_USER $DB_NAME -e "FLUSH TABLES $TABLE_NAME FOR EXPORT;"

# Get the actual .ibd file path
IBD_PATH="$MYSQL_DATA_DIR/$DB_NAME/${TABLE_NAME}.ibd"

# Check with sudo since MySQL data dir needs root access
if ! sudo test -f "$IBD_PATH"; then
    echo -e "${RED}Error: .ibd file not found at $IBD_PATH${NC}"
    mysql -u$DB_USER $DB_NAME -e "UNLOCK TABLES;"
    exit 1
fi

# Copy the .ibd file for processing (using sudo)
sudo cp "$IBD_PATH" "$PARSER_DIR/tests/${TABLE_NAME}_compressed.ibd"
sudo chown $(whoami):$(whoami) "$PARSER_DIR/tests/${TABLE_NAME}_compressed.ibd"

mysql -u$DB_USER $DB_NAME -e "UNLOCK TABLES;"

echo -e "${YELLOW}Step 3: Decompressing with ib_parser${NC}"
cd $PARSER_DIR
./build/ib_parser 2 "tests/${TABLE_NAME}_compressed.ibd" "tests/${TABLE_NAME}_decompressed.ibd"

echo -e "${YELLOW}Step 4: Creating new database for import${NC}"
IMPORT_DB_NAME="${DB_NAME}_import"
mysql -u$DB_USER -e "DROP DATABASE IF EXISTS $IMPORT_DB_NAME;"
mysql -u$DB_USER -e "CREATE DATABASE $IMPORT_DB_NAME;"

echo -e "${YELLOW}Step 5: Creating new uncompressed table with same structure${NC}"
mysql -u$DB_USER $IMPORT_DB_NAME <<EOF
CREATE TABLE $IMPORT_TABLE (
    id INT PRIMARY KEY AUTO_INCREMENT,
    data VARCHAR(255),
    number INT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ROW_FORMAT=DYNAMIC;
EOF

echo -e "${YELLOW}Step 6: Preparing for import - discarding tablespace${NC}"
mysql -u$DB_USER $IMPORT_DB_NAME -e "ALTER TABLE $IMPORT_TABLE DISCARD TABLESPACE;"

echo -e "${YELLOW}Step 7: Stopping MySQL to copy the decompressed file${NC}"
sudo systemctl stop mysql

echo -e "${YELLOW}Step 8: Copying decompressed file and removing original to avoid ID conflicts${NC}"
# Remove the original compressed table's .ibd to avoid tablespace ID conflict
sudo rm -f "$MYSQL_DATA_DIR/$DB_NAME/${TABLE_NAME}.ibd"
# Copy the decompressed file
sudo cp "$PARSER_DIR/tests/${TABLE_NAME}_decompressed.ibd" "$MYSQL_DATA_DIR/$IMPORT_DB_NAME/${IMPORT_TABLE}.ibd"
sudo chown mysql:mysql "$MYSQL_DATA_DIR/$IMPORT_DB_NAME/${IMPORT_TABLE}.ibd"
sudo chmod 640 "$MYSQL_DATA_DIR/$IMPORT_DB_NAME/${IMPORT_TABLE}.ibd"

echo -e "${YELLOW}Step 9: Starting MySQL${NC}"
sudo systemctl start mysql

# Wait for MySQL to be ready
sleep 3

echo -e "${YELLOW}Step 10: Importing the decompressed tablespace${NC}"
mysql -u$DB_USER $IMPORT_DB_NAME -e "ALTER TABLE $IMPORT_TABLE IMPORT TABLESPACE;"

echo -e "${YELLOW}Step 11: Verifying the imported data${NC}"
echo "Imported decompressed table data:"
mysql -u$DB_USER $IMPORT_DB_NAME -e "SELECT * FROM $IMPORT_TABLE LIMIT 5;"

# Check row count (we expect 40 rows based on the inserts)
IMPORT_COUNT=$(mysql -u$DB_USER $IMPORT_DB_NAME -sN -e "SELECT COUNT(*) FROM $IMPORT_TABLE;")
EXPECTED_COUNT=40

if [ "$IMPORT_COUNT" -eq "$EXPECTED_COUNT" ]; then
    echo -e "${GREEN}✓ Success: Row count is correct ($IMPORT_COUNT rows)${NC}"
    
    # Verify some specific data
    FIRST_ROW=$(mysql -u$DB_USER $IMPORT_DB_NAME -sN -e "SELECT data FROM $IMPORT_TABLE WHERE id=1;")
    if [[ "$FIRST_ROW" == "First compressed row" ]]; then
        echo -e "${GREEN}✓ Success: Data integrity verified!${NC}"
    else
        echo -e "${RED}✗ Warning: Data may be corrupted${NC}"
    fi
else
    echo -e "${RED}✗ Error: Row count mismatch (Expected: $EXPECTED_COUNT, Got: $IMPORT_COUNT)${NC}"
fi

echo -e "${YELLOW}Step 12: Cleanup${NC}"
rm -f "$PARSER_DIR/tests/${TABLE_NAME}_compressed.ibd"
rm -f "$PARSER_DIR/tests/${TABLE_NAME}_decompressed.ibd"

echo -e "${GREEN}Compressed table test completed!${NC}"
echo "Databases '$DB_NAME' and '$IMPORT_DB_NAME' kept for inspection. Drop them manually if needed."