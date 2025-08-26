# InnoDB Page Decompression Library Development

## Overview
This document describes the development of a minimal InnoDB page decompression library for use with Go programs, without requiring full MySQL dependencies.

## Problem Statement
We need to decompress InnoDB compressed pages (ROW_FORMAT=COMPRESSED) in Go programs. The challenge is that:
- InnoDB's `page_zip_decompress_low()` function is in `libinnodb_zipdecompress.a`
- This function expects exact ABI compatibility with InnoDB's internal structures
- Without the correct headers, struct layouts don't match, causing decompression failures

## Current Status

### What's Been Created
1. **`lib/innodb_decompress_v3.cpp`** - Decompression implementation
2. **`lib/innodb_decompress.h`** - C interface for Go
3. **`lib/mysql_stubs.cpp`** - Stubs for InnoDB logging/error handling
4. **`decompress_v2.go`** - Go wrapper using CGo
5. **`examples/decompress_example.go`** - Test program

### The Core Issue
**ABI Mismatch**: The `page_zip_des_t` structure in our code doesn't match the exact memory layout expected by the compiled `libinnodb_zipdecompress.a`. 

Without access to the original MySQL/Percona headers used to build the library, we're guessing at struct layouts, causing decompression to fail.

## Solution: Development in MySQL Environment

### Phase 1: Create Working C Implementation in MySQL Source Tree

#### 1. Create Minimal Test Program
```c
// test_decompress.c - Build this in MySQL source tree
#include "univ.i"
#include "fil0fil.h"
#include "page0zip.h"
#include "page0page.h"
#include "page0size.h"
#include "mach0data.h"

int decompress_page_proper(
    const unsigned char* compressed_data,
    size_t compressed_size,
    unsigned char* output_buffer,
    size_t output_size)
{
    if (compressed_size != 1024 && compressed_size != 2048 && 
        compressed_size != 4096 && compressed_size != 8192) {
        return -1; // Invalid compressed size
    }
    
    // Check page type
    uint16_t page_type = mach_read_from_2(compressed_data + FIL_PAGE_TYPE);
    if (page_type != FIL_PAGE_INDEX) {
        // Not an INDEX page, just copy
        memcpy(output_buffer, compressed_data, compressed_size);
        return compressed_size;
    }
    
    // Setup page_zip descriptor with REAL InnoDB init
    page_zip_des_t page_zip;
    page_zip_des_init(&page_zip);
    
    // Set compressed data pointer
    page_zip.data = (page_zip_t*)compressed_data;
    page_zip.ssize = page_size_to_ssize(compressed_size);
    
    // Allocate aligned buffer as InnoDB expects
    byte* temp = static_cast<byte*>(
        ut::malloc_withkey(UT_NEW_THIS_FILE_PSI_KEY, 2 * UNIV_PAGE_SIZE));
    byte* aligned = static_cast<byte*>(ut_align(temp, UNIV_PAGE_SIZE));
    memset(aligned, 0, UNIV_PAGE_SIZE);
    
    // Decompress using real InnoDB function
    bool success = page_zip_decompress_low(&page_zip, aligned, true);
    
    if (success) {
        memcpy(output_buffer, aligned, UNIV_PAGE_SIZE);
        ut::free(temp);
        return UNIV_PAGE_SIZE;
    }
    
    ut::free(temp);
    return -2; // Decompression failed
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <compressed.ibd>\n", argv[0]);
        return 1;
    }
    
    // Test with real compressed .ibd file
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        perror("fopen");
        return 1;
    }
    
    // Read page 4 (usually has compressed data)
    unsigned char compressed[8192];
    unsigned char decompressed[16384];
    
    fseek(f, 4 * 8192, SEEK_SET);  // Page 4 at offset 32768
    size_t read = fread(compressed, 1, 8192, f);
    fclose(f);
    
    int result = decompress_page_proper(compressed, read, 
                                        decompressed, sizeof(decompressed));
    
    if (result > 0) {
        printf("Decompression successful: %d bytes\n", result);
        // Dump first few bytes to verify
        for (int i = 0; i < 64; i++) {
            printf("%02X ", decompressed[i]);
            if ((i + 1) % 16 == 0) printf("\n");
        }
        return 0;
    } else {
        printf("Decompression failed: %d\n", result);
        return 1;
    }
}
```

#### 2. Build in MySQL Environment
```bash
# In MySQL source directory
g++ -o test_decompress test_decompress.c \
    -I./storage/innobase/include \
    -I./storage/innobase \
    -I./include \
    -L./lib -linnodb_zipdecompress \
    -lz -llz4 -lpthread

# Test with compressed InnoDB file
./test_decompress /path/to/test_compressed.ibd
```

### Phase 2: Document Structure Layouts

