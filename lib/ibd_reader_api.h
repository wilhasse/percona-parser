/**
 * InnoDB Data File Reader Library - Public C API
 * 
 * This library provides functions to read, decrypt, and decompress InnoDB data files.
 * It's designed to be used from C, C++, and other languages via FFI (like Go, Python, etc.)
 */

#ifndef IBD_READER_API_H
#define IBD_READER_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version information */
#define IBD_READER_VERSION_MAJOR 1
#define IBD_READER_VERSION_MINOR 0
#define IBD_READER_VERSION_PATCH 0

/* Export macros for shared library */
#ifdef _WIN32
    #ifdef IBD_READER_EXPORTS
        #define IBD_API __declspec(dllexport)
    #else
        #define IBD_API __declspec(dllimport)
    #endif
#else
    #define IBD_API __attribute__((visibility("default")))
#endif

/* Result codes */
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

/* Page type constants */
typedef enum {
    IBD_PAGE_TYPE_ALLOCATED = 0,
    IBD_PAGE_UNDO_LOG = 2,
    IBD_PAGE_INODE = 3,
    IBD_PAGE_IBUF_FREE_LIST = 4,
    IBD_PAGE_IBUF_BITMAP = 5,
    IBD_PAGE_TYPE_SYS = 6,
    IBD_PAGE_TYPE_TRX_SYS = 7,
    IBD_PAGE_TYPE_FSP_HDR = 8,
    IBD_PAGE_TYPE_XDES = 9,
    IBD_PAGE_TYPE_BLOB = 10,
    IBD_PAGE_TYPE_ZBLOB = 11,
    IBD_PAGE_TYPE_ZBLOB2 = 12,
    IBD_PAGE_COMPRESSED = 14,
    IBD_PAGE_ENCRYPTED = 15,
    IBD_PAGE_COMPRESSED_AND_ENCRYPTED = 16,
    IBD_PAGE_ENCRYPTED_RTREE = 17,
    IBD_PAGE_INDEX = 17855
} ibd_page_type_t;

/* Opaque handle for the reader context */
typedef struct ibd_reader* ibd_reader_t;

/* Page information structure */
typedef struct {
    uint32_t page_number;
    uint16_t page_type;
    size_t physical_size;
    size_t logical_size;
    int is_compressed;
    int is_encrypted;
} ibd_page_info_t;

/* ============================================================================
 * Library Initialization and Cleanup
 * ============================================================================ */

/**
 * Initialize the IBD reader library.
 * Must be called once before using any other functions.
 * @return IBD_SUCCESS on success, error code otherwise
 */
IBD_API ibd_result_t ibd_init(void);

/**
 * Cleanup the IBD reader library.
 * Should be called when done using the library.
 */
IBD_API void ibd_cleanup(void);

/**
 * Get the library version string.
 * @return Version string in format "major.minor.patch"
 */
IBD_API const char* ibd_get_version(void);

/* ============================================================================
 * Reader Context Management
 * ============================================================================ */

/**
 * Create a new IBD reader context.
 * @return Reader handle or NULL on failure
 */
IBD_API ibd_reader_t ibd_reader_create(void);

/**
 * Destroy an IBD reader context and free resources.
 * @param reader Reader handle to destroy
 */
IBD_API void ibd_reader_destroy(ibd_reader_t reader);

/**
 * Get the last error message from a reader context.
 * @param reader Reader handle
 * @return Error message string (do not free)
 */
IBD_API const char* ibd_reader_get_error(ibd_reader_t reader);

/**
 * Set debug mode for verbose output.
 * @param reader Reader handle
 * @param enable 1 to enable debug output, 0 to disable
 */
IBD_API void ibd_reader_set_debug(ibd_reader_t reader, int enable);

/* ============================================================================
 * Decompression Functions
 * ============================================================================ */

/**
 * Decompress an entire IBD file.
 * @param reader Reader handle
 * @param input_path Path to compressed IBD file
 * @param output_path Path for decompressed output
 * @return IBD_SUCCESS on success, error code otherwise
 */
