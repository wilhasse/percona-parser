# IB_Parser Encryption/Decryption Testing Guide

## From Zero to Hero: Complete Guide to Test Encryption/Decryption

This guide will walk you through testing the ib_parser's encryption and decryption capabilities from start to finish.

---

## Prerequisites

1. **MySQL/Percona Server 8.0+** installed and running
2. **ib_parser** built (`cd build && make`)
3. **Root access** to MySQL (no password) or adjust scripts accordingly
4. **sudo access** for accessing MySQL data files

---

## Quick Start (5 minutes)

### The Fastest Way to See It Work

```bash
cd tests/
./demo_encryption.sh
```

This will:
- ✅ Create an encrypted table with sensitive data
- ✅ Export and decrypt it using ib_parser
- ✅ Show that encrypted file has NO readable text
- ✅ Show that decrypted file HAS readable text
- ✅ Prove decryption works!

---

## Step-by-Step Manual Testing

### Step 1: Enable Encryption in MySQL

```bash
# Connect to MySQL
mysql -uroot

# Install keyring plugin (stores encryption keys)
INSTALL PLUGIN keyring_file SONAME 'keyring_file.so';

# Enable encryption by default
SET GLOBAL default_table_encryption = ON;

# Check it's enabled
SHOW VARIABLES LIKE '%encrypt%';
```

### Step 2: Create an Encrypted Table

```sql
-- Create test database
CREATE DATABASE encryption_test;
USE encryption_test;

-- Create encrypted table
CREATE TABLE secret_data (
    id INT PRIMARY KEY AUTO_INCREMENT,
    secret_info VARCHAR(255)
) ENCRYPTION='Y';

-- Insert sensitive data
INSERT INTO secret_data (secret_info) VALUES 
    ('TOP SECRET: Nuclear launch codes'),
    ('CONFIDENTIAL: CEO salary $10M'),
    ('PRIVATE: Customer SSN 123-45-6789'),
    ('CLASSIFIED: Spy satellite location'),
    ('RESTRICTED: Military base coordinates');

-- Verify data is there
SELECT * FROM secret_data;
```

### Step 3: Export the Encrypted Table

```bash
# In MySQL, prepare for export
mysql -uroot encryption_test -e "FLUSH TABLES secret_data FOR EXPORT;"

# Copy the encrypted .ibd file
sudo cp /var/lib/mysql/encryption_test/secret_data.ibd ~/encrypted_table.ibd
sudo chown $USER:$USER ~/encrypted_table.ibd

# Unlock tables
mysql -uroot encryption_test -e "UNLOCK TABLES;"
```

### Step 4: Get Encryption Keys

```bash
# Get server UUID (needed for decryption)
SERVER_UUID=$(mysql -uroot -sN -e "SELECT @@server_uuid;")
echo "Server UUID: $SERVER_UUID"

# Find and copy keyring file
KEYRING_FILE="/var/lib/mysql-keyring/keyring"
sudo cp $KEYRING_FILE ~/keyring_backup
sudo chown $USER:$USER ~/keyring_backup
```

### Step 5: Verify File is Encrypted

```bash
# Try to find secrets in encrypted file (should find NOTHING)
echo "Searching for secrets in ENCRYPTED file:"
strings ~/encrypted_table.ibd | grep -i "secret\|confidential\|private"
# Result: Nothing found - data is encrypted! ✅
```

### Step 6: Decrypt Using ib_parser

```bash
# Decrypt the file
# Syntax: ib_parser 1 <master_key_id> <server_uuid> <keyring_file> <encrypted.ibd> <decrypted.ibd>

../build/ib_parser 1 1 "$SERVER_UUID" ~/keyring_backup ~/encrypted_table.ibd ~/decrypted_table.ibd

# If master_key_id 1 doesn't work, try 2:
# ../build/ib_parser 1 2 "$SERVER_UUID" ~/keyring_backup ~/encrypted_table.ibd ~/decrypted_table.ibd
```