#### Extract Exact Structure Information
```c
// struct_info.c - Build in MySQL source to get exact layouts
#include "page0zip.h"
#include <stddef.h>
#include <stdio.h>

int main() {
    printf("=== page_zip_des_t structure layout ===\n");
    printf("sizeof(page_zip_des_t) = %zu\n", sizeof(page_zip_des_t));
    printf("offsetof(data) = %zu\n", offsetof(page_zip_des_t, data));
    printf("offsetof(ssize) = %zu\n", offsetof(page_zip_des_t, ssize));
    printf("offsetof(n_blobs) = %zu\n", offsetof(page_zip_des_t, n_blobs));
    printf("offsetof(m_start) = %zu\n", offsetof(page_zip_des_t, m_start));
    printf("offsetof(m_end) = %zu\n", offsetof(page_zip_des_t, m_end));
    printf("offsetof(m_nonempty) = %zu\n", offsetof(page_zip_des_t, m_nonempty));
    
    // Print field sizes
    page_zip_des_t pz;
    printf("\nField sizes:\n");
    printf("sizeof(data) = %zu\n", sizeof(pz.data));
    printf("sizeof(ssize) = %zu\n", sizeof(pz.ssize));
    printf("sizeof(n_blobs) = %zu\n", sizeof(pz.n_blobs));
    
    return 0;
}
```

### Phase 3: Create Standalone Library

Once working in MySQL environment, create a minimal shared library:

```c
// innodb_decompress_standalone.c
// This includes ONLY what's needed, with proper headers from MySQL

#include "univ.i"
#include "page0zip.h"
#include "fil0fil.h"
#include "mach0data.h"

// Export C interface for Go
#ifdef __cplusplus
extern "C" {
#endif

int innodb_decompress_page(
    const unsigned char* compressed_data,
    size_t compressed_size,
    unsigned char* output_buffer,
    size_t output_size,
    size_t* bytes_written)
{
    // Implementation from tested code above
    // ...
}

const char* innodb_decompress_version(void) {
    return "1.0.0-mysql";
}

#ifdef __cplusplus
}
#endif
```

Build as shared library:
```bash
g++ -shared -fPIC -o libinnodb_decompress_standalone.so \
    innodb_decompress_standalone.c \
    -I/path/to/mysql/storage/innobase/include \
    -L/path/to/mysql/lib -linnodb_zipdecompress \
    -lz -llz4
```

### Phase 4: Integrate with Go

Once you have a working `.so` from MySQL environment:

1. Copy `libinnodb_decompress_standalone.so` to Go project
2. Update Go wrapper to use the working library
3. Test with same `.ibd` files used in C tests

## Directory Structure

```
percona-parser/
├── INNODB_DECOMPRESSION_LIBRARY.md (this file)
├── decompress.cc                    (original Percona implementation)
├── lib/
│   ├── innodb_decompress_v3.cpp    (current attempt - ABI mismatch)
│   ├── innodb_decompress.h         (C interface)
│   ├── mysql_stubs.cpp             (InnoDB symbol stubs)
│   └── libinnodb_zipdecompress.a   (MySQL static library)
└── test_in_mysql/                   (to be created)
    ├── test_decompress.c            (test in MySQL env)
    ├── struct_info.c                (document structures)
    └── build.sh                     (build script)
```

## Dependencies

### Minimal (what we actually need):
- `libinnodb_zipdecompress.a` - Core decompression
- `libz` - zlib compression
- `liblz4` - LZ4 compression
- `libstdc++` - C++ standard library

### What we DON'T need:
- Full MySQL server libraries
- MySQL client libraries
- Keyring infrastructure
- MySQL plugin system

## Testing

### Test Files
- `testdata/test.ibd` - Uncompressed InnoDB file (16KB pages)
- `testdata/test_compressed.ibd` - Compressed InnoDB file (8KB compressed pages)
  - Page 4 is type 17855 (FIL_PAGE_INDEX) - should decompress
  - Other pages are system pages - just copy

### Expected Behavior
1. Detect compressed pages by size (< 16KB) and type
2. INDEX pages (type 17855) get decompressed to 16KB
3. Other page types are copied as-is
4. Decompressed pages can be parsed by Go InnoDB parser

## Common Issues and Solutions

### Issue 1: ABI Mismatch
**Symptom**: Decompression fails with valid compressed pages
**Cause**: `page_zip_des_t` structure doesn't match compiled library
**Solution**: Build against actual MySQL headers

### Issue 2: Undefined Symbols
**Symptom**: Link errors for InnoDB logger classes
**Cause**: `libinnodb_zipdecompress.a` expects InnoDB infrastructure
**Solution**: Provide minimal stubs (mysql_stubs.cpp)

### Issue 3: Wrong Symbol Linkage
**Symptom**: "undefined symbol: page_zip_decompress_low"
**Cause**: Function has C++ linkage, not C
**Solution**: Use `extern` not `extern "C"` for declaration

## Next Steps

1. [ ] Build and test in MySQL environment with real headers
2. [ ] Document exact structure layouts from MySQL source
3. [ ] Create minimal standalone library that works
4. [ ] Test with various compressed InnoDB files
5. [ ] Package as clean Go library with minimal dependencies

## References

- [InnoDB Page Compression](https://dev.mysql.com/doc/refman/8.0/en/innodb-compression.html)
- [InnoDB Page Structure](https://dev.mysql.com/doc/internals/en/innodb-page-structure.html)
- percona-parser decompress.cc - Working implementation reference