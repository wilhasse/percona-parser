#!/bin/bash
#
# Test runner for percona-parser
# Usage: ./tests/run_tests.sh [--verbose]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PARSER="$PROJECT_DIR/build/ib_parser"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

VERBOSE=0
TESTS_PASSED=0
TESTS_FAILED=0

if [[ "$1" == "--verbose" || "$1" == "-v" ]]; then
    VERBOSE=1
fi

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    TESTS_PASSED=$((TESTS_PASSED + 1))
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    TESTS_FAILED=$((TESTS_FAILED + 1))
}

log_info() {
    if [[ $VERBOSE -eq 1 ]]; then
        echo -e "${YELLOW}[INFO]${NC} $1"
    fi
}

# Check if parser is built
if [[ ! -x "$PARSER" ]]; then
    echo -e "${RED}Error: Parser not found at $PARSER${NC}"
    echo "Run 'make' in build directory first"
    exit 1
fi

echo "================================"
echo "  Percona Parser Test Suite"
echo "================================"
echo ""

# Test 1: Parse rebuilt (uncompressed) file
echo "Test 1: Parse rebuilt 16KB file"
RECORD_COUNT=$(${PARSER} 3 "$SCRIPT_DIR/compressed_test_rebuilt.ibd" "$SCRIPT_DIR/compressed_test_sdi.json" 2>&1 | grep -c "Found record" || true)
if [[ "$RECORD_COUNT" -eq 55 ]]; then
    log_pass "Found $RECORD_COUNT records (expected 55)"
else
    log_fail "Found $RECORD_COUNT records (expected 55)"
fi

# Test 2: Parse compressed file
echo "Test 2: Parse compressed 8KB file"
RECORD_COUNT=$(${PARSER} 3 "$SCRIPT_DIR/compressed_test.ibd" "$SCRIPT_DIR/compressed_test_sdi.json" 2>&1 | grep -c "Found record" || true)
if [[ "$RECORD_COUNT" -eq 55 ]]; then
    log_pass "Found $RECORD_COUNT records (expected 55)"
else
    log_fail "Found $RECORD_COUNT records (expected 55)"
fi

# Test 3: Verify first record values
echo "Test 3: Verify first record field values"
OUTPUT=$(${PARSER} 3 "$SCRIPT_DIR/compressed_test_rebuilt.ibd" "$SCRIPT_DIR/compressed_test_sdi.json" 2>&1 | grep "^1|Product A|")
if [[ -n "$OUTPUT" ]]; then
    log_pass "First record: id=1, name='Product A'"
    log_info "Full record: $OUTPUT"
else
    log_fail "First record values incorrect"
fi

# Test 4: Verify DATETIME format
echo "Test 4: Verify DATETIME decoding"
OUTPUT=$(${PARSER} 3 "$SCRIPT_DIR/compressed_test_rebuilt.ibd" "$SCRIPT_DIR/compressed_test_sdi.json" 2>&1 | grep "2025-12-28 18:31:37" | head -1)
if [[ -n "$OUTPUT" ]]; then
    log_pass "DATETIME decoded correctly (2025-12-28 18:31:37)"
else
    log_fail "DATETIME not decoded correctly"
fi

# Test 5: Verify internal columns hidden in default mode
echo "Test 5: Internal columns hidden in default output"
OUTPUT=$(${PARSER} 3 "$SCRIPT_DIR/compressed_test_rebuilt.ibd" "$SCRIPT_DIR/compressed_test_sdi.json" 2>&1 | grep "^id|" | head -1)
if [[ "$OUTPUT" == "id|name|description|created_at" ]]; then
    log_pass "Header shows 4 user columns (internal hidden)"
else
    log_fail "Header incorrect: $OUTPUT"
fi

# Test 6: Verify debug mode shows internal columns
echo "Test 6: Debug mode shows internal columns"
OUTPUT=$(IB_PARSER_DEBUG=1 ${PARSER} 3 "$SCRIPT_DIR/compressed_test_rebuilt.ibd" "$SCRIPT_DIR/compressed_test_sdi.json" 2>&1 | grep "DB_TRX_ID" | head -1)
if [[ -n "$OUTPUT" ]]; then
    log_pass "Debug mode shows DB_TRX_ID"
