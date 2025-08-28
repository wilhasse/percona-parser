# InnoDB Reader Library API Reference

Complete API documentation for the InnoDB Reader Library (`ibd_reader_api.h`).

## Table of Contents
- [Constants](#constants)
- [Types](#types)
- [Initialization Functions](#initialization-functions)
- [Reader Management](#reader-management)
- [Decompression Functions](#decompression-functions)
- [Decryption Functions](#decryption-functions)
- [Combined Operations](#combined-operations)
- [Utility Functions](#utility-functions)

## Constants

### Version Information
```c
#define IBD_READER_VERSION_MAJOR 1
#define IBD_READER_VERSION_MINOR 0
#define IBD_READER_VERSION_PATCH 0
```

### Result Codes
```c
typedef enum {
    IBD_SUCCESS                     = 0,   // Operation successful
    IBD_ERROR_INVALID_PARAM        = -1,   // Invalid parameter passed
    IBD_ERROR_FILE_NOT_FOUND       = -2,   // File not found
    IBD_ERROR_FILE_READ            = -3,   // File read error
    IBD_ERROR_FILE_WRITE           = -4,   // File write error
    IBD_ERROR_INVALID_FORMAT       = -5,   // Invalid file format
    IBD_ERROR_COMPRESSION          = -6,   // Compression error
    IBD_ERROR_DECOMPRESSION        = -7,   // Decompression error
    IBD_ERROR_ENCRYPTION           = -8,   // Encryption error
    IBD_ERROR_DECRYPTION           = -9,   // Decryption error
    IBD_ERROR_MEMORY              = -10,   // Memory allocation error
    IBD_ERROR_NOT_IMPLEMENTED     = -11,   // Feature not implemented
    IBD_ERROR_KEYRING             = -12,   // Keyring access error
    IBD_ERROR_UNKNOWN             = -99    // Unknown error
} ibd_result_t;
```

### Page Type Constants
```c
typedef enum {
    IBD_PAGE_TYPE_ALLOCATED                = 0,
    IBD_PAGE_UNDO_LOG                     = 2,
    IBD_PAGE_INODE                        = 3,
    IBD_PAGE_IBUF_FREE_LIST               = 4,
    IBD_PAGE_IBUF_BITMAP                  = 5,
    IBD_PAGE_TYPE_SYS                     = 6,
    IBD_PAGE_TYPE_TRX_SYS                 = 7,
    IBD_PAGE_TYPE_FSP_HDR                 = 8,
    IBD_PAGE_TYPE_XDES                    = 9,
    IBD_PAGE_TYPE_BLOB                    = 10,
    IBD_PAGE_TYPE_ZBLOB                   = 11,
    IBD_PAGE_TYPE_ZBLOB2                  = 12,
    IBD_PAGE_COMPRESSED                   = 14,
    IBD_PAGE_ENCRYPTED                    = 15,
    IBD_PAGE_COMPRESSED_AND_ENCRYPTED     = 16,
    IBD_PAGE_ENCRYPTED_RTREE              = 17,
    IBD_PAGE_INDEX                        = 17855
} ibd_page_type_t;
```

## Types

### Reader Handle
```c
typedef struct ibd_reader* ibd_reader_t;
```
Opaque handle to a reader context. All operations use this handle to maintain state.

### Page Information Structure
```c
typedef struct {
    uint32_t page_number;    // Page number in the file
    uint16_t page_type;      // Type of page (see ibd_page_type_t)
    size_t physical_size;    // Size on disk (may be compressed)
    size_t logical_size;     // Uncompressed size
    int is_compressed;       // 1 if compressed, 0 otherwise
    int is_encrypted;        // 1 if encrypted, 0 otherwise
} ibd_page_info_t;
```

## Initialization Functions

### ibd_init
```c
ibd_result_t ibd_init(void);
```
Initialize the InnoDB reader library. Must be called once before using any other functions.

**Returns:**
- `IBD_SUCCESS` on success
- Error code on failure

**Example:**
```c
if (ibd_init() != IBD_SUCCESS) {
    fprintf(stderr, "Failed to initialize library\n");
    return -1;
}
```

### ibd_cleanup
```c
void ibd_cleanup(void);
```
Cleanup the InnoDB reader library. Should be called when done using the library.

### ibd_get_version
```c
const char* ibd_get_version(void);
```
Get the library version string.

**Returns:**
- Version string in format "major.minor.patch"

**Example:**
```c
printf("Library version: %s\n", ibd_get_version());
```

## Reader Management

### ibd_reader_create
```c
ibd_reader_t ibd_reader_create(void);
```
Create a new IBD reader context.

**Returns:**
- Reader handle on success
- NULL on failure

### ibd_reader_destroy
```c
void ibd_reader_destroy(ibd_reader_t reader);
```
Destroy an IBD reader context and free resources.

**Parameters:**
- `reader`: Reader handle to destroy

### ibd_reader_get_error
```c
const char* ibd_reader_get_error(ibd_reader_t reader);
```
Get the last error message from a reader context.

**Parameters:**
- `reader`: Reader handle

**Returns:**
- Error message string (do not free)

### ibd_reader_set_debug
```c
void ibd_reader_set_debug(ibd_reader_t reader, int enable);
```
Set debug mode for verbose output.

**Parameters:**
- `reader`: Reader handle
- `enable`: 1 to enable debug output, 0 to disable

## Decompression Functions

### ibd_decompress_file
```c
ibd_result_t ibd_decompress_file(ibd_reader_t reader,
                                 const char* input_path,
                                 const char* output_path);
```
Decompress an entire IBD file.

**Parameters:**
- `reader`: Reader handle
- `input_path`: Path to compressed IBD file
- `output_path`: Path for decompressed output

**Returns:**
- `IBD_SUCCESS` on success
- Error code on failure

**Example:**
```c
ibd_result_t result = ibd_decompress_file(reader, 
                                          "compressed.ibd", 
                                          "output.ibd");
if (result != IBD_SUCCESS) {
    fprintf(stderr, "Error: %s\n", ibd_reader_get_error(reader));
}
```

### ibd_decompress_page
```c
ibd_result_t ibd_decompress_page(ibd_reader_t reader,
                                 const uint8_t* compressed,
                                 size_t compressed_size,
                                 uint8_t* decompressed,
                                 size_t* decompressed_size,
                                 ibd_page_info_t* page_info);
```
Decompress a single page in memory.

**Parameters:**
- `reader`: Reader handle (can be NULL for stateless operation)
- `compressed`: Compressed page data
- `compressed_size`: Size of compressed data
- `decompressed`: Output buffer for decompressed data
- `decompressed_size`: In: buffer size, Out: actual decompressed size
- `page_info`: Optional page information (can be NULL)

**Returns:**
- `IBD_SUCCESS` on success
- Error code on failure

## Decryption Functions

### ibd_decrypt_file
```c
ibd_result_t ibd_decrypt_file(ibd_reader_t reader,
                              const char* input_path,
                              const char* output_path,
                              const char* keyring_path,
                              uint32_t master_key_id,
                              const char* server_uuid);
```
Decrypt an entire IBD file.

**Parameters:**
- `reader`: Reader handle
- `input_path`: Path to encrypted IBD file
- `output_path`: Path for decrypted output
- `keyring_path`: Path to keyring file
- `master_key_id`: Master key ID
- `server_uuid`: Server UUID string

**Returns:**
- `IBD_SUCCESS` on success
- Error code on failure

### ibd_decrypt_page
```c
ibd_result_t ibd_decrypt_page(ibd_reader_t reader,
                              const uint8_t* encrypted,
                              size_t page_size,
                              uint8_t* decrypted,
                              const uint8_t* key,
                              size_t key_len,
                              const uint8_t* iv,
                              size_t iv_len);
```
Decrypt a single page in memory.

**Parameters:**
- `reader`: Reader handle
- `encrypted`: Encrypted page data
- `page_size`: Page size
- `decrypted`: Output buffer for decrypted data (must be at least page_size)
- `key`: Encryption key
- `key_len`: Key length
- `iv`: Initialization vector
- `iv_len`: IV length

**Returns:**
- `IBD_SUCCESS` on success
- Error code on failure

## Combined Operations

### ibd_decrypt_and_decompress_file
```c
ibd_result_t ibd_decrypt_and_decompress_file(ibd_reader_t reader,
                                             const char* input_path,
                                             const char* output_path,
                                             const char* keyring_path,
                                             uint32_t master_key_id,
                                             const char* server_uuid);
```
Decrypt and decompress an IBD file in one operation.

**Parameters:**
- `reader`: Reader handle
- `input_path`: Path to encrypted/compressed IBD file
- `output_path`: Path for output file
- `keyring_path`: Path to keyring file
- `master_key_id`: Master key ID
- `server_uuid`: Server UUID string

**Returns:**
- `IBD_SUCCESS` on success
- Error code on failure

**Example:**
```c
ibd_result_t result = ibd_decrypt_and_decompress_file(
    reader,
    "encrypted_compressed.ibd",
    "output.ibd",
    "/var/lib/mysql-keyring/keyring",
    1,
    "550fa1f5-8821-11ef-a8c9-0242ac120002"
);
```

## Utility Functions

### ibd_get_page_info
```c
ibd_result_t ibd_get_page_info(const uint8_t* page_data,
                               size_t page_size,
                               ibd_page_info_t* info);
```
Get page information from a page buffer.

**Parameters:**
- `page_data`: Page data buffer
- `page_size`: Size of page data
- `info`: Output page information structure

**Returns:**
- `IBD_SUCCESS` on success
- Error code on failure

### ibd_is_page_compressed
```c
int ibd_is_page_compressed(const uint8_t* page_data,
                           size_t physical_size,
                           size_t logical_size);
```
Check if a page is compressed based on its header.

**Parameters:**
- `page_data`: Page data buffer
- `physical_size`: Physical size of the page
- `logical_size`: Logical size of the page

**Returns:**
- 1 if compressed
- 0 if not compressed

### ibd_get_page_type_name
```c
const char* ibd_get_page_type_name(uint16_t page_type);
```
Get human-readable name for a page type.

**Parameters:**
- `page_type`: Page type value

**Returns:**
- Page type name string (do not free)

**Example:**
```c
uint16_t page_type = IBD_PAGE_INDEX;
printf("Page type: %s\n", ibd_get_page_type_name(page_type));
// Output: Page type: INDEX
```

## Error Handling Best Practices

Always check return codes:
```c
ibd_result_t result = ibd_decompress_file(reader, input, output);
if (result != IBD_SUCCESS) {
    const char* error_msg = ibd_reader_get_error(reader);
    fprintf(stderr, "Operation failed: %s (code: %d)\n", 
            error_msg, result);
    
    switch(result) {
        case IBD_ERROR_FILE_NOT_FOUND:
            // Handle missing file
            break;
        case IBD_ERROR_DECOMPRESSION:
            // Handle decompression failure
            break;
        default:
            // Handle other errors
            break;
    }
}
```

## Memory Management

### Buffer Allocation Guidelines

For decompression:
```c
// Allocate 2x input size for compressed pages
size_t input_size = 8192;  // 8KB compressed
size_t output_size = input_size * 2;  // 16KB decompressed
uint8_t* output_buffer = malloc(output_size);
```

For page operations:
```c
// Standard InnoDB page sizes
#define COMPRESSED_PAGE_SIZE 8192
#define UNCOMPRESSED_PAGE_SIZE 16384
```

### Resource Cleanup

Always clean up in reverse order:
```c
ibd_reader_t reader = ibd_reader_create();
// ... use reader ...
ibd_reader_destroy(reader);
ibd_cleanup();
```