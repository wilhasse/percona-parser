# InnoDB Reader Library Examples

Complete code examples for using the InnoDB Reader Library in various programming languages.

## Table of Contents
- [C Example](#c-example)
- [C++ Example](#c-example-1)
- [Go Example](#go-example)
- [Python Example](#python-example)
- [Real-World Use Cases](#real-world-use-cases)

## C Example

### Complete Program

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ibd_reader_api.h"

int main(int argc, char* argv[]) {
    // Check arguments
    if (argc != 3) {
        printf("Usage: %s <input.ibd> <output.ibd>\n", argv[0]);
        return 1;
    }
    
    // Initialize library
    if (ibd_init() != IBD_SUCCESS) {
        fprintf(stderr, "Failed to initialize library\n");
        return 1;
    }
    
    // Create reader
    ibd_reader_t reader = ibd_reader_create();
    if (!reader) {
        fprintf(stderr, "Failed to create reader\n");
        ibd_cleanup();
        return 1;
    }
    
    // Enable debug mode for verbose output
    ibd_reader_set_debug(reader, 1);
    
    // Decompress file
    printf("Decompressing %s to %s...\n", argv[1], argv[2]);
    ibd_result_t result = ibd_decompress_file(reader, argv[1], argv[2]);
    
    if (result == IBD_SUCCESS) {
        printf("Success!\n");
    } else {
        fprintf(stderr, "Failed: %s (code %d)\n", 
                ibd_reader_get_error(reader), result);
    }
    
    // Cleanup
    ibd_reader_destroy(reader);
    ibd_cleanup();
    
    return result == IBD_SUCCESS ? 0 : 1;
}
```

### Compile and Run

```bash
gcc -o decompress decompress.c -libd_reader
./decompress compressed.ibd output.ibd
```

## C++ Example

### Object-Oriented Wrapper

```cpp
#include <iostream>
#include <string>
#include <memory>
#include <stdexcept>

extern "C" {
    #include "ibd_reader_api.h"
}

class IbdReader {
private:
    ibd_reader_t reader_;
    static bool initialized_;
    
public:
    IbdReader() : reader_(nullptr) {
        if (!initialized_) {
            if (ibd_init() != IBD_SUCCESS) {
                throw std::runtime_error("Failed to initialize IBD library");
            }
            initialized_ = true;
        }
        
        reader_ = ibd_reader_create();
        if (!reader_) {
            throw std::runtime_error("Failed to create reader");
        }
    }
    
    ~IbdReader() {
        if (reader_) {
            ibd_reader_destroy(reader_);
        }
    }
    
    // Delete copy constructor and assignment
    IbdReader(const IbdReader&) = delete;
    IbdReader& operator=(const IbdReader&) = delete;
    
    // Move constructor
    IbdReader(IbdReader&& other) noexcept : reader_(other.reader_) {
        other.reader_ = nullptr;
    }
    
    void setDebug(bool enable) {
        ibd_reader_set_debug(reader_, enable ? 1 : 0);
    }
    
    void decompressFile(const std::string& input, const std::string& output) {
        auto result = ibd_decompress_file(reader_, input.c_str(), output.c_str());
        if (result != IBD_SUCCESS) {
            throw std::runtime_error(std::string("Decompression failed: ") + 
                                   ibd_reader_get_error(reader_));
        }
    }
    
    void decryptFile(const std::string& input, 
                    const std::string& output,
                    const std::string& keyring,
                    uint32_t masterKeyId,
                    const std::string& serverUuid) {
        auto result = ibd_decrypt_file(reader_, 
                                       input.c_str(), 
                                       output.c_str(),
                                       keyring.c_str(),
                                       masterKeyId,
                                       serverUuid.c_str());
        if (result != IBD_SUCCESS) {
            throw std::runtime_error(std::string("Decryption failed: ") + 
                                   ibd_reader_get_error(reader_));
        }
    }
    
    static void cleanup() {
        if (initialized_) {
            ibd_cleanup();
            initialized_ = false;
        }
    }
};

bool IbdReader::initialized_ = false;

// Usage example
int main() {
    try {
        IbdReader reader;
        reader.setDebug(true);
        reader.decompressFile("input.ibd", "output.ibd");
        
        // Decrypt example
        IbdReader reader2;
        reader2.decryptFile("encrypted.ibd", 
                           "decrypted.ibd",
                           "/var/lib/mysql-keyring/keyring",
                           1,
                           "550fa1f5-8821-11ef-a8c9-0242ac120002");
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    IbdReader::cleanup();
    return 0;
}
```

## Go Example

### Complete Go Bindings

Build and run the Go example:

```bash
cd examples/go
go build

# Run using the wrapper script (handles library path automatically)
./run_example.sh -i input.ibd -o output.ibd

# Or manually set the library path
LD_LIBRARY_PATH=../../build ./ibd-reader-example -i input.ibd -o output.ibd
```

**Note**: The shared library must be accessible at runtime. Use the provided `run_example.sh` wrapper script or set `LD_LIBRARY_PATH` to the build directory.

```go
package main

import (
    "flag"
    "fmt"
    "log"
    "os"
)

// #cgo CFLAGS: -I./lib
// #cgo LDFLAGS: -L./build -libd_reader -Wl,-rpath,./build
// #include "ibd_reader_api.h"
// #include <stdlib.h>
import "C"
import "unsafe"

type IbdReader struct {
    handle C.ibd_reader_t
}

// NewIbdReader creates a new reader instance
func NewIbdReader() (*IbdReader, error) {
    handle := C.ibd_reader_create()
    if handle == nil {
        return nil, fmt.Errorf("failed to create reader")
    }
    return &IbdReader{handle: handle}, nil
}

// Close destroys the reader
func (r *IbdReader) Close() {
    if r.handle != nil {
        C.ibd_reader_destroy(r.handle)
        r.handle = nil
    }
}

// SetDebug enables or disables debug mode
func (r *IbdReader) SetDebug(enable bool) {
    val := C.int(0)
    if enable {
        val = 1
    }
    C.ibd_reader_set_debug(r.handle, val)
}

// DecompressFile decompresses an IBD file
func (r *IbdReader) DecompressFile(input, output string) error {
    cInput := C.CString(input)
    cOutput := C.CString(output)
    defer C.free(unsafe.Pointer(cInput))
    defer C.free(unsafe.Pointer(cOutput))
    
    result := C.ibd_decompress_file(r.handle, cInput, cOutput)
    if result != 0 {
        errMsg := C.GoString(C.ibd_reader_get_error(r.handle))
        return fmt.Errorf("decompression failed: %s (code %d)", errMsg, result)
    }
    return nil
}

// DecryptFile decrypts an IBD file
func (r *IbdReader) DecryptFile(input, output, keyring string, 
                                masterKeyID uint32, serverUUID string) error {
    cInput := C.CString(input)
    cOutput := C.CString(output)
    cKeyring := C.CString(keyring)
    cUUID := C.CString(serverUUID)
    defer C.free(unsafe.Pointer(cInput))
    defer C.free(unsafe.Pointer(cOutput))
    defer C.free(unsafe.Pointer(cKeyring))
    defer C.free(unsafe.Pointer(cUUID))
    
    result := C.ibd_decrypt_file(r.handle, cInput, cOutput, cKeyring, 
                                 C.uint32_t(masterKeyID), cUUID)
    if result != 0 {
        errMsg := C.GoString(C.ibd_reader_get_error(r.handle))
        return fmt.Errorf("decryption failed: %s (code %d)", errMsg, result)
    }
    return nil
}

// Initialize the library
func init() {
    if C.ibd_init() != 0 {
        panic("Failed to initialize IBD library")
    }
}

func main() {
    // Parse command-line flags
    var (
        input   = flag.String("i", "", "Input IBD file")
        output  = flag.String("o", "", "Output file")
        decrypt = flag.Bool("decrypt", false, "Decrypt the file")
        keyring = flag.String("keyring", "", "Keyring file path")
        keyID   = flag.Uint("key", 0, "Master key ID")
        uuid    = flag.String("uuid", "", "Server UUID")
        debug   = flag.Bool("debug", false, "Enable debug output")
    )
    flag.Parse()
    
    if *input == "" || *output == "" {
        fmt.Fprintf(os.Stderr, "Usage: %s -i <input.ibd> -o <output.ibd>\n", os.Args[0])
        os.Exit(1)
    }
    
    // Create reader
    reader, err := NewIbdReader()
    if err != nil {
        log.Fatal(err)
    }
    defer reader.Close()
    
    // Set debug mode
    reader.SetDebug(*debug)
    
    // Process file
    if *decrypt {
        if *keyring == "" || *keyID == 0 || *uuid == "" {
            log.Fatal("Decryption requires -keyring, -key, and -uuid")
        }
        err = reader.DecryptFile(*input, *output, *keyring, uint32(*keyID), *uuid)
    } else {
        err = reader.DecompressFile(*input, *output)
    }
    
    if err != nil {
        log.Fatal(err)
    }
    
    fmt.Println("Operation completed successfully")
}
```

### Build and Run

```bash
go build -o ibd_tool main.go
# Use the wrapper script or set LD_LIBRARY_PATH
LD_LIBRARY_PATH=./build ./ibd_tool -i compressed.ibd -o output.ibd -debug
```

## Python Example

### Using ctypes

```python
#!/usr/bin/env python3
import ctypes
import sys
from pathlib import Path

class IbdReader:
    def __init__(self, lib_path='./libibd_reader.so'):
        # Load the library
        self.lib = ctypes.CDLL(lib_path)
        
        # Define function signatures
        self.lib.ibd_init.restype = ctypes.c_int
        self.lib.ibd_cleanup.restype = None
        self.lib.ibd_reader_create.restype = ctypes.c_void_p
        self.lib.ibd_reader_destroy.argtypes = [ctypes.c_void_p]
        self.lib.ibd_reader_get_error.restype = ctypes.c_char_p
        self.lib.ibd_reader_set_debug.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.ibd_decompress_file.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p
        ]
        self.lib.ibd_decompress_file.restype = ctypes.c_int
        
        # Initialize library
        if self.lib.ibd_init() != 0:
            raise RuntimeError("Failed to initialize IBD library")
        
        # Create reader
        self.reader = self.lib.ibd_reader_create()
        if not self.reader:
            raise RuntimeError("Failed to create reader")
    
    def __del__(self):
        if hasattr(self, 'reader') and self.reader:
            self.lib.ibd_reader_destroy(self.reader)
        if hasattr(self, 'lib'):
            self.lib.ibd_cleanup()
    
    def set_debug(self, enable=True):
        self.lib.ibd_reader_set_debug(self.reader, 1 if enable else 0)
    
    def decompress_file(self, input_path, output_path):
        result = self.lib.ibd_decompress_file(
            self.reader,
            input_path.encode('utf-8'),
            output_path.encode('utf-8')
        )
        
        if result != 0:
            error_msg = self.lib.ibd_reader_get_error(self.reader)
            raise RuntimeError(f"Decompression failed: {error_msg.decode('utf-8')}")
    
    def decrypt_file(self, input_path, output_path, keyring_path, 
                    master_key_id, server_uuid):
        # Define decrypt function signature
        self.lib.ibd_decrypt_file.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
            ctypes.c_char_p, ctypes.c_uint32, ctypes.c_char_p
        ]
        self.lib.ibd_decrypt_file.restype = ctypes.c_int
        
        result = self.lib.ibd_decrypt_file(
            self.reader,
            input_path.encode('utf-8'),
            output_path.encode('utf-8'),
            keyring_path.encode('utf-8'),
            master_key_id,
            server_uuid.encode('utf-8')
        )
        
        if result != 0:
            error_msg = self.lib.ibd_reader_get_error(self.reader)
            raise RuntimeError(f"Decryption failed: {error_msg.decode('utf-8')}")

# Usage example
def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.ibd> <output.ibd>")
        sys.exit(1)
    
    try:
        reader = IbdReader()
        reader.set_debug(True)
        reader.decompress_file(sys.argv[1], sys.argv[2])
        print("Decompression successful!")
        
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
```

## Real-World Use Cases

### Batch Processing Script

```bash
#!/bin/bash
# Process all IBD files in a directory

INPUT_DIR="/var/lib/mysql"
OUTPUT_DIR="/backup/decompressed"

for ibd_file in "$INPUT_DIR"/*.ibd; do
    filename=$(basename "$ibd_file")
    echo "Processing $filename..."
    
    ./ib_parser 2 "$ibd_file" "$OUTPUT_DIR/$filename"
    
    if [ $? -eq 0 ]; then
        echo "✓ $filename processed successfully"
    else
        echo "✗ Failed to process $filename"
    fi
done
```

### Database Recovery Tool

```c
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include "ibd_reader_api.h"

int recover_database(const char* db_path, const char* output_path,
                    const char* keyring_path) {
    DIR* dir;
    struct dirent* entry;
    ibd_reader_t reader;
    char input_file[PATH_MAX];
    char output_file[PATH_MAX];
    int recovered = 0, failed = 0;
    
    // Initialize
    if (ibd_init() != IBD_SUCCESS) {
        return -1;
    }
    
    reader = ibd_reader_create();
    if (!reader) {
        ibd_cleanup();
        return -1;
    }
    
    // Open directory
    dir = opendir(db_path);
    if (!dir) {
        perror("opendir");
        ibd_reader_destroy(reader);
        ibd_cleanup();
        return -1;
    }
    
    // Process each .ibd file
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".ibd") == NULL) {
            continue;
        }
        
        snprintf(input_file, sizeof(input_file), "%s/%s", 
                 db_path, entry->d_name);
        snprintf(output_file, sizeof(output_file), "%s/%s", 
                 output_path, entry->d_name);
        
        printf("Recovering %s...\n", entry->d_name);
        
        // Try to decrypt and decompress
        ibd_result_t result = ibd_decrypt_and_decompress_file(
            reader, input_file, output_file, keyring_path,
            1, "550fa1f5-8821-11ef-a8c9-0242ac120002"
        );
        
        if (result == IBD_SUCCESS) {
            printf("  ✓ Recovered successfully\n");
            recovered++;
        } else {
            // Try just decompression if decryption fails
            result = ibd_decompress_file(reader, input_file, output_file);
            if (result == IBD_SUCCESS) {
                printf("  ✓ Decompressed (not encrypted)\n");
                recovered++;
            } else {
                printf("  ✗ Failed: %s\n", ibd_reader_get_error(reader));
                failed++;
            }
        }
    }
    
    closedir(dir);
    ibd_reader_destroy(reader);
    ibd_cleanup();
    
    printf("\nRecovery complete: %d succeeded, %d failed\n", 
           recovered, failed);
    return failed > 0 ? 1 : 0;
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        printf("Usage: %s <db_path> <output_path> <keyring_path>\n", argv[0]);
        return 1;
    }
    
    return recover_database(argv[1], argv[2], argv[3]);
}
```

### Page Analysis Tool

```go
package main

import (
    "encoding/hex"
    "fmt"
    "io"
    "os"
)

func analyzeIbdFile(filename string) error {
    file, err := os.Open(filename)
    if err != nil {
        return err
    }
    defer file.Close()
    
    pageSize := 16384
    pageNum := 0
    
    for {
        page := make([]byte, pageSize)
        n, err := io.ReadFull(file, page)
        if err == io.EOF {
            break
        }
        if err != nil && err != io.ErrUnexpectedEOF {
            return err
        }
        if n < 38 {
            break
        }
        
        // Parse page header
        pageType := uint16(page[24])<<8 | uint16(page[25])
        pageNumber := uint32(page[4])<<24 | uint32(page[5])<<16 |
                     uint32(page[6])<<8 | uint32(page[7])
        
        fmt.Printf("Page %d:\n", pageNum)
        fmt.Printf("  Number: %d\n", pageNumber)
        fmt.Printf("  Type: %d (%s)\n", pageType, getPageTypeName(pageType))
        fmt.Printf("  First 64 bytes:\n")
        fmt.Printf("    %s\n", hex.EncodeToString(page[:64]))
        fmt.Println()
        
        pageNum++
    }
    
    return nil
}

func getPageTypeName(pageType uint16) string {
    switch pageType {
    case 0: return "ALLOCATED"
    case 2: return "UNDO_LOG"
    case 3: return "INODE"
    case 8: return "FSP_HDR"
    case 17855: return "INDEX"
    case 14: return "COMPRESSED"
    case 15: return "ENCRYPTED"
    default: return "UNKNOWN"
    }
}

func main() {
    if len(os.Args) != 2 {
        fmt.Fprintf(os.Stderr, "Usage: %s <file.ibd>\n", os.Args[0])
        os.Exit(1)
    }
    
    if err := analyzeIbdFile(os.Args[1]); err != nil {
        fmt.Fprintf(os.Stderr, "Error: %v\n", err)
        os.Exit(1)
    }
}
```