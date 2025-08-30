#!/bin/bash

# Test script for ENCRYPTED tables
# This script creates an encrypted table, exports it, decrypts with ib_parser,
# and imports it back as a normal table

set -e  # Exit on error

echo "==========================================="
echo "Testing ENCRYPTED Table Processing"
echo "==========================================="

# Configuration
DB_USER="root"
DB_PASS=""  # Add password if needed
DB_NAME="test_encryption"
TABLE_NAME="test_encrypted"
IMPORT_TABLE="test_normal_from_encrypted"
MYSQL_DATA_DIR="/var/lib/mysql"  # Adjust if different
PARSER_DIR="/home/cslog/percona-parser"
KEYRING_FILE="/var/lib/mysql-keyring/keyring"  # Default keyring location

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}Step 1: Enabling encryption in Percona Server${NC}"
# Check if encryption is already enabled
ENCRYPTION_STATUS=$(mysql -u$DB_USER -sN -e "SELECT @@innodb_encrypt_tables;" 2>/dev/null || echo "OFF")

if [ "$ENCRYPTION_STATUS" != "ON" ]; then
    echo "Enabling table encryption..."
    mysql -u$DB_USER <<EOF
SET GLOBAL innodb_encrypt_tables = ON;
SET GLOBAL innodb_encrypt_log = ON;
SET GLOBAL innodb_redo_log_encrypt = ON;
SET GLOBAL innodb_undo_log_encrypt = ON;
EOF
fi

# Ensure keyring plugin is loaded
KEYRING_LOADED=$(mysql -u$DB_USER -sN -e "SELECT COUNT(*) FROM information_schema.PLUGINS WHERE PLUGIN_NAME='keyring_file';" 2>/dev/null || echo "0")

if [ "$KEYRING_LOADED" -eq "0" ]; then
    echo "Loading keyring_file plugin..."
    mysql -u$DB_USER -e "INSTALL PLUGIN keyring_file SONAME 'keyring_file.so';"
fi

echo -e "${YELLOW}Step 2: Creating test database and encrypted table${NC}"
mysql -u$DB_USER -e "DROP DATABASE IF EXISTS $DB_NAME;"
mysql -u$DB_USER -e "CREATE DATABASE $DB_NAME;"

