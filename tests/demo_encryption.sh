#!/bin/bash

# Simple demonstration of ib_parser encryption/decryption capabilities
# Shows that encrypted data becomes readable after decryption

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}IB_PARSER ENCRYPTION DEMO${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""

# Check if we have test files
if [ ! -f "encrypted_test.ibd" ] || [ ! -f "decrypted_test.ibd" ]; then
    echo -e "${YELLOW}Creating test encrypted table...${NC}"
    
    # Ensure keyring is loaded
    mysql -uroot -e "INSTALL PLUGIN keyring_file SONAME 'keyring_file.so';" 2>/dev/null || true
    mysql -uroot -e "SET GLOBAL default_table_encryption=ON;" 2>/dev/null || true
    
    # Create test database and encrypted table
    mysql -uroot <<'EOF' 2>/dev/null || true
DROP DATABASE IF EXISTS encryption_demo;
CREATE DATABASE encryption_demo;
USE encryption_demo;

CREATE TABLE secret_data (
    id INT PRIMARY KEY AUTO_INCREMENT,
    secret_info VARCHAR(100)
) ENCRYPTION='Y';

INSERT INTO secret_data (secret_info) VALUES 
    ('TOP SECRET: Project Blue Sky'),
    ('CONFIDENTIAL: Meeting at 3pm'),
    ('PRIVATE: Password is hunter2'),
    ('CLASSIFIED: Agent code X-99'),
    ('RESTRICTED: Launch codes 1234');
    
FLUSH TABLES secret_data FOR EXPORT;
EOF
    
    # Copy encrypted file
    sudo cp /var/lib/mysql/encryption_demo/secret_data.ibd encrypted_test.ibd 2>/dev/null
    sudo chown $USER:$USER encrypted_test.ibd 2>/dev/null
    
    mysql -uroot -e "UNLOCK TABLES;" 2>/dev/null
    
    # Get server info
    SERVER_UUID=$(mysql -uroot -sN -e "SELECT @@server_uuid;")
    
    # Copy keyring
    sudo cp /var/lib/mysql-keyring/keyring keyring_test 2>/dev/null
    sudo chown $USER:$USER keyring_test 2>/dev/null
    
    # Decrypt the file
    echo -e "${YELLOW}Decrypting with ib_parser...${NC}"
    ../build/ib_parser 1 1 "$SERVER_UUID" keyring_test encrypted_test.ibd decrypted_test.ibd 2>&1 | grep -E "(Successfully|master_key =)" || true
fi

echo ""
echo -e "${BLUE}1. ANALYZING ENCRYPTED FILE${NC}"
echo "----------------------------------------"

echo -e "${YELLOW}Searching for readable secrets in ENCRYPTED file:${NC}"
FOUND_IN_ENCRYPTED=0

SECRETS=("Top secret" "Confidential" "Private information" "Classified" "Secret data row")

for SECRET in "${SECRETS[@]}"; do
    if strings encrypted_test.ibd 2>/dev/null | grep -q "$SECRET"; then
        echo -e "  ${RED}✗ Found: '$SECRET' (This shouldn't happen!)${NC}"
        ((FOUND_IN_ENCRYPTED++))
    else
        echo -e "  ${GREEN}✓ NOT found: '$SECRET' (Good - data is encrypted)${NC}"
    fi
done

echo ""
echo -e "${BLUE}2. ANALYZING DECRYPTED FILE${NC}"
echo "----------------------------------------"

echo -e "${YELLOW}Searching for readable secrets in DECRYPTED file:${NC}"
FOUND_IN_DECRYPTED=0

for SECRET in "${SECRETS[@]}"; do
    if strings decrypted_test.ibd 2>/dev/null | grep -q "$SECRET"; then
        echo -e "  ${GREEN}✓ Found: '$SECRET' (Success - data is readable!)${NC}"
        ((FOUND_IN_DECRYPTED++))
    else
        echo -e "  ${RED}✗ NOT found: '$SECRET'${NC}"
    fi
done

echo ""
echo -e "${BLUE}3. ACTUAL DECRYPTED CONTENT${NC}"
echo "----------------------------------------"
echo "Sample of decrypted data:"
strings decrypted_test.ibd 2>/dev/null | grep -E "(SECRET|CONFIDENTIAL|PRIVATE|CLASSIFIED|RESTRICTED)" | sed 's/^/  /'

echo ""
echo -e "${BLUE}4. RESULTS SUMMARY${NC}"
echo "----------------------------------------"

if [ $FOUND_IN_ENCRYPTED -eq 0 ] && [ $FOUND_IN_DECRYPTED -gt 0 ]; then
    echo -e "${GREEN}✓ SUCCESS: Encryption/Decryption works perfectly!${NC}"
    echo ""
    echo "  Encrypted file: $FOUND_IN_ENCRYPTED secrets visible (none - as expected)"
    echo "  Decrypted file: $FOUND_IN_DECRYPTED secrets visible (data successfully decrypted)"
    echo ""
    echo -e "${GREEN}The ib_parser successfully decrypted the encrypted InnoDB table!${NC}"
else
    echo -e "${RED}✗ Something went wrong${NC}"
    echo "  Encrypted file: $FOUND_IN_ENCRYPTED secrets visible"
    echo "  Decrypted file: $FOUND_IN_DECRYPTED secrets visible"
fi

echo ""
echo -e "${BLUE}=========================================${NC}"
echo "Files created:"
echo "  - encrypted_test.ibd (encrypted, unreadable)"
echo "  - decrypted_test.ibd (decrypted, readable)"
echo "  - keyring_test (encryption keys)"
echo ""
echo "You can inspect these files yourself with:"
echo "  strings encrypted_test.ibd | less"
echo "  strings decrypted_test.ibd | less"
echo -e "${BLUE}=========================================${NC}"