#!/bin/bash

# Test script for Mode 5 SDI Rebuild (RES-22 Option B)
# This script tests the --sdi-json flag that rebuilds SDI from ibd2sdi JSON

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Configuration
DB_USER="root"
DB_NAME="test_sdi_rebuild"
TABLE_NAME="compressed_imported"
MYSQL_DATA_DIR="/var/lib/mysql"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PARSER_DIR="$(dirname "$SCRIPT_DIR")"
OUTPUT_FILE="/tmp/sdi_rebuild_test.ibd"

# Source files
SOURCE_IBD="$SCRIPT_DIR/compressed_test.ibd"
SOURCE_SDI="$SCRIPT_DIR/compressed_test_sdi.json"

echo "==========================================="
echo "Testing Mode 5: SDI Rebuild with --sdi-json"
echo "==========================================="
echo ""

# Helper to run MySQL and show command
run_mysql() {
    local cmd="$1"
    local db="${2:-}"
    echo -e "${CYAN}  [MySQL] $cmd${NC}"
    if [ -n "$db" ]; then
        mysql -u$DB_USER "$db" -e "$cmd" 2>&1 || true
    else
        mysql -u$DB_USER -e "$cmd" 2>&1 || true
    fi
}

# Step 1: Check prerequisites
echo -e "${YELLOW}Step 1: Checking prerequisites${NC}"
if [ ! -f "$SOURCE_IBD" ]; then
    echo -e "${RED}ERROR: Source .ibd not found: $SOURCE_IBD${NC}"
    exit 1
fi
echo "  Source .ibd: $SOURCE_IBD ($(stat -c%s "$SOURCE_IBD") bytes)"

if [ ! -f "$SOURCE_SDI" ]; then
    echo -e "${RED}ERROR: Source SDI JSON not found: $SOURCE_SDI${NC}"
    exit 1
fi
echo "  Source SDI:  $SOURCE_SDI"

if [ ! -f "$PARSER_DIR/build/ib_parser" ]; then
    echo -e "${YELLOW}  ib_parser not found, building...${NC}"
    make -C "$PARSER_DIR/build" -j$(nproc)
fi
echo "  ib_parser:   $PARSER_DIR/build/ib_parser"
echo ""

# Step 2: Run Mode 5 with --sdi-json
echo -e "${YELLOW}Step 2: Running Mode 5 with --sdi-json${NC}"
echo -e "${CYAN}  [CMD] ./build/ib_parser 5 $SOURCE_IBD $OUTPUT_FILE --sdi-json=$SOURCE_SDI${NC}"
cd "$PARSER_DIR"
if ./build/ib_parser 5 "$SOURCE_IBD" "$OUTPUT_FILE" --sdi-json="$SOURCE_SDI" 2>&1; then
    echo -e "${GREEN}  ✓ Mode 5 completed successfully${NC}"
else
    echo -e "${RED}  ✗ Mode 5 failed${NC}"
    exit 1
fi
echo ""

# Step 3: Verify output file
echo -e "${YELLOW}Step 3: Verifying output file${NC}"
if [ -f "$OUTPUT_FILE" ]; then
    echo "  Output file: $OUTPUT_FILE ($(stat -c%s "$OUTPUT_FILE") bytes)"
    echo -e "${GREEN}  ✓ Output file created${NC}"
else
    echo -e "${RED}  ✗ Output file not created${NC}"
    exit 1
fi
echo ""

# Step 4: Test with ibd2sdi
echo -e "${YELLOW}Step 4: Testing with ibd2sdi${NC}"
echo -e "${CYAN}  [CMD] ibd2sdi $OUTPUT_FILE${NC}"
if ibd2sdi "$OUTPUT_FILE" > /tmp/sdi_output.json 2>&1; then
    echo -e "${GREEN}  ✓ ibd2sdi can read the file${NC}"
    echo "  SDI output saved to /tmp/sdi_output.json"
    # Show table name from SDI
    TABLE_IN_SDI=$(grep -o '"name": "[^"]*"' /tmp/sdi_output.json | head -1 || echo "unknown")
    echo "  Table in SDI: $TABLE_IN_SDI"
else
    echo -e "${RED}  ✗ ibd2sdi failed:${NC}"
    cat /tmp/sdi_output.json 2>/dev/null || true
    ibd2sdi "$OUTPUT_FILE" 2>&1 || true
fi
echo ""

# Step 5: Test with innochecksum
echo -e "${YELLOW}Step 5: Testing with innochecksum${NC}"
echo -e "${CYAN}  [CMD] innochecksum $OUTPUT_FILE${NC}"
if innochecksum "$OUTPUT_FILE" 2>&1; then
    echo -e "${GREEN}  ✓ innochecksum passed${NC}"
else
    echo -e "${RED}  ✗ innochecksum failed${NC}"
fi
echo ""

# Step 6: Show page types
echo -e "${YELLOW}Step 6: Page type summary${NC}"
echo -e "${CYAN}  [CMD] innochecksum -S $OUTPUT_FILE${NC}"
innochecksum -S "$OUTPUT_FILE" 2>&1 || true
echo ""

# Step 7: Prepare MySQL for import test
echo -e "${YELLOW}Step 7: Preparing MySQL for import test${NC}"

# Clean up existing database
run_mysql "DROP DATABASE IF EXISTS $DB_NAME;"

