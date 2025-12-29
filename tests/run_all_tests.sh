#!/bin/bash

# Master script to run all ib_parser tests
# This script runs tests for compressed, encrypted, and encrypted+compressed tables
#
# Usage:
#   ./run_all_tests.sh              # Normal mode (quiet, logs to files)
#   ./run_all_tests.sh --verbose    # Show real-time test output
#   ./run_all_tests.sh --json       # Output results as JSON
#   ./run_all_tests.sh -v --json    # Both verbose and JSON output

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PARSER_DIR="$(dirname "$SCRIPT_DIR")"
LOG_DIR="$SCRIPT_DIR/logs"

# Parse arguments
VERBOSE=false
JSON_OUTPUT=false
for arg in "$@"; do
    case $arg in
        -v|--verbose)
            VERBOSE=true
            ;;
        -j|--json)
            JSON_OUTPUT=true
            ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  -v, --verbose    Show real-time test output"
            echo "  -j, --json       Output results as JSON"
            echo "  -h, --help       Show this help message"
            exit 0
            ;;
    esac
done

# Create log directory
mkdir -p "$LOG_DIR"

# JSON results array
declare -a JSON_RESULTS

# Function to add JSON result
add_json_result() {
    local name=$1
    local status=$2
    local duration=$3
    local message=$4
    local log_file=$5

    JSON_RESULTS+=("{\"name\":\"$name\",\"status\":\"$status\",\"duration_seconds\":$duration,\"message\":\"$message\",\"log_file\":\"$log_file\"}")
}

# Function to run a test and capture results
run_test() {
    local test_name=$1
    local test_script=$2
    local log_file="$LOG_DIR/${test_name}_$(date +%Y%m%d_%H%M%S).log"
    local start_time=$(date +%s)
    local exit_code=0

    if [ "$JSON_OUTPUT" != "true" ]; then
        echo -e "${YELLOW}Running: $test_name${NC}"
        echo "Log file: $log_file"
    fi

    if [ "$VERBOSE" = "true" ]; then
        # Verbose mode: show output in real-time and also save to log
        echo -e "${CYAN}----------------------------------------${NC}"
        if VERBOSE=1 bash "$test_script" 2>&1 | tee "$log_file"; then
            exit_code=0
        else
            exit_code=1
        fi
        echo -e "${CYAN}----------------------------------------${NC}"
    else
        # Quiet mode: redirect to log file only
        if bash "$test_script" > "$log_file" 2>&1; then
            exit_code=0
        else
            exit_code=1
        fi
    fi

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    if [ $exit_code -eq 0 ]; then
        if [ "$JSON_OUTPUT" != "true" ]; then
            echo -e "${GREEN}✓ $test_name PASSED${NC} (${duration}s)"
        fi
        add_json_result "$test_name" "passed" "$duration" "Test completed successfully" "$log_file"
        return 0
    else
        if [ "$JSON_OUTPUT" != "true" ]; then
            echo -e "${RED}✗ $test_name FAILED${NC} (${duration}s)"
            if [ "$VERBOSE" != "true" ]; then
                echo "Check log file for details: $log_file"
                tail -20 "$log_file"
            fi
        fi
        # Check if it was skipped (exit 0 with skip message)
        if grep -q "SKIPPED:" "$log_file" 2>/dev/null; then
            add_json_result "$test_name" "skipped" "$duration" "Test skipped (encryption not available)" "$log_file"
        else
            add_json_result "$test_name" "failed" "$duration" "Test failed - check log file" "$log_file"
        fi
        return 1
    fi
}

# Print header (unless JSON only)
if [ "$JSON_OUTPUT" != "true" ] || [ "$VERBOSE" = "true" ]; then
    echo -e "${BLUE}==========================================="
    echo -e "IB_PARSER COMPREHENSIVE TEST SUITE"
    echo -e "===========================================${NC}"
    echo ""
    echo "This will test:"
    echo "1. Compressed tables"
    echo "2. Encrypted tables"
    echo "3. Encrypted + Compressed tables"
    echo "4. Type decoding (DECIMAL, DATE, TIME, ENUM, SET)"
    echo "5. Charset-aware decoding (latin1 + utf8mb4)"
    echo "6. JSON binary decoding (JSON columns)"
    echo "7. Secondary index parsing (index selection)"
    echo "8. LOB external decoding (LONGTEXT, LONGBLOB)"
    echo "9. ZLOB compressed decoding (ROW_FORMAT=COMPRESSED)"
    echo "10. SDI rebuild (Mode 5 with --sdi-json)"
    echo "11. SDI external rebuild (external SDI BLOB pages)"
    echo "12. CFG import (instant columns via --cfg-out)"
    echo "13. Index-id remap import (target SDI)"
    echo ""
    if [ "$VERBOSE" = "true" ]; then
        echo -e "${CYAN}Verbose mode enabled - showing real-time output${NC}"
        echo ""
    fi
fi

# Check prerequisites
if [ "$JSON_OUTPUT" != "true" ]; then
    echo -e "${YELLOW}Checking prerequisites...${NC}"
fi

# Check if MySQL/Percona is running
if ! systemctl is-active --quiet mysql; then
    if [ "$JSON_OUTPUT" = "true" ]; then
        echo '{"error":"MySQL/Percona Server is not running","tests":[]}'
        exit 1
    fi
    echo -e "${RED}Error: MySQL/Percona Server is not running${NC}"
    echo "Please start MySQL first: sudo systemctl start mysql"
    exit 1
fi

# Check if ib_parser is built
if [ ! -f "$PARSER_DIR/build/ib_parser" ]; then
    if [ "$JSON_OUTPUT" = "true" ]; then
        echo '{"error":"ib_parser not found - build required","tests":[]}'
        exit 1
    fi
    echo -e "${RED}Error: ib_parser not found${NC}"
    echo "Please build the project first:"
    echo "  cd $PARSER_DIR/build && cmake .. && make"
    exit 1
