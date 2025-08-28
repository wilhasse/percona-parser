# InnoDB Reader Library Usage Guide

The InnoDB Reader Library provides a C API for reading, decompressing, and decrypting InnoDB data files. It can be integrated into various programming languages through FFI (Foreign Function Interface).

## Table of Contents
- [Overview](#overview)
- [Initialization](#initialization)
- [Basic Usage](#basic-usage)
- [Error Handling](#error-handling)
- [Advanced Features](#advanced-features)
- [Language Bindings](#language-bindings)

## Overview

The library provides functions to:
- Decompress InnoDB compressed pages and files
- Decrypt InnoDB encrypted pages and files
- Parse InnoDB page headers and metadata
- Combine decrypt and decompress operations

## Initialization

Before using any library functions, you must initialize the library:

```c
#include "ibd_reader_api.h"

int main() {
    // Initialize the library
    if (ibd_init() != IBD_SUCCESS) {
        fprintf(stderr, "Failed to initialize library\n");
        return 1;
    }
    
    // Your code here
    
    // Cleanup when done
    ibd_cleanup();
    return 0;
}
```

## Basic Usage

### Creating a Reader Context

Most operations require a reader context:

```c
ibd_reader_t reader = ibd_reader_create();
if (!reader) {
    fprintf(stderr, "Failed to create reader\n");
    return -1;
}

// Use the reader...

// Always destroy when done
ibd_reader_destroy(reader);
```

### Decompressing Files

```c
ibd_result_t result = ibd_decompress_file(reader, 
                                          "compressed.ibd", 
                                          "decompressed.ibd");
if (result != IBD_SUCCESS) {
    fprintf(stderr, "Error: %s\n", ibd_reader_get_error(reader));
}
```

### Decompressing Pages in Memory

```c
uint8_t compressed_page[8192];    // Read from file
uint8_t decompressed_page[16384];
size_t decompressed_size = sizeof(decompressed_page);
ibd_page_info_t page_info;

ibd_result_t result = ibd_decompress_page(reader,
                                          compressed_page,
                                          sizeof(compressed_page),
                                          decompressed_page,
                                          &decompressed_size,
                                          &page_info);

if (result == IBD_SUCCESS) {
    printf("Page %u decompressed (type: %s)\n", 
           page_info.page_number,
           ibd_get_page_type_name(page_info.page_type));
}
```

### Decrypting Files

```c
ibd_result_t result = ibd_decrypt_file(reader,
                                       "encrypted.ibd",
                                       "decrypted.ibd",
                                       "/path/to/keyring",
                                       1,  // master_key_id
                                       "550fa1f5-8821-11ef-a8c9-0242ac120002");
```

### Combined Operations

Decrypt and decompress in a single call:

```c
ibd_result_t result = ibd_decrypt_and_decompress_file(reader,
                                                      "encrypted_compressed.ibd",
                                                      "output.ibd",
                                                      "/path/to/keyring",
                                                      1,  // master_key_id
                                                      "550fa1f5-8821-11ef-a8c9-0242ac120002");
```

## Error Handling

All functions return `ibd_result_t` codes:

```c
typedef enum {
    IBD_SUCCESS = 0,
    IBD_ERROR_INVALID_PARAM = -1,
    IBD_ERROR_FILE_NOT_FOUND = -2,
    IBD_ERROR_FILE_READ = -3,
    IBD_ERROR_FILE_WRITE = -4,
    IBD_ERROR_INVALID_FORMAT = -5,
    IBD_ERROR_COMPRESSION = -6,
    IBD_ERROR_DECOMPRESSION = -7,
    IBD_ERROR_ENCRYPTION = -8,
    IBD_ERROR_DECRYPTION = -9,
    IBD_ERROR_MEMORY = -10,
    IBD_ERROR_NOT_IMPLEMENTED = -11,
    IBD_ERROR_KEYRING = -12,
    IBD_ERROR_UNKNOWN = -99
} ibd_result_t;
```

Get detailed error messages:

```c
if (result != IBD_SUCCESS) {
    const char* error_msg = ibd_reader_get_error(reader);
    fprintf(stderr, "Operation failed: %s (code: %d)\n", error_msg, result);
}
```

## Advanced Features

### Debug Mode

Enable verbose debug output:

```c
ibd_reader_set_debug(reader, 1);  // Enable
ibd_reader_set_debug(reader, 0);  // Disable
```

### Page Information

Get metadata about InnoDB pages:

```c
ibd_page_info_t info;
ibd_result_t result = ibd_get_page_info(page_data, page_size, &info);

printf("Page Number: %u\n", info.page_number);
printf("Page Type: %s\n", ibd_get_page_type_name(info.page_type));
printf("Physical Size: %zu\n", info.physical_size);
printf("Logical Size: %zu\n", info.logical_size);
printf("Compressed: %s\n", info.is_compressed ? "Yes" : "No");
printf("Encrypted: %s\n", info.is_encrypted ? "Yes" : "No");
```

### Checking Compression

```c
int is_compressed = ibd_is_page_compressed(page_data, 
                                           physical_size, 
                                           logical_size);
```

## Language Bindings

### C++ Usage

The library has a C interface, so it can be used directly in C++:

```cpp
extern "C" {
    #include "ibd_reader_api.h"
}

class IbdReader {
private:
    ibd_reader_t reader_;
    
public:
    IbdReader() {
        ibd_init();
        reader_ = ibd_reader_create();
    }
    
    ~IbdReader() {
        if (reader_) {
            ibd_reader_destroy(reader_);
        }
        ibd_cleanup();
    }
    
    bool decompressFile(const std::string& input, 
                       const std::string& output) {
        return ibd_decompress_file(reader_, 
                                   input.c_str(), 
                                   output.c_str()) == IBD_SUCCESS;
    }
};
```

### Python (using ctypes)

```python
import ctypes

# Load the library
lib = ctypes.CDLL('./libibd_reader.so')

# Initialize
lib.ibd_init()

# Create reader
reader = lib.ibd_reader_create()

# Decompress file
result = lib.ibd_decompress_file(reader, 
                                 b"input.ibd", 
                                 b"output.ibd")
if result == 0:
    print("Success!")

# Cleanup
lib.ibd_reader_destroy(reader)
lib.ibd_cleanup()
```

### Go Usage

See [Examples.md](Examples.md#go-example) for complete Go bindings.

## Thread Safety

The library is thread-safe when:
- Each thread uses its own `ibd_reader_t` context
- `ibd_init()` is called before creating threads
- `ibd_cleanup()` is called after all threads finish

Example:
```c
#pragma omp parallel
{
    ibd_reader_t reader = ibd_reader_create();
    // Each thread has its own reader
    ibd_decompress_file(reader, input, output);
    ibd_reader_destroy(reader);
}
```

## Memory Management

- All allocated readers must be destroyed with `ibd_reader_destroy()`
- Output buffers for page operations must be pre-allocated
- The library manages its internal memory automatically
- Call `ibd_cleanup()` to free all global resources

## Performance Tips

1. **Reuse readers**: Create once, use multiple times
2. **Buffer sizes**: Allocate output buffers 2x input size for compression
3. **Debug mode**: Disable in production for better performance
4. **Batch operations**: Process multiple pages before destroying reader

## Troubleshooting

### Common Issues

**Library fails to initialize:**
- Ensure the library was built with required MySQL libraries
- Check that all dependencies are in the library path

**Decompression fails:**
- Verify the input file is actually compressed InnoDB format
- Check page size detection (8KB vs 16KB)

**Decryption fails:**
- Verify keyring file exists and is readable
- Ensure master key ID and server UUID are correct
- Check that the file is actually encrypted

**Memory errors:**
- Ensure output buffers are large enough
- Check for memory leaks with valgrind

### Debug Output

Enable debug mode to see detailed operation logs:

```c
ibd_reader_set_debug(reader, 1);
// Operations will now print detailed debug info to stderr
```

## See Also

- [API_reference.md](Api_reference.md) - Complete API documentation
- [Building.md](Building.md) - Build instructions
- [Examples.md](Examples.md) - Complete code examples