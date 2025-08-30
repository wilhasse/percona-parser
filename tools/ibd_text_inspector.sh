#!/bin/bash

# InnoDB Page Text Inspector
# Simple tool to search for text strings in .ibd files to verify decryption/decompression

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to display usage
usage() {
    echo "Usage: $0 <ibd_file> [search_terms...]"
    echo ""
    echo "Inspects an InnoDB .ibd file for readable text"
    echo ""
    echo "Arguments:"
    echo "  ibd_file       Path to the .ibd file to inspect"
    echo "  search_terms   Optional: specific terms to search for"
    echo ""
    echo "Examples:"
    echo "  $0 decrypted.ibd                    # Show all readable strings"
    echo "  $0 decrypted.ibd 'secret' 'private' # Search for specific terms"
    exit 1
}

# Check arguments
if [ $# -lt 1 ]; then
    usage
fi

IBD_FILE="$1"
shift

# Check if file exists
if [ ! -f "$IBD_FILE" ]; then
    echo -e "${RED}Error: File '$IBD_FILE' not found${NC}"
    exit 1
fi

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}InnoDB Page Text Inspector${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""

# Get file info
FILE_SIZE=$(stat -c%s "$IBD_FILE")
PAGE_SIZE=16384
NUM_PAGES=$((FILE_SIZE / PAGE_SIZE))

echo -e "${YELLOW}File Information:${NC}"
echo "  File: $IBD_FILE"
echo "  Size: $FILE_SIZE bytes"
echo "  Pages: $NUM_PAGES (16KB each)"
echo ""

# Function to check if a page contains data
page_has_data() {
    local page_num=$1
    local offset=$((page_num * PAGE_SIZE))
    
    # Read page and check if it has non-zero content
    dd if="$IBD_FILE" bs=$PAGE_SIZE skip=$page_num count=1 2>/dev/null | \
        hexdump -C | grep -qv "00 00 00 00 00 00 00 00"
}

# Function to extract readable strings from a page
extract_page_strings() {
    local page_num=$1
    local offset=$((page_num * PAGE_SIZE))
    
    # Extract strings (minimum 4 characters) from the page
    dd if="$IBD_FILE" bs=$PAGE_SIZE skip=$page_num count=1 2>/dev/null | \
        strings -n 4
}

# If no search terms provided, show general statistics
if [ $# -eq 0 ]; then
    echo -e "${YELLOW}Searching for readable text in all pages...${NC}"
    echo ""
    
    FOUND_COUNT=0
    
    for ((page=0; page<NUM_PAGES; page++)); do
        STRINGS=$(extract_page_strings $page)
        if [ ! -z "$STRINGS" ]; then
            LINE_COUNT=$(echo "$STRINGS" | wc -l)
            if [ $LINE_COUNT -gt 2 ]; then  # Skip pages with only metadata
                echo -e "${GREEN}Page $page: Found $LINE_COUNT readable strings${NC}"
                echo "$STRINGS" | head -10 | sed 's/^/  /'
                if [ $LINE_COUNT -gt 10 ]; then
                    echo "  ... and $((LINE_COUNT - 10)) more"
                fi
                echo ""
                ((FOUND_COUNT++))
            fi
        fi
    done
    
    echo -e "${BLUE}Summary: Found readable text in $FOUND_COUNT pages${NC}"
    
else
    # Search for specific terms
    echo -e "${YELLOW}Searching for specific terms...${NC}"
    echo ""
    
    TOTAL_MATCHES=0
    
    for TERM in "$@"; do
        echo -e "${BLUE}Searching for: '$TERM'${NC}"
        TERM_MATCHES=0
        
        for ((page=0; page<NUM_PAGES; page++)); do
            MATCHES=$(extract_page_strings $page | grep -i "$TERM" 2>/dev/null || true)
            if [ ! -z "$MATCHES" ]; then
                MATCH_COUNT=$(echo "$MATCHES" | wc -l)
                echo -e "  ${GREEN}Page $page: $MATCH_COUNT matches${NC}"
                echo "$MATCHES" | head -5 | sed 's/^/    /'
                if [ $MATCH_COUNT -gt 5 ]; then
                    echo "    ... and $((MATCH_COUNT - 5)) more"
                fi
                ((TERM_MATCHES += MATCH_COUNT))
            fi
        done
        
        if [ $TERM_MATCHES -eq 0 ]; then
            echo -e "  ${RED}No matches found${NC}"
        else
            echo -e "  ${GREEN}Total: $TERM_MATCHES matches${NC}"
        fi
        echo ""
        
        ((TOTAL_MATCHES += TERM_MATCHES))
    done
    
    echo -e "${BLUE}=========================================${NC}"
    echo -e "${BLUE}Total matches found: $TOTAL_MATCHES${NC}"
fi

# Check for encryption/compression indicators
echo ""
echo -e "${YELLOW}File Analysis:${NC}"

# Check if file appears encrypted (looking for random data patterns)
ENTROPY=$(dd if="$IBD_FILE" bs=16384 skip=4 count=1 2>/dev/null | \
          od -An -tx1 | tr -d ' \n' | \
          fold -w2 | sort | uniq -c | wc -l)

if [ $ENTROPY -gt 200 ]; then
    echo -e "  ${RED}⚠ File appears to be encrypted (high entropy)${NC}"
else
    echo -e "  ${GREEN}✓ File appears to be decrypted (normal entropy)${NC}"
fi

# Check for readable table data
TABLE_DATA=$(strings -n 10 "$IBD_FILE" | head -20)
if [ ! -z "$TABLE_DATA" ]; then
    echo -e "  ${GREEN}✓ Readable data found${NC}"
else
    echo -e "  ${RED}⚠ No readable data found${NC}"
fi

echo ""
echo -e "${BLUE}Inspection complete!${NC}"