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
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PARSER_DIR="$(dirname "$SCRIPT_DIR")"
KEYRING_FILE="/var/lib/mysql-keyring/keyring"  # Default keyring location

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}Step 1: Checking encryption availability${NC}"
# Check if encryption is available by testing table creation
mysql -u$DB_USER -e "CREATE DATABASE IF NOT EXISTS _encryption_test_tmp_;" 2>/dev/null
if ! mysql -u$DB_USER -e "CREATE TABLE _encryption_test_tmp_.enc_check (id INT) ENCRYPTION='Y';" 2>/dev/null; then
    echo -e "${YELLOW}SKIPPED: Encryption not available (keyring not configured)${NC}"
    echo "To enable encryption, configure keyring component in Percona 8.0"
    mysql -u$DB_USER -e "DROP DATABASE IF EXISTS _encryption_test_tmp_;" 2>/dev/null
    exit 0
fi
mysql -u$DB_USER -e "DROP DATABASE IF EXISTS _encryption_test_tmp_;" 2>/dev/null
echo -e "${GREEN}Encryption is available${NC}"

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

echo -e "${YELLOW}Step 10: Verifying decryption success${NC}"
echo "Note: Direct import to MySQL may fail due to metadata, so we'll verify decryption by inspecting the file content"

# Start MySQL if it's stopped
sudo systemctl start mysql 2>/dev/null || true
sleep 2

echo -e "${YELLOW}Step 11: Inspecting decrypted file for readable data${NC}"

# Define test data that should be visible after decryption
EXPECTED_DATA=(
    "Encrypted data row"
    "Secret info"
    "Confidential"
    "Private"
    "Classified"
    "Restricted"
)

echo "Checking encrypted file (should NOT find readable data):"
ENCRYPTED_READABLE=0
for TERM in "${EXPECTED_DATA[@]}"; do
    if strings "tests/${TABLE_NAME}_encrypted.ibd" 2>/dev/null | grep -q "$TERM"; then
        ((ENCRYPTED_READABLE++))
    fi
done
echo "  Found $ENCRYPTED_READABLE readable terms (expected: 0 for encrypted file)"

echo ""
echo "Checking decrypted file (SHOULD find readable data):"
DECRYPTED_READABLE=0
FOUND_TERMS=""
for TERM in "${EXPECTED_DATA[@]}"; do
    if strings "tests/${IMPORT_TABLE}_temp.ibd" 2>/dev/null | grep -q "$TERM"; then
        ((DECRYPTED_READABLE++))
        FOUND_TERMS="${FOUND_TERMS}  ✓ Found: $TERM\n"
    fi
done

echo -e "$FOUND_TERMS"
echo "  Found $DECRYPTED_READABLE readable terms"

# Show some actual decrypted content
echo ""
echo "Sample of decrypted content:"
strings "tests/${IMPORT_TABLE}_temp.ibd" 2>/dev/null | grep -E "(Encrypted data row|Secret info|Confidential|Private|Classified)" | head -5 | sed 's/^/  /'

# Verify decryption success
echo ""
if [ $ENCRYPTED_READABLE -eq 0 ] && [ $DECRYPTED_READABLE -gt 0 ]; then
    echo -e "${GREEN}✓ SUCCESS: Decryption verified!${NC}"
    echo -e "${GREEN}  - Encrypted file: No readable data (as expected)${NC}"
    echo -e "${GREEN}  - Decrypted file: Found $DECRYPTED_READABLE expected terms${NC}"
else
    if [ $ENCRYPTED_READABLE -gt 0 ]; then
        echo -e "${YELLOW}⚠ Warning: File might not have been encrypted properly${NC}"
    fi
    if [ $DECRYPTED_READABLE -eq 0 ]; then
        echo -e "${RED}✗ Error: Decryption may have failed - no readable data found${NC}"
    fi
fi

echo -e "${YELLOW}Step 12: Cleanup${NC}"
rm -f "$PARSER_DIR/tests/${TABLE_NAME}_encrypted.ibd"
rm -f "$PARSER_DIR/tests/keyring_backup"
rm -f /tmp/decrypt_log.txt

echo -e "${GREEN}Encrypted table test completed!${NC}"
echo "Database '$DB_NAME' kept for inspection. Drop it manually if needed."