### Step 7: Verify Decryption Worked

```bash
# Search for secrets in DECRYPTED file (should find ALL of them)
echo "Searching for secrets in DECRYPTED file:"
strings ~/decrypted_table.ibd | grep -i "secret\|confidential\|private"
# Result: All secrets visible - decryption worked! ✅
```

---

## Using the Inspector Tool

The `ibd_text_inspector.sh` tool provides detailed analysis:

```bash
# Inspect encrypted file (finds nothing)
./ibd_text_inspector.sh ~/encrypted_table.ibd "SECRET" "CONFIDENTIAL"

# Inspect decrypted file (finds everything)
./ibd_text_inspector.sh ~/decrypted_table.ibd "SECRET" "CONFIDENTIAL"
```

Output shows:
- File size and page count
- Entropy analysis (high = encrypted, low = decrypted)
- Text search results with page numbers
- Clear indication if decryption succeeded

---

## Testing Different Scenarios

### Test 1: Simple Encryption
```bash
./demo_encryption.sh
```

### Test 2: Full Encryption Test Suite
```bash
./test_encrypted.sh
```

### Test 3: Compressed Tables
```bash
./test_compressed.sh
```

### Test 4: Encrypted + Compressed
```bash
./test_encrypted_compressed.sh
```

### Test 5: Run All Tests
```bash
./run_all_tests.sh
```

---

## Understanding the Results

### ✅ **Successful Decryption:**
- Encrypted file: No readable text found
- Decrypted file: All original text is readable
- Inspector shows normal entropy (not random)
- Specific search terms are found

### ❌ **Failed Decryption:**
- Decrypted file still has no readable text
- Inspector shows high entropy (still random)
- Search terms not found
- Possible causes:
  - Wrong master_key_id (try 1, then 2)
  - Wrong server UUID
  - Corrupted keyring file
  - Table wasn't actually encrypted

---

## Troubleshooting

### Problem: "Can't find master key from keyring"
**Solution:** Install keyring plugin first:
```sql
INSTALL PLUGIN keyring_file SONAME 'keyring_file.so';
```

### Problem: "No such key in container"
**Solution:** Try different master_key_id:
```bash
# Try with ID 2 instead of 1
../build/ib_parser 1 2 "$SERVER_UUID" keyring encrypted.ibd decrypted.ibd
```

### Problem: Can't find keyring file
**Solution:** Check alternative locations:
```bash
find /var/lib -name "keyring" 2>/dev/null
mysql -uroot -e "SELECT @@keyring_file_data;"
```

### Problem: Permission denied accessing files
**Solution:** Use sudo for MySQL data files:
```bash
sudo cp /var/lib/mysql/... 
sudo chown $USER:$USER ...
```

---

## What's Actually Happening?

1. **Encryption**: MySQL encrypts data pages using AES-256
2. **Keyring**: Stores the master encryption key
3. **Export**: Copies the encrypted .ibd file
4. **ib_parser Decryption**:
   - Reads the master key from keyring
   - Extracts tablespace key from .ibd file
   - Decrypts each page
   - Writes decrypted data
5. **Verification**: Readable text proves decryption worked

---

## Clean Up

```bash
# Remove test files
rm -f ~/encrypted_table.ibd ~/decrypted_table.ibd ~/keyring_backup

# Drop test database
mysql -uroot -e "DROP DATABASE IF EXISTS encryption_test;"
```

---

## Key Points

✅ **ib_parser successfully decrypts encrypted InnoDB tables**  
✅ **No need to import back to MySQL to verify**  
✅ **Text inspection proves decryption works**  
✅ **Supports Percona Server and MySQL encryption**  

---

## Questions?

- Check the demo: `./demo_encryption.sh`
- Inspect files: `./ibd_text_inspector.sh <file>`
- Run all tests: `./run_all_tests.sh`

The ib_parser is a powerful tool for:
- Data recovery from encrypted databases
- Security auditing
- Migration from encrypted systems
- Database forensics (with proper authorization)