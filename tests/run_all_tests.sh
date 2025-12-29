#!/bin/bash

# Master script to run all ib_parser tests
# This script runs tests for compressed, encrypted, and encrypted+compressed tables

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PARSER_DIR="$(dirname "$SCRIPT_DIR")"
LOG_DIR="$SCRIPT_DIR/logs"

# Create log directory
mkdir -p "$LOG_DIR"

echo -e "${BLUE}==========================================="
echo -e "IB_PARSER COMPREHENSIVE TEST SUITE"
echo -e "===========================================${NC}"
echo ""
echo "This will test:"
echo "1. Compressed tables"
echo "2. Encrypted tables"
echo "3. Encrypted + Compressed tables"
echo "4. Type decoding (DECIMAL, DATE, TIME, ENUM, SET)"
echo "5. LOB external decoding (LONGTEXT, LONGBLOB)"
echo "6. ZLOB compressed decoding (ROW_FORMAT=COMPRESSED)"
echo ""

# Function to run a test and capture results
run_test() {
    local test_name=$1
    local test_script=$2
    local log_file="$LOG_DIR/${test_name}_$(date +%Y%m%d_%H%M%S).log"
    
    echo -e "${YELLOW}Running: $test_name${NC}"
    echo "Log file: $log_file"
    
    if bash "$test_script" > "$log_file" 2>&1; then
        echo -e "${GREEN}✓ $test_name PASSED${NC}"
        return 0
    else
        echo -e "${RED}✗ $test_name FAILED${NC}"
        echo "Check log file for details: $log_file"
        tail -20 "$log_file"
        return 1
    fi
}

# Check prerequisites
echo -e "${YELLOW}Checking prerequisites...${NC}"

# Check if MySQL/Percona is running
if ! systemctl is-active --quiet mysql; then
    echo -e "${RED}Error: MySQL/Percona Server is not running${NC}"
    echo "Please start MySQL first: sudo systemctl start mysql"
    exit 1
fi

# Check if ib_parser is built
if [ ! -f "$PARSER_DIR/build/ib_parser" ]; then
    echo -e "${RED}Error: ib_parser not found${NC}"
    echo "Please build the project first:"
    echo "  cd $PARSER_DIR/build && cmake .. && make"
    exit 1
fi

# Check sudo access (needed for stopping/starting MySQL)
if ! sudo -n true 2>/dev/null; then
    echo -e "${YELLOW}This script requires sudo access to stop/start MySQL${NC}"
    echo "Please enter your password when prompted."
    sudo true
fi

echo -e "${GREEN}Prerequisites check passed${NC}"
echo ""

# Track test results
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

# Run tests
echo -e "${BLUE}Starting test suite...${NC}"
echo "=========================================="

# Test 1: Compressed tables
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "COMPRESSED_TABLES" "$SCRIPT_DIR/test_compressed.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo ""

# Test 2: Encrypted tables
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "ENCRYPTED_TABLES" "$SCRIPT_DIR/test_encrypted.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo ""

# Test 3: Encrypted + Compressed tables
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "ENCRYPTED_COMPRESSED_TABLES" "$SCRIPT_DIR/test_encrypted_compressed.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo ""

# Test 4: Type decoding (DECIMAL, DATE, TIME, ENUM, SET, etc.)
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "TYPE_DECODING" "$SCRIPT_DIR/test_types_decode.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo ""

# Test 5: LOB external decoding (LONGTEXT, LONGBLOB)
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "LOB_DECODING" "$SCRIPT_DIR/test_lob_decode.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo ""

# Test 6: ZLOB compressed decoding (ROW_FORMAT=COMPRESSED)
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "ZLOB_DECODING" "$SCRIPT_DIR/test_zlob_decode.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
echo ""

# Summary
echo "=========================================="
echo -e "${BLUE}TEST SUITE SUMMARY${NC}"
echo "=========================================="
echo "Total tests run: $TOTAL_TESTS"
echo -e "${GREEN}Passed: $PASSED_TESTS${NC}"
echo -e "${RED}Failed: $FAILED_TESTS${NC}"
echo ""

if [ $FAILED_TESTS -eq 0 ]; then
    echo -e "${GREEN}✓ ALL TESTS PASSED!${NC}"
    exit 0
else
    echo -e "${RED}✗ SOME TESTS FAILED${NC}"
    echo "Check the log files in: $LOG_DIR"
    exit 1
fi