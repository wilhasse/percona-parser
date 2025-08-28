/**
 * InnoDB Data File Reader Library - C API Implementation
 */

#include "ibd_reader_api.h"
#include <cstring>
#include <string>
#include <memory>
#include <fcntl.h>
#include <unistd.h>

// MySQL headers we need (must come first for proper type definitions)
#include "my_config.h"
#include "my_io.h"
#include "my_sys.h"
#include "page0size.h"
#include "fil0fil.h"
#include "mach0data.h"
#include "univ.i"

// Include our internal headers (after MySQL headers)
#include "../decompress.h"
#include "../decrypt.h"
#include "../parser.h"
#include "../my_keyring_lookup.h"

// Reader context structure
struct ibd_reader {
    std::string last_error;
    bool debug_mode;
    
    ibd_reader() : debug_mode(false) {}
    
    void set_error(const std::string& msg) {
        last_error = msg;
        if (debug_mode) {
            fprintf(stderr, "[IBD_READER] Error: %s\n", msg.c_str());
        }
    }
    
    void clear_error() {
        last_error.clear();
    }
};

// Global initialization flag
static bool g_initialized = false;

// Version string
static const char* VERSION_STRING = "1.0.0";

/* ============================================================================
 * Library Initialization and Cleanup
 * ============================================================================ */

IBD_API ibd_result_t ibd_init(void) {
    if (g_initialized) {
        return IBD_SUCCESS;
    }
    
    // Initialize MySQL subsystems if needed
    MY_INIT("ibd_reader");
    
    g_initialized = true;
    return IBD_SUCCESS;
}

IBD_API void ibd_cleanup(void) {
    if (g_initialized) {
        // Cleanup MySQL subsystems if needed
        my_end(0);
        g_initialized = false;
    }
}

IBD_API const char* ibd_get_version(void) {
    return VERSION_STRING;
}

/* ============================================================================
 * Reader Context Management
 * ============================================================================ */

IBD_API ibd_reader_t ibd_reader_create(void) {
    try {
        return new ibd_reader();
    } catch (...) {
        return nullptr;
    }
}

IBD_API void ibd_reader_destroy(ibd_reader_t reader) {
    if (reader) {
        delete reader;
    }
}

IBD_API const char* ibd_reader_get_error(ibd_reader_t reader) {
    if (!reader) {
        return "Invalid reader handle";
    }
    return reader->last_error.c_str();
}

IBD_API void ibd_reader_set_debug(ibd_reader_t reader, int enable) {
    if (reader) {
        reader->debug_mode = (enable != 0);
    }
}

/* ============================================================================
 * Decompression Functions
 * ============================================================================ */

IBD_API ibd_result_t ibd_decompress_file(ibd_reader_t reader,
                                         const char* input_path,
                                         const char* output_path) {
    if (!input_path || !output_path) {
        if (reader) reader->set_error("Invalid parameters");
        return IBD_ERROR_INVALID_PARAM;
    }
    
    if (reader) reader->clear_error();
    
    // Open input file
    File in_fd = my_open(input_path, O_RDONLY, MYF(0));
    if (in_fd < 0) {
        if (reader) reader->set_error(std::string("Cannot open input file: ") + input_path);
        return IBD_ERROR_FILE_NOT_FOUND;
    }
    
    // Open output file
    File out_fd = my_create(output_path, 0, O_WRONLY | O_CREAT | O_TRUNC, MYF(0));
    if (out_fd < 0) {
        my_close(in_fd, MYF(0));
        if (reader) reader->set_error(std::string("Cannot create output file: ") + output_path);
        return IBD_ERROR_FILE_WRITE;
    }
    
    // Call internal decompress function
    bool result = decompress_ibd(in_fd, out_fd);
    
    my_close(in_fd, MYF(0));
    my_close(out_fd, MYF(0));
    
    if (!result) {
        if (reader) reader->set_error("Decompression failed");
        return IBD_ERROR_DECOMPRESSION;
    }
    
    return IBD_SUCCESS;
}

