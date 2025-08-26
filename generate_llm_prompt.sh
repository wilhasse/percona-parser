#!/bin/bash

# Script to generate a single file with all C++ source code for LLM prompting
# This creates a consolidated view of the percona-parser codebase for analysis
# Includes .cc, .h files and documentation

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Output configuration
OUTPUT_DIR="."
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
OUTPUT_FILE="percona-parser-codebase-${TIMESTAMP}.txt"

# Function to print colored output
print_color() {
    local color=$1
    shift
    echo -e "${color}$@${NC}"
}

echo "📦 Generating consolidated percona-parser codebase for LLM prompting..."

# Function to add a file with header
add_file() {
    local filepath=$1
    local description=$2
    
    if [ -f "$filepath" ]; then
        echo "" >> "$OUTPUT_FILE"
        echo "================================================================================" >> "$OUTPUT_FILE"
        echo "// File: $filepath" >> "$OUTPUT_FILE"
        echo "// Description: $description" >> "$OUTPUT_FILE"
        echo "================================================================================" >> "$OUTPUT_FILE"
        echo "" >> "$OUTPUT_FILE"
        cat "$filepath" >> "$OUTPUT_FILE"
    fi
}

# Start with a header
cat > "$OUTPUT_FILE" << 'EOF'
# Percona Parser Codebase
# InnoDB data recovery and parsing tools for C++
# Generated for LLM analysis and understanding
# 
# This project provides tools for parsing and recovering data from InnoDB 
# database files, including encrypted and compressed pages.
#
# ⚠️ THIS IS A GENERATED FILE - DO NOT EDIT
# ⚠️ Generated from actual source files

## Project Structure Overview:
- Core parsers: parser.cc/h, ib_parser.cc
- Encryption: decrypt.cc/h, ibd_enc_reader.cc/h
- Compression: decompress.cc/h
- Recovery: undrop_for_innodb.cc/h
- Database schema: tables_dict.cc/h
- Utilities: mysql_crc32c.cc/h, keyring support

## Key Features:
- Parse InnoDB page structure and extract records
- Handle encrypted InnoDB tablespaces
- Decompress compressed InnoDB pages
- Recover dropped tables from InnoDB files
- Support for various MySQL/Percona keyring systems
- CRC32c checksum validation

================================================================================

EOF

# Add main documentation files
add_file "README.md" "Project documentation and usage examples"
add_file "CMakeLists.txt" "CMake build configuration"

# Add header files
print_color $BLUE "Adding header files..."
add_file "parser.h" "Core parser header definitions"
add_file "undrop_for_innodb.h" "InnoDB recovery utilities header"
add_file "tables_dict.h" "Database schema dictionary header"
add_file "decrypt.h" "Encryption/decryption utilities header"
add_file "decompress.h" "Compression utilities header"
add_file "ibd_enc_reader.h" "Encrypted InnoDB file reader header"
add_file "mysql_crc32c.h" "CRC32c checksum implementation header"
add_file "my_keyring_lookup.h" "Keyring lookup utilities header"

# Add implementation files
print_color $BLUE "Adding implementation files..."
add_file "parser.cc" "Core parser implementation"
add_file "ib_parser.cc" "InnoDB page parser implementation"
add_file "undrop_for_innodb.cc" "InnoDB recovery utilities"
add_file "tables_dict.cc" "Database schema dictionary"
add_file "decrypt.cc" "Encryption/decryption implementation"
add_file "decompress.cc" "Compression utilities implementation"
add_file "ibd_enc_reader.cc" "Encrypted InnoDB file reader"
add_file "mysql_crc32c.cc" "CRC32c checksum implementation"
add_file "my_keyring_lookup.cc" "Keyring lookup implementation"
add_file "keyring_stubs.cc" "Keyring stub implementations"

# Add footer with statistics
echo "" >> "$OUTPUT_FILE"
echo "================================================================================" >> "$OUTPUT_FILE"
echo "// Code Statistics" >> "$OUTPUT_FILE"
echo "================================================================================" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"

# Count lines and files
TOTAL_LINES=$(wc -l < "$OUTPUT_FILE")
CC_FILES=$(find . -name "*.cc" 2>/dev/null | wc -l)
H_FILES=$(find . -name "*.h" 2>/dev/null | wc -l)
TOTAL_SOURCE_FILES=$((CC_FILES + H_FILES))

echo "Total lines in this document: $TOTAL_LINES" >> "$OUTPUT_FILE"
echo "Total C++ implementation files (.cc): $CC_FILES" >> "$OUTPUT_FILE"
echo "Total header files (.h): $H_FILES" >> "$OUTPUT_FILE"
echo "Total source files: $TOTAL_SOURCE_FILES" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"
echo "Generated at: $(date)" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"

echo "⚠️ IMPORTANT NOTES FOR LLM:" >> "$OUTPUT_FILE"
echo "================================" >> "$OUTPUT_FILE"
echo "1. This is a SINGLE GENERATED FILE containing multiple source files concatenated" >> "$OUTPUT_FILE"
echo "2. Each section marked with '// File:' is a DIFFERENT source file" >> "$OUTPUT_FILE"
echo "3. These are NOT multiple versions - they are DIFFERENT files from the project" >> "$OUTPUT_FILE"
echo "4. The actual source code is in the project root directory" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"

echo "Key Implementation Details:" >> "$OUTPUT_FILE"
echo "1. InnoDB pages are always 16KB (16384 bytes)" >> "$OUTPUT_FILE"
echo "2. Support for encrypted InnoDB tablespaces" >> "$OUTPUT_FILE"
echo "3. Support for compressed InnoDB pages" >> "$OUTPUT_FILE"
echo "4. Data recovery from dropped InnoDB tables" >> "$OUTPUT_FILE"
echo "5. Integration with MySQL/Percona keyring systems" >> "$OUTPUT_FILE"
echo "6. CRC32c checksums for data integrity" >> "$OUTPUT_FILE"
echo "7. Handles both file-per-table and system tablespaces" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"

echo "Build Instructions:" >> "$OUTPUT_FILE"
echo "  mkdir build && cd build" >> "$OUTPUT_FILE"
echo "  cmake .." >> "$OUTPUT_FILE"
echo "  make" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"
echo "This will compile the Percona parser tools for InnoDB data recovery." >> "$OUTPUT_FILE"

print_color $GREEN ""
print_color $GREEN "✅ Generated: $OUTPUT_FILE"
print_color $GREEN "📊 File size: $(du -h "$OUTPUT_FILE" | cut -f1)"
print_color $GREEN "📝 Total lines: $TOTAL_LINES"
print_color $GREEN ""
print_color $GREEN "You can now use this file to prompt an LLM with the complete codebase context."
print_color $YELLOW ""
print_color $YELLOW "💡 Tip: Add this file to .gitignore to avoid committing it:"
print_color $YELLOW "    echo 'percona-parser-codebase-*.txt' >> .gitignore"