else
    log_fail "Debug mode missing internal columns"
fi

# Test 7: Compressed and rebuilt produce same record count
echo "Test 7: Compressed/rebuilt parity"
COUNT_REBUILT=$(${PARSER} 3 "$SCRIPT_DIR/compressed_test_rebuilt.ibd" "$SCRIPT_DIR/compressed_test_sdi.json" 2>&1 | grep -c "Found record" || true)
COUNT_COMPRESSED=$(${PARSER} 3 "$SCRIPT_DIR/compressed_test.ibd" "$SCRIPT_DIR/compressed_test_sdi.json" 2>&1 | grep -c "Found record" || true)
if [[ "$COUNT_REBUILT" -eq "$COUNT_COMPRESSED" ]]; then
    log_pass "Both files yield $COUNT_REBUILT records"
else
    log_fail "Mismatch: rebuilt=$COUNT_REBUILT, compressed=$COUNT_COMPRESSED"
fi

# Test 8: Mode 5 rebuild functionality
echo "Test 8: Mode 5 rebuild compressed to uncompressed"
TEMP_OUT=$(mktemp)
${PARSER} 5 "$SCRIPT_DIR/compressed_test.ibd" "$TEMP_OUT" 2>&1 | grep -q "REBUILD COMPLETE" && \
    log_pass "Mode 5 rebuild successful" || \
    log_fail "Mode 5 rebuild failed"
rm -f "$TEMP_OUT"

# Test 9: CSV output format
echo "Test 9: CSV output format (--format=csv)"
OUTPUT=$(${PARSER} 3 "$SCRIPT_DIR/compressed_test_rebuilt.ibd" "$SCRIPT_DIR/compressed_test_sdi.json" --format=csv 2>&1 | grep '^1,Product A,' | head -1)
if [[ -n "$OUTPUT" ]]; then
    log_pass "CSV format works (comma-separated)"
    log_info "CSV line: $OUTPUT"
else
    log_fail "CSV format not working"
fi

# Test 10: JSONL output format
echo "Test 10: JSONL output format (--format=jsonl)"
OUTPUT=$(${PARSER} 3 "$SCRIPT_DIR/compressed_test_rebuilt.ibd" "$SCRIPT_DIR/compressed_test_sdi.json" --format=jsonl 2>&1 | grep '"id":1' | head -1)
if [[ -n "$OUTPUT" ]]; then
    log_pass "JSONL format works (JSON objects)"
    log_info "JSONL line: $OUTPUT"
else
    log_fail "JSONL format not working"
fi

# Test 11: --with-meta adds page/record metadata
echo "Test 11: Metadata output (--with-meta)"
OUTPUT=$(${PARSER} 3 "$SCRIPT_DIR/compressed_test_rebuilt.ibd" "$SCRIPT_DIR/compressed_test_sdi.json" --format=jsonl --with-meta 2>&1 | grep '"page_no"' | head -1)
if [[ -n "$OUTPUT" ]]; then
    log_pass "--with-meta adds page_no/rec_offset"
    log_info "Meta line: $OUTPUT"
else
    log_fail "--with-meta not adding metadata"
fi

# Test 12: Output to file (--output)
echo "Test 12: Output to file (--output=PATH)"
TEMP_OUT=$(mktemp)
${PARSER} 3 "$SCRIPT_DIR/compressed_test_rebuilt.ibd" "$SCRIPT_DIR/compressed_test_sdi.json" --format=csv --output="$TEMP_OUT" >/dev/null 2>&1
LINE_COUNT=$(wc -l < "$TEMP_OUT")
if [[ "$LINE_COUNT" -eq 56 ]]; then  # 55 records + 1 header
    log_pass "File output: $LINE_COUNT lines (55 records + header)"
else
    log_fail "File output incorrect: $LINE_COUNT lines"
fi
rm -f "$TEMP_OUT"

echo ""
echo "================================"
echo "  Results: $TESTS_PASSED passed, $TESTS_FAILED failed"
echo "================================"

if [[ $TESTS_FAILED -gt 0 ]]; then
    exit 1
fi
exit 0