fi

# Check sudo access (needed for stopping/starting MySQL)
if ! sudo -n true 2>/dev/null; then
    if [ "$JSON_OUTPUT" != "true" ]; then
        echo -e "${YELLOW}This script requires sudo access to stop/start MySQL${NC}"
        echo "Please enter your password when prompted."
    fi
    sudo true
fi

if [ "$JSON_OUTPUT" != "true" ]; then
    echo -e "${GREEN}Prerequisites check passed${NC}"
    echo ""
fi

# Track test results
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
SKIPPED_TESTS=0

SUITE_START_TIME=$(date +%s)

# Run tests
if [ "$JSON_OUTPUT" != "true" ]; then
    echo -e "${BLUE}Starting test suite...${NC}"
    echo "=========================================="
fi

# Test 1: Compressed tables
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "COMPRESSED_TABLES" "$SCRIPT_DIR/test_compressed.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
[ "$JSON_OUTPUT" != "true" ] && echo ""

# Test 2: Encrypted tables
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "ENCRYPTED_TABLES" "$SCRIPT_DIR/test_encrypted.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
[ "$JSON_OUTPUT" != "true" ] && echo ""

# Test 3: Encrypted + Compressed tables
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "ENCRYPTED_COMPRESSED_TABLES" "$SCRIPT_DIR/test_encrypted_compressed.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
[ "$JSON_OUTPUT" != "true" ] && echo ""

# Test 4: Type decoding (DECIMAL, DATE, TIME, ENUM, SET, etc.)
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "TYPE_DECODING" "$SCRIPT_DIR/test_types_decode.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
[ "$JSON_OUTPUT" != "true" ] && echo ""

# Test 5: Charset-aware decoding (latin1 + utf8mb4)
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "CHARSET_DECODING" "$SCRIPT_DIR/test_charset_decode.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
[ "$JSON_OUTPUT" != "true" ] && echo ""

# Test 6: JSON binary decoding (JSON columns)
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "JSON_DECODING" "$SCRIPT_DIR/test_json_decode.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
[ "$JSON_OUTPUT" != "true" ] && echo ""

# Test 7: Secondary index parsing (index selection)
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "SECONDARY_INDEX" "$SCRIPT_DIR/test_secondary_index.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
[ "$JSON_OUTPUT" != "true" ] && echo ""

# Test 8: LOB external decoding (LONGTEXT, LONGBLOB)
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "LOB_DECODING" "$SCRIPT_DIR/test_lob_decode.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
[ "$JSON_OUTPUT" != "true" ] && echo ""

# Test 9: ZLOB compressed decoding (ROW_FORMAT=COMPRESSED)
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "ZLOB_DECODING" "$SCRIPT_DIR/test_zlob_decode.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
[ "$JSON_OUTPUT" != "true" ] && echo ""

# Test 10: SDI rebuild (Mode 5 with --sdi-json)
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "SDI_REBUILD" "$SCRIPT_DIR/test_sdi_rebuild.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
[ "$JSON_OUTPUT" != "true" ] && echo ""

# Test 11: SDI external rebuild (external SDI BLOB pages)
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "SDI_EXTERNAL" "$SCRIPT_DIR/test_sdi_external.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
[ "$JSON_OUTPUT" != "true" ] && echo ""

# Test 12: CFG import (instant columns via --cfg-out)
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "CFG_IMPORT" "$SCRIPT_DIR/test_cfg_import.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
[ "$JSON_OUTPUT" != "true" ] && echo ""

# Test 13: Index-id remap import (target SDI)
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "INDEX_ID_REMAP" "$SCRIPT_DIR/test_index_id_remap.sh"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi
[ "$JSON_OUTPUT" != "true" ] && echo ""

SUITE_END_TIME=$(date +%s)
SUITE_DURATION=$((SUITE_END_TIME - SUITE_START_TIME))

# Output JSON if requested
if [ "$JSON_OUTPUT" = "true" ]; then
    # Build JSON array
    JSON_TESTS=$(IFS=,; echo "${JSON_RESULTS[*]}")

    # Determine overall status
    if [ $FAILED_TESTS -eq 0 ]; then
        OVERALL_STATUS="passed"
    else
        OVERALL_STATUS="failed"
    fi

    cat << EOF
{
  "suite": "ib_parser",
  "timestamp": "$(date -Iseconds)",
  "duration_seconds": $SUITE_DURATION,
  "status": "$OVERALL_STATUS",
  "summary": {
    "total": $TOTAL_TESTS,
    "passed": $PASSED_TESTS,
    "failed": $FAILED_TESTS
  },
  "tests": [
    $JSON_TESTS
  ]
}
EOF

    if [ $FAILED_TESTS -eq 0 ]; then
        exit 0
    else
        exit 1
    fi
fi

# Summary (text mode)
echo "=========================================="
echo -e "${BLUE}TEST SUITE SUMMARY${NC}"
echo "=========================================="
echo "Total tests run: $TOTAL_TESTS"
echo -e "${GREEN}Passed: $PASSED_TESTS${NC}"
echo -e "${RED}Failed: $FAILED_TESTS${NC}"
echo "Duration: ${SUITE_DURATION}s"
echo ""

if [ $FAILED_TESTS -eq 0 ]; then
    echo -e "${GREEN}✓ ALL TESTS PASSED!${NC}"
    exit 0
else
    echo -e "${RED}✗ SOME TESTS FAILED${NC}"
    echo "Check the log files in: $LOG_DIR"
    exit 1
fi
