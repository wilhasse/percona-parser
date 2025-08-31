# InnoDB Reader Library Examples

Complete code examples for using the InnoDB Reader Library in various programming languages.

## Table of Contents
- [C Example](#c-example)
- [C++ Example](#c-example-1)
- [Go Example](#go-example)
- [Python Example](#python-example)
- [Real-World Use Cases](#real-world-use-cases)
- [Known Limitations](#known-limitations)

## C Example

### Complete Decompression Program

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ibd_reader_api.h"

int main(int argc, char* argv[]) {
    // Check arguments
    if (argc != 3) {
        printf("Usage: %s <compressed.ibd> <output.ibd>\n", argv[0]);
        printf("Note: Decompressed files cannot be imported back to MySQL\n");
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
    printf("Warning: Output file will have mixed page sizes\n");
    ibd_result_t result = ibd_decompress_file(reader, argv[1], argv[2]);
    
    if (result == IBD_SUCCESS) {
        printf("Success! INDEX pages decompressed, metadata pages preserved\n");
        printf("Note: File cannot be imported due to ROW_FORMAT mismatch\n");
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
gcc -o decompress decompress.c -L./build -libd_reader -I./lib
LD_LIBRARY_PATH=./build ./decompress compressed.ibd output.ibd
```

## C++ Example

### Object-Oriented Wrapper with Error Handling

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
        std::cout << "Decompressing compressed table..." << std::endl;
        std::cout << "Note: Output will have COMPRESSED metadata but uncompressed data" << std::endl;
        
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
        // Decompress a compressed table
        IbdReader reader;
        reader.setDebug(true);
        reader.decompressFile("compressed_table.ibd", "decompressed.ibd");
        std::cout << "Decompression successful!" << std::endl;
        std::cout << "Warning: Cannot import back to MySQL due to metadata" << std::endl;
        
        // Decrypt an encrypted table
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

### Complete Go Implementation

```go
package main

import (
    "flag"
    "fmt"
    "log"
    "os"
)

// #cgo CFLAGS: -I../../lib
// #cgo LDFLAGS: -L../../build -libd_reader
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

// DecompressFile decompresses a compressed IBD file
func (r *IbdReader) DecompressFile(input, output string) error {
    fmt.Println("Decompressing ROW_FORMAT=COMPRESSED table...")
    fmt.Println("Note: Output retains COMPRESSED metadata, cannot be imported")
    
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

// Initialize the library
func init() {
    if C.ibd_init() != 0 {
        panic("Failed to initialize IBD library")
    }
}

func main() {
    // Parse command-line flags
    var (
        input   = flag.String("i", "", "Input compressed IBD file")
        output  = flag.String("o", "", "Output decompressed file")
        debug   = flag.Bool("debug", false, "Enable debug output")
    )
    flag.Parse()
    
    if *input == "" || *output == "" {
        fmt.Fprintf(os.Stderr, "Usage: %s -i <compressed.ibd> -o <output.ibd>\n", os.Args[0])
        fmt.Fprintf(os.Stderr, "\nNote: Decompressed files cannot be imported back to MySQL\n")
        fmt.Fprintf(os.Stderr, "      due to ROW_FORMAT metadata mismatch\n")
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
    
    // Decompress file
    err = reader.DecompressFile(*input, *output)
    if err != nil {
        log.Fatal(err)
    }
    
    fmt.Println("Decompression completed successfully!")
    fmt.Println("INDEX pages expanded from 8KB to 16KB")
    fmt.Println("Metadata pages kept at physical size")
}
```

### Build and Run

```bash
cd examples/go
go build

# Use the wrapper script (recommended)
./run_example.sh -i compressed.ibd -o output.ibd -debug

# Or set library path manually
LD_LIBRARY_PATH=../../build ./ibd-reader-example -i compressed.ibd -o output.ibd
```

## Python Example

### Using ctypes with Error Handling

```python
#!/usr/bin/env python3
import ctypes
import sys
import os
from pathlib import Path

class IbdReader:
    def __init__(self, lib_path='./build/libibd_reader.so'):
        # Load the library
        if not os.path.exists(lib_path):
            raise FileNotFoundError(f"Library not found at {lib_path}")
            
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
        """
        Decompress a ROW_FORMAT=COMPRESSED table.
        Note: Output file will have COMPRESSED metadata but uncompressed data,
        preventing direct MySQL import.
        """
        print(f"Decompressing {input_path}...")
        print("Warning: Output cannot be imported back to MySQL")
        
        result = self.lib.ibd_decompress_file(
            self.reader,
            input_path.encode('utf-8'),
            output_path.encode('utf-8')
        )
        
        if result != 0:
            error_msg = self.lib.ibd_reader_get_error(self.reader)
            raise RuntimeError(f"Decompression failed: {error_msg.decode('utf-8')}")

# Usage example
def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <compressed.ibd> <output.ibd>")
        print("\nNote: Decompressed files retain COMPRESSED metadata")
        print("      and cannot be imported back to MySQL")
        sys.exit(1)
    
    try:
        reader = IbdReader()
        reader.set_debug(True)
        reader.decompress_file(sys.argv[1], sys.argv[2])
        print("Decompression successful!")
        print("INDEX pages expanded, metadata pages preserved")
        
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
```

## Real-World Use Cases

### Batch Processing Script for Compressed Tables

```bash
#!/bin/bash
# Process all compressed IBD files in a directory
# Note: Output files cannot be imported back to MySQL

INPUT_DIR="/var/lib/mysql/compressed_db"
OUTPUT_DIR="/backup/decompressed"
LOG_FILE="/var/log/decompression.log"

echo "Starting batch decompression at $(date)" >> "$LOG_FILE"
echo "WARNING: Decompressed files retain COMPRESSED metadata" >> "$LOG_FILE"

for ibd_file in "$INPUT_DIR"/*.ibd; do
    if [ ! -f "$ibd_file" ]; then
        continue
    fi
    
    filename=$(basename "$ibd_file")
    echo "Processing $filename..." | tee -a "$LOG_FILE"
    
    ./build/ib_parser 2 "$ibd_file" "$OUTPUT_DIR/$filename" 2>&1 | tee -a "$LOG_FILE"
    
    if [ ${PIPESTATUS[0]} -eq 0 ]; then
        echo "✓ $filename decompressed (cannot be imported)" | tee -a "$LOG_FILE"
    else
        echo "✗ Failed to decompress $filename" | tee -a "$LOG_FILE"
    fi
done

echo "Batch processing complete at $(date)" >> "$LOG_FILE"
```

### Database Recovery Tool with Limitations Check

```c
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "ibd_reader_api.h"

int check_compressed_table(const char* ibd_path) {
    // Check if file is from a compressed table
    // This is a simplified check - real implementation would read FSP header
    FILE* f = fopen(ibd_path, "rb");
    if (!f) return -1;
    
    // Read FSP header flags
    fseek(f, 54, SEEK_SET);
    uint16_t flags;
    fread(&flags, 2, 1, f);
    fclose(f);
    
    // Check compression flag (simplified)
    return (flags & 0x8000) ? 1 : 0;
}

int process_ibd_file(const char* input, const char* output) {
    ibd_reader_t reader;
    ibd_result_t result;
    
    // Initialize
    if (ibd_init() != IBD_SUCCESS) {
        fprintf(stderr, "Failed to initialize library\n");
        return -1;
    }
    
    reader = ibd_reader_create();
    if (!reader) {
        ibd_cleanup();
        return -1;
    }
    
    // Check if compressed
    int is_compressed = check_compressed_table(input);
    
    if (is_compressed == 1) {
        printf("File is compressed (ROW_FORMAT=COMPRESSED)\n");
        printf("Decompressing...\n");
        
        result = ibd_decompress_file(reader, input, output);
        
        if (result == IBD_SUCCESS) {
            printf("✓ Decompression successful\n");
            printf("⚠ WARNING: Output file cannot be imported to MySQL\n");
            printf("  Reason: ROW_FORMAT metadata mismatch\n");
            printf("  The file has COMPRESSED metadata but uncompressed data\n");
        } else {
            fprintf(stderr, "✗ Decompression failed: %s\n", 
                    ibd_reader_get_error(reader));
        }
    } else {
        printf("File is not compressed, copying as-is\n");
        // Just copy the file
        result = IBD_SUCCESS;
    }
    
    ibd_reader_destroy(reader);
    ibd_cleanup();
    
    return result == IBD_SUCCESS ? 0 : -1;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: %s <input.ibd> <output.ibd>\n", argv[0]);
        printf("\nLimitations:\n");
        printf("- Decompressed files cannot be imported back to MySQL\n");
        printf("- ROW_FORMAT metadata is preserved in headers\n");
        printf("- Output has mixed page sizes (INDEX at 16KB, metadata at 8KB)\n");
        return 1;
    }
    
    return process_ibd_file(argv[1], argv[2]);
}
```

### Testing Decompression Results

```python
#!/usr/bin/env python3
"""
Verify decompression results and show limitations
"""

import sys
import os

def verify_decompression(original_file, decompressed_file):
    """
    Verify that decompression worked but show why import fails
    """
    # Check file sizes
    orig_size = os.path.getsize(original_file)
    decomp_size = os.path.getsize(decompressed_file)
    
    print(f"Original size: {orig_size} bytes")
    print(f"Decompressed size: {decomp_size} bytes")
    
    if decomp_size > orig_size:
        print("✓ File expanded after decompression")
        expansion = decomp_size - orig_size
        print(f"  Expansion: {expansion} bytes")
        
        # Check for typical expansion pattern
        if expansion == 8192:  # One INDEX page expanded
            print("✓ Typical single INDEX page expansion detected")
        elif expansion % 8192 == 0:
            pages = expansion // 8192
            print(f"✓ {pages} INDEX pages expanded")
    else:
        print("⚠ No expansion detected - file may not have been compressed")
    
    print("\nLimitations:")
    print("- Cannot import to MySQL due to ROW_FORMAT mismatch")
    print("- File header still indicates COMPRESSED format")
    print("- Mixed page sizes in output (INDEX=16KB, metadata=8KB)")
    print("\nMySQL import will fail with:")
    print("  ERROR 1808: Schema mismatch")
    print("  (Table has ROW_TYPE_DYNAMIC, file has ROW_TYPE_COMPRESSED)")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <original.ibd> <decompressed.ibd>")
        sys.exit(1)
    
    verify_decompression(sys.argv[1], sys.argv[2])
```

## Known Limitations

### Decompression Limitations

1. **Cannot Import to MySQL**: Decompressed files cannot be imported back to MySQL due to ROW_FORMAT metadata mismatch
2. **Mixed Page Sizes**: Output files have INDEX pages at logical size (16KB) and metadata pages at physical size (8KB)
3. **Metadata Retention**: File headers retain COMPRESSED format indicators despite containing uncompressed data

### Error Messages

When attempting to import a decompressed file:
```sql
ERROR 1808 (HY000): Schema mismatch 
(Table has ROW_TYPE_DYNAMIC row format, .ibd file has ROW_TYPE_COMPRESSED row format.)
```

This is expected behavior and indicates the metadata mismatch issue.

### Workarounds

Currently, there are no workarounds to import decompressed files directly. The decompression is useful for:
- Data recovery and inspection
- Extracting readable content from compressed tables
- Analyzing table structure
- Forensic analysis

But not for:
- Direct import back to MySQL
- Table restoration
- Migration between servers

## Support and Troubleshooting

### Common Issues

1. **Library not found**: Set `LD_LIBRARY_PATH` to the build directory
2. **Initialization failed**: Ensure all Percona Server dependencies are available
3. **Decompression successful but import fails**: This is expected behavior

### Debug Output

Enable debug mode to see detailed processing information:
```c
ibd_reader_set_debug(reader, 1);
```

This will show:
- Page-by-page processing
- Compression detection
- Decompression operations
- Page type information