IBD_API ibd_result_t ibd_decompress_page(ibd_reader_t reader,
                                         const uint8_t* compressed,
                                         size_t compressed_size,
                                         uint8_t* decompressed,
                                         size_t* decompressed_size,
                                         ibd_page_info_t* page_info) {
    if (!compressed || !decompressed || !decompressed_size) {
        if (reader) reader->set_error("Invalid parameters");
        return IBD_ERROR_INVALID_PARAM;
    }
    
    if (reader) reader->clear_error();
    
    // Determine if page is compressed
    size_t logical_size = *decompressed_size;
    bool is_compressed = is_page_compressed(compressed, compressed_size, logical_size);
    
    // Fill page info if requested
    if (page_info) {
        page_info->page_number = mach_read_from_4(compressed + FIL_PAGE_OFFSET);
        page_info->page_type = mach_read_from_2(compressed + FIL_PAGE_TYPE);
        page_info->physical_size = compressed_size;
        page_info->logical_size = logical_size;
        page_info->is_compressed = is_compressed ? 1 : 0;
        page_info->is_encrypted = 0; // Would need to check encryption header
    }
    
    // Decompress the page
    bool result = decompress_page_inplace(
        compressed,
        compressed_size,
        is_compressed,
        decompressed,
        *decompressed_size,
        logical_size
    );
    
    if (!result) {
        if (reader) reader->set_error("Page decompression failed");
        return IBD_ERROR_DECOMPRESSION;
    }
    
    *decompressed_size = logical_size;
    return IBD_SUCCESS;
}

/* ============================================================================
 * Decryption Functions
 * ============================================================================ */

IBD_API ibd_result_t ibd_decrypt_file(ibd_reader_t reader,
                                      const char* input_path,
                                      const char* output_path,
                                      const char* keyring_path,
                                      uint32_t master_key_id,
                                      const char* server_uuid) {
    if (!input_path || !output_path || !keyring_path || !server_uuid) {
        if (reader) reader->set_error("Invalid parameters");
        return IBD_ERROR_INVALID_PARAM;
    }
    
    if (reader) reader->clear_error();
    
    try {
        // Get master key from keyring
        std::vector<unsigned char> master_key;
        if (!get_master_key(master_key_id, server_uuid, keyring_path, master_key)) {
            if (reader) reader->set_error("Failed to get master key from keyring");
            return IBD_ERROR_KEYRING;
        }
        
        // Read tablespace key and IV
        Tablespace_key_iv ts_key_iv;
        if (!read_tablespace_key_iv(input_path, 0, master_key, ts_key_iv)) {
            if (reader) reader->set_error("Failed to read tablespace key/IV");
            return IBD_ERROR_DECRYPTION;
        }
        
        // Decrypt the file
        if (!decrypt_ibd_file(input_path, output_path, ts_key_iv, false)) {
            if (reader) reader->set_error("File decryption failed");
            return IBD_ERROR_DECRYPTION;
        }
        
        return IBD_SUCCESS;
    } catch (const std::exception& e) {
        if (reader) reader->set_error(std::string("Exception: ") + e.what());
        return IBD_ERROR_UNKNOWN;
    }
}

IBD_API ibd_result_t ibd_decrypt_page(ibd_reader_t reader,
                                      const uint8_t* encrypted,
                                      size_t page_size,
                                      uint8_t* decrypted,
                                      const uint8_t* key,
                                      size_t key_len,
                                      const uint8_t* iv,
                                      size_t iv_len) {
    if (!encrypted || !decrypted || !key || !iv) {
        if (reader) reader->set_error("Invalid parameters");
        return IBD_ERROR_INVALID_PARAM;
    }
    
    if (reader) reader->clear_error();
    
    // Copy encrypted data to output buffer first
    memcpy(decrypted, encrypted, page_size);
    
    // Decrypt in place
    bool result = decrypt_page_inplace(
        decrypted,
        page_size,
        key,
        key_len,
        iv,
        16  // AES block size
    );
    
    if (!result) {
        if (reader) reader->set_error("Page decryption failed");
        return IBD_ERROR_DECRYPTION;
    }
    
    return IBD_SUCCESS;
}