# Stop MySQL to clean filesystem
echo "  Stopping MySQL to clean up..."
sudo systemctl stop mysql
sleep 2
sudo rm -rf "$MYSQL_DATA_DIR/$DB_NAME"
sudo systemctl start mysql
sleep 3

# Wait for MySQL
while ! mysql -u$DB_USER -e "SELECT 1;" >/dev/null 2>&1; do
    echo "  Waiting for MySQL..."
    sleep 1
done

# Create database
run_mysql "CREATE DATABASE $DB_NAME;"
echo ""

# Step 8: Create matching table structure
echo -e "${YELLOW}Step 8: Creating table with matching structure${NC}"
# The SDI describes table "compressed_test" (columns must match to import)

cat << 'EOF'
  Table structure (from SDI):
  - id INT PRIMARY KEY AUTO_INCREMENT
  - name VARCHAR(100)
  - description TEXT
  - created_at DATETIME DEFAULT CURRENT_TIMESTAMP
EOF

run_mysql "CREATE TABLE $TABLE_NAME (
    id INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(100),
    description TEXT,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
) ROW_FORMAT=DYNAMIC;" "$DB_NAME"
echo ""

# Step 9: Discard tablespace
echo -e "${YELLOW}Step 9: Discarding tablespace${NC}"
run_mysql "ALTER TABLE $TABLE_NAME DISCARD TABLESPACE;" "$DB_NAME"
echo ""

# Step 10: Copy rebuilt file
echo -e "${YELLOW}Step 10: Copying rebuilt .ibd file${NC}"
echo "  Stopping MySQL..."
sudo systemctl stop mysql
sleep 2

TARGET_IBD="$MYSQL_DATA_DIR/$DB_NAME/${TABLE_NAME}.ibd"
echo -e "${CYAN}  [CMD] cp $OUTPUT_FILE $TARGET_IBD${NC}"
sudo cp "$OUTPUT_FILE" "$TARGET_IBD"
sudo chown mysql:mysql "$TARGET_IBD"
sudo chmod 640 "$TARGET_IBD"
echo "  File copied to: $TARGET_IBD"

echo "  Starting MySQL..."
sudo systemctl start mysql
sleep 3

# Wait for MySQL
while ! mysql -u$DB_USER -e "SELECT 1;" >/dev/null 2>&1; do
    echo "  Waiting for MySQL..."
    sleep 1
done
echo ""

# Step 11: Attempt import
echo -e "${YELLOW}Step 11: Attempting IMPORT TABLESPACE${NC}"
echo -e "${CYAN}  [MySQL] ALTER TABLE $TABLE_NAME IMPORT TABLESPACE;${NC}"
echo ""

# Run import and capture full output
IMPORT_RESULT=$(mysql -u$DB_USER "$DB_NAME" -e "ALTER TABLE $TABLE_NAME IMPORT TABLESPACE;" 2>&1) || true
if [ -n "$IMPORT_RESULT" ]; then
    echo -e "${RED}  MySQL output:${NC}"
    echo "$IMPORT_RESULT" | sed 's/^/    /'
else
    echo -e "${GREEN}  ✓ IMPORT TABLESPACE succeeded!${NC}"
fi
echo ""

# Step 12: Check MySQL error log
echo -e "${YELLOW}Step 12: Checking MySQL error log (last 30 lines)${NC}"
MYSQL_LOG="/var/log/mysql/error.log"
echo -e "${CYAN}  [CMD] grep crash/signal/corruption in $MYSQL_LOG${NC}"
sudo grep -E "(signal|crash|corruption|validation|Apparent|IMPORT)" "$MYSQL_LOG" 2>/dev/null | tail -15 || echo "  No relevant errors found"
echo ""

# Step 13: Try to query the table (if import succeeded)
echo -e "${YELLOW}Step 13: Attempting to query imported data${NC}"
run_mysql "SELECT * FROM $TABLE_NAME LIMIT 5;" "$DB_NAME"
echo ""

# Summary
echo "==========================================="
echo "Test Summary"
echo "==========================================="
echo ""
echo "Source files:"
echo "  .ibd: $SOURCE_IBD"
echo "  .json: $SOURCE_SDI"
echo ""
echo "Output file:"
echo "  $OUTPUT_FILE"
echo ""
echo "Test results:"

# Check each step
if [ -f "$OUTPUT_FILE" ]; then
    echo -e "  Mode 5 rebuild:    ${GREEN}PASS${NC}"
else
    echo -e "  Mode 5 rebuild:    ${RED}FAIL${NC}"
fi

if ibd2sdi "$OUTPUT_FILE" >/dev/null 2>&1; then
    echo -e "  ibd2sdi:           ${GREEN}PASS${NC}"
else
    echo -e "  ibd2sdi:           ${RED}FAIL${NC}"
fi

if innochecksum "$OUTPUT_FILE" >/dev/null 2>&1; then
    echo -e "  innochecksum:      ${GREEN}PASS${NC}"
else
    echo -e "  innochecksum:      ${RED}FAIL${NC}"
fi

if mysql -u$DB_USER "$DB_NAME" -e "SELECT COUNT(*) FROM $TABLE_NAME;" >/dev/null 2>&1; then
    echo -e "  MySQL IMPORT:      ${GREEN}PASS${NC}"
else
    echo -e "  MySQL IMPORT:      ${RED}FAIL${NC}"
fi

echo ""
echo "Database '$DB_NAME' kept for inspection. Drop manually if needed:"
echo "  mysql -u$DB_USER -e 'DROP DATABASE $DB_NAME;'"
echo ""