IBD_API ibd_result_t ibd_decompress_file(ibd_reader_t reader,
                                         const char* input_path,
                                         const char* output_path);

/**
 * Decompress a single page in memory.
 * @param reader Reader handle (can be NULL for stateless operation)
 * @param compressed Compressed page data
 * @param compressed_size Size of compressed data
 * @param decompressed Output buffer for decompressed data
 * @param decompressed_size In: buffer size, Out: actual decompressed size
 * @param page_info Optional page information (can be NULL)
 * @return IBD_SUCCESS on success, error code otherwise
 */
IBD_API ibd_result_t ibd_decompress_page(ibd_reader_t reader,
                                         const uint8_t* compressed,
                                         size_t compressed_size,
                                         uint8_t* decompressed,
                                         size_t* decompressed_size,
                                         ibd_page_info_t* page_info);

/* ============================================================================
 * Decryption Functions
 * ============================================================================ */

/**
 * Decrypt an entire IBD file.
 * @param reader Reader handle
 * @param input_path Path to encrypted IBD file
 * @param output_path Path for decrypted output
 * @param keyring_path Path to keyring file
 * @param master_key_id Master key ID
 * @param server_uuid Server UUID string
 * @return IBD_SUCCESS on success, error code otherwise
 */
IBD_API ibd_result_t ibd_decrypt_file(ibd_reader_t reader,
                                      const char* input_path,
                                      const char* output_path,
                                      const char* keyring_path,
                                      uint32_t master_key_id,
                                      const char* server_uuid);

/**
 * Decrypt a single page in memory.
 * @param reader Reader handle
 * @param encrypted Encrypted page data
 * @param page_size Page size
 * @param decrypted Output buffer for decrypted data (must be at least page_size)
 * @param key Encryption key
 * @param key_len Key length
 * @param iv Initialization vector
 * @param iv_len IV length
 * @return IBD_SUCCESS on success, error code otherwise
 */
IBD_API ibd_result_t ibd_decrypt_page(ibd_reader_t reader,
                                      const uint8_t* encrypted,
                                      size_t page_size,
                                      uint8_t* decrypted,
                                      const uint8_t* key,
                                      size_t key_len,
                                      const uint8_t* iv,
                                      size_t iv_len);

/* ============================================================================
 * Combined Operations
 * ============================================================================ */

/**
 * Decrypt and decompress an IBD file in one operation.
 * @param reader Reader handle
 * @param input_path Path to encrypted/compressed IBD file
 * @param output_path Path for output file
 * @param keyring_path Path to keyring file
 * @param master_key_id Master key ID
 * @param server_uuid Server UUID string
 * @return IBD_SUCCESS on success, error code otherwise
 */
IBD_API ibd_result_t ibd_decrypt_and_decompress_file(ibd_reader_t reader,
                                                     const char* input_path,
                                                     const char* output_path,
                                                     const char* keyring_path,
                                                     uint32_t master_key_id,
                                                     const char* server_uuid);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * Get page information from a page buffer.
 * @param page_data Page data buffer
 * @param page_size Size of page data
 * @param info Output page information structure
 * @return IBD_SUCCESS on success, error code otherwise
 */
IBD_API ibd_result_t ibd_get_page_info(const uint8_t* page_data,
                                       size_t page_size,
                                       ibd_page_info_t* info);

/**
 * Check if a page is compressed based on its header.
 * @param page_data Page data buffer
 * @param physical_size Physical size of the page
 * @param logical_size Logical size of the page
 * @return 1 if compressed, 0 if not
 */
IBD_API int ibd_is_page_compressed(const uint8_t* page_data,
                                   size_t physical_size,
                                   size_t logical_size);

/**
 * Get human-readable name for a page type.
 * @param page_type Page type value
 * @return Page type name string (do not free)
 */
IBD_API const char* ibd_get_page_type_name(uint16_t page_type);

#ifdef __cplusplus
}
#endif

#endif /* IBD_READER_API_H */