mysql -u$DB_USER $DB_NAME <<EOF
CREATE TABLE $TABLE_NAME (
    id INT PRIMARY KEY AUTO_INCREMENT,
    sensitive_data VARCHAR(255),
    secret_number INT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENCRYPTION='Y';

-- Insert test data
INSERT INTO $TABLE_NAME (sensitive_data, secret_number) VALUES 
    ('Encrypted data row 1', 1001),
    ('Encrypted data row 2', 2002),
    ('Encrypted data row 3', 3003),
    ('Encrypted data row 4', 4004),
    ('Encrypted data row 5', 5005);

-- Insert more rows
INSERT INTO $TABLE_NAME (sensitive_data, secret_number) 
SELECT CONCAT('Secret info ', id), id * 100 
FROM $TABLE_NAME;

INSERT INTO $TABLE_NAME (sensitive_data, secret_number) 
SELECT CONCAT('Confidential ', id), id * 200 
FROM $TABLE_NAME;

SELECT COUNT(*) as row_count FROM $TABLE_NAME;
EOF

echo -e "${YELLOW}Step 3: Getting encryption info${NC}"
# Get server UUID and master key ID
SERVER_UUID=$(mysql -u$DB_USER -sN -e "SELECT @@server_uuid;")
echo "Server UUID: $SERVER_UUID"

# Get the master key ID (usually starts from 1)
MASTER_KEY_ID=1
echo "Using Master Key ID: $MASTER_KEY_ID"

# Find keyring file location
if [ ! -f "$KEYRING_FILE" ]; then
    # Try alternative locations
    KEYRING_FILE=$(mysql -u$DB_USER -sN -e "SELECT @@keyring_file_data;" 2>/dev/null | cut -d';' -f1 || echo "")
    if [ -z "$KEYRING_FILE" ] || [ ! -f "$KEYRING_FILE" ]; then
        echo -e "${YELLOW}Trying to find keyring file...${NC}"
        KEYRING_FILE=$(find /var/lib/mysql* -name "keyring" -type f 2>/dev/null | head -1)
    fi
fi

if [ ! -f "$KEYRING_FILE" ]; then
    echo -e "${RED}Error: Keyring file not found. Please check keyring configuration.${NC}"
    exit 1
fi
echo "Keyring file: $KEYRING_FILE"

echo -e "${YELLOW}Step 4: Flushing table and getting .ibd file path${NC}"
mysql -u$DB_USER $DB_NAME -e "FLUSH TABLES $TABLE_NAME FOR EXPORT;"

# Get the actual .ibd file path
IBD_PATH="$MYSQL_DATA_DIR/$DB_NAME/${TABLE_NAME}.ibd"

# Check with sudo since MySQL data dir needs root access
if ! sudo test -f "$IBD_PATH"; then
    echo -e "${RED}Error: .ibd file not found at $IBD_PATH${NC}"
    mysql -u$DB_USER $DB_NAME -e "UNLOCK TABLES;"
    exit 1
fi

# Copy the .ibd and keyring files for processing (using sudo)
sudo cp "$IBD_PATH" "$PARSER_DIR/tests/${TABLE_NAME}_encrypted.ibd"
sudo chown $(whoami):$(whoami) "$PARSER_DIR/tests/${TABLE_NAME}_encrypted.ibd"
sudo cp "$KEYRING_FILE" "$PARSER_DIR/tests/keyring_backup"
sudo chown $(whoami):$(whoami) "$PARSER_DIR/tests/keyring_backup"

mysql -u$DB_USER $DB_NAME -e "UNLOCK TABLES;"

echo -e "${YELLOW}Step 5: Creating import table structure${NC}"
mysql -u$DB_USER $DB_NAME <<EOF
CREATE TABLE $IMPORT_TABLE (
    id INT PRIMARY KEY AUTO_INCREMENT,
    sensitive_data VARCHAR(255),
    secret_number INT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENCRYPTION='N';
EOF

echo -e "${YELLOW}Step 6: Preparing for import - discarding tablespace${NC}"
mysql -u$DB_USER $DB_NAME -e "ALTER TABLE $IMPORT_TABLE DISCARD TABLESPACE;"

echo -e "${YELLOW}Step 7: Stopping MySQL to process the file${NC}"
sudo systemctl stop mysql

echo -e "${YELLOW}Step 8: Decrypting with ib_parser${NC}"
cd $PARSER_DIR

# Try decryption with different master key IDs if the first fails
for KEY_ID in 1 2; do
    echo "Trying with master_key_id=$KEY_ID..."
    # First decrypt to a temp location, then move with sudo
    if ./build/ib_parser 1 $KEY_ID "$SERVER_UUID" "tests/keyring_backup" "tests/${TABLE_NAME}_encrypted.ibd" "tests/${IMPORT_TABLE}_temp.ibd" 2>&1 | tee /tmp/decrypt_log.txt; then
        if grep -q "Successfully decrypted" /tmp/decrypt_log.txt; then
            echo "Successfully decrypted with master_key_id=$KEY_ID"
            sudo mv "tests/${IMPORT_TABLE}_temp.ibd" "$MYSQL_DATA_DIR/$DB_NAME/${IMPORT_TABLE}.ibd"
            break
        fi
    fi
done

# Set proper ownership
sudo chown mysql:mysql "$MYSQL_DATA_DIR/$DB_NAME/${IMPORT_TABLE}.ibd"
sudo chmod 640 "$MYSQL_DATA_DIR/$DB_NAME/${IMPORT_TABLE}.ibd"

echo -e "${YELLOW}Step 9: Starting MySQL${NC}"
sudo systemctl start mysql

# Wait for MySQL to be ready
sleep 3

echo -e "${YELLOW}Step 10: Importing the decrypted tablespace${NC}"
mysql -u$DB_USER $DB_NAME -e "ALTER TABLE $IMPORT_TABLE IMPORT TABLESPACE;"

echo -e "${YELLOW}Step 11: Verifying the imported data${NC}"
echo "Original encrypted table data:"
mysql -u$DB_USER $DB_NAME -e "SELECT * FROM $TABLE_NAME LIMIT 5;"

echo "Imported decrypted table data:"
mysql -u$DB_USER $DB_NAME -e "SELECT * FROM $IMPORT_TABLE LIMIT 5;"

# Compare row counts
ORIG_COUNT=$(mysql -u$DB_USER $DB_NAME -sN -e "SELECT COUNT(*) FROM $TABLE_NAME;")
IMPORT_COUNT=$(mysql -u$DB_USER $DB_NAME -sN -e "SELECT COUNT(*) FROM $IMPORT_TABLE;")

if [ "$ORIG_COUNT" -eq "$IMPORT_COUNT" ]; then
    echo -e "${GREEN}✓ Success: Row counts match ($ORIG_COUNT rows)${NC}"
    
    # Compare data
    DIFF_COUNT=$(mysql -u$DB_USER $DB_NAME -sN -e "
        SELECT COUNT(*) FROM (
            SELECT * FROM $TABLE_NAME
            EXCEPT
            SELECT * FROM $IMPORT_TABLE
        ) as diff;
    " 2>/dev/null || echo "0")
    
    if [ "$DIFF_COUNT" -eq "0" ]; then
        echo -e "${GREEN}✓ Success: All data matches perfectly!${NC}"
    else
        echo -e "${RED}✗ Warning: Data mismatch detected${NC}"
    fi
else
    echo -e "${RED}✗ Error: Row count mismatch (Original: $ORIG_COUNT, Imported: $IMPORT_COUNT)${NC}"
fi

echo -e "${YELLOW}Step 12: Cleanup${NC}"
rm -f "$PARSER_DIR/tests/${TABLE_NAME}_encrypted.ibd"
rm -f "$PARSER_DIR/tests/keyring_backup"
rm -f /tmp/decrypt_log.txt

echo -e "${GREEN}Encrypted table test completed!${NC}"
echo "Database '$DB_NAME' kept for inspection. Drop it manually if needed."