/* ============================================================================
 * Combined Operations
 * ============================================================================ */

IBD_API ibd_result_t ibd_decrypt_and_decompress_file(ibd_reader_t reader,
                                                     const char* input_path,
                                                     const char* output_path,
                                                     const char* keyring_path,
                                                     uint32_t master_key_id,
                                                     const char* server_uuid) {
    if (!input_path || !output_path || !keyring_path || !server_uuid) {
        if (reader) reader->set_error("Invalid parameters");
        return IBD_ERROR_INVALID_PARAM;
    }
    
    if (reader) reader->clear_error();
    
    // Create temporary file for decrypted output
    std::string temp_path = std::string(output_path) + ".tmp";
    
    // First decrypt
    ibd_result_t result = ibd_decrypt_file(reader, input_path, temp_path.c_str(),
                                           keyring_path, master_key_id, server_uuid);
    if (result != IBD_SUCCESS) {
        return result;
    }
    
    // Then decompress
    result = ibd_decompress_file(reader, temp_path.c_str(), output_path);
    
    // Remove temporary file
    unlink(temp_path.c_str());
    
    return result;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

IBD_API ibd_result_t ibd_get_page_info(const uint8_t* page_data,
                                       size_t page_size,
                                       ibd_page_info_t* info) {
    if (!page_data || !info) {
        return IBD_ERROR_INVALID_PARAM;
    }
    
    info->page_number = mach_read_from_4(page_data + FIL_PAGE_OFFSET);
    info->page_type = mach_read_from_2(page_data + FIL_PAGE_TYPE);
    info->physical_size = page_size;
    info->logical_size = page_size; // Would need more context to determine
    
    // Simple heuristics for compression/encryption
    info->is_compressed = 0;
    info->is_encrypted = 0;
    
    if (info->page_type == IBD_PAGE_COMPRESSED || 
        info->page_type == IBD_PAGE_COMPRESSED_AND_ENCRYPTED) {
        info->is_compressed = 1;
    }
    
    if (info->page_type == IBD_PAGE_ENCRYPTED || 
        info->page_type == IBD_PAGE_COMPRESSED_AND_ENCRYPTED) {
        info->is_encrypted = 1;
    }
    
    return IBD_SUCCESS;
}

IBD_API int ibd_is_page_compressed(const uint8_t* page_data,
                                   size_t physical_size,
                                   size_t logical_size) {
    return is_page_compressed(page_data, physical_size, logical_size) ? 1 : 0;
}

IBD_API const char* ibd_get_page_type_name(uint16_t page_type) {
    switch(page_type) {
        case IBD_PAGE_TYPE_ALLOCATED: return "ALLOCATED";
        case IBD_PAGE_UNDO_LOG: return "UNDO_LOG";
        case IBD_PAGE_INODE: return "INODE";
        case IBD_PAGE_IBUF_FREE_LIST: return "IBUF_FREE_LIST";
        case IBD_PAGE_IBUF_BITMAP: return "IBUF_BITMAP";
        case IBD_PAGE_TYPE_SYS: return "SYS";
        case IBD_PAGE_TYPE_TRX_SYS: return "TRX_SYS";
        case IBD_PAGE_TYPE_FSP_HDR: return "FSP_HDR";
        case IBD_PAGE_TYPE_XDES: return "XDES";
        case IBD_PAGE_TYPE_BLOB: return "BLOB";
        case IBD_PAGE_TYPE_ZBLOB: return "ZBLOB";
        case IBD_PAGE_TYPE_ZBLOB2: return "ZBLOB2";
        case IBD_PAGE_COMPRESSED: return "COMPRESSED";
        case IBD_PAGE_ENCRYPTED: return "ENCRYPTED";
        case IBD_PAGE_COMPRESSED_AND_ENCRYPTED: return "COMPRESSED_AND_ENCRYPTED";
        case IBD_PAGE_ENCRYPTED_RTREE: return "ENCRYPTED_RTREE";
        case IBD_PAGE_INDEX: return "INDEX";
        default: return "UNKNOWN";
    }
}