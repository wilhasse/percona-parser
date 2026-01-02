/**
 * InnoDB Data File Reader Library - C API Implementation
 */

#include "ibd_reader_api.h"
#include <cstring>
#include <string>
#include <memory>
#include <queue>
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
    
    // Determine if page should be decompressed and process it
    size_t logical_size = *decompressed_size;
    size_t actual_size = 0;
    bool should_decompress = should_decompress_page(compressed, compressed_size, logical_size);
    
    // Fill page info if requested
    if (page_info) {
        page_info->page_number = mach_read_from_4(compressed + FIL_PAGE_OFFSET);
        page_info->page_type = mach_read_from_2(compressed + FIL_PAGE_TYPE);
        page_info->physical_size = compressed_size;
        page_info->logical_size = logical_size;
        page_info->is_compressed = should_decompress ? 1 : 0;
        page_info->is_encrypted = 0; // Would need to check encryption header
    }
    
    // Process the page (decompress INDEX/RTREE or copy metadata)
    bool result = decompress_page_inplace(
        compressed,
        compressed_size,
        logical_size,
        decompressed,
        *decompressed_size,
        &actual_size
    );
    
    // Update the actual size used
    if (result) {
        *decompressed_size = actual_size;
    }
    
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
    // For API compatibility: return 1 if tablespace appears compressed
    return (physical_size < logical_size) ? 1 : 0;
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

/* ============================================================================
 * Table Row Parsing API Implementation
 * ============================================================================ */

// Forward declarations from undrop_for_innodb
extern table_def_t table_definitions[];
extern int table_definitions_cnt;
extern bool check_for_a_record(page_t *page, rec_t *rec, table_def_t *table, ulint *offsets);
extern ulint my_rec_offs_nth_size(const ulint* offsets, ulint i);
extern const unsigned char* my_rec_get_nth_field(const rec_t* rec, const ulint* offsets,
                                                  ulint i, ulint* len);

// Column value storage for a row
struct ibd_column_storage {
    ibd_column_type_t type;
    bool is_null;
    std::string name;
    std::string str_data;  // For string/binary data
    union {
        int64_t int_val;
        uint64_t uint_val;
        double float_val;
    } num;
    std::string formatted;
};

// Row data structure
struct ibd_row_data {
    std::vector<ibd_column_storage> columns;
};

// Table iterator structure
struct ibd_table_iterator {
    ibd_reader_t reader;
    int fd;
    std::string table_name;
    table_def_t table_def;
    parser_context_t parser_ctx;

    // Page iteration state
    size_t physical_page_size;
    size_t logical_page_size;
    bool tablespace_compressed;
    uint64_t total_pages;
    uint64_t current_page;

    // Page buffer
    std::vector<unsigned char> page_buf;
    std::vector<unsigned char> logical_buf;
    bool at_end;

    // Buffered rows from current page (parsed via callback)
    std::queue<ibd_row_data*> row_queue;

    // Statistics
    uint64_t rows_read;

    std::string last_error;

    ibd_table_iterator() : reader(nullptr), fd(-1), physical_page_size(0),
                           logical_page_size(0), tablespace_compressed(false),
                           total_pages(0), current_page(0),
                           at_end(false), rows_read(0) {
        memset(&table_def, 0, sizeof(table_def));
    }

    ~ibd_table_iterator() {
        // Clean up buffered rows
        while (!row_queue.empty()) {
            delete row_queue.front();
            row_queue.pop();
        }
        if (fd >= 0) {
            close(fd);
        }
        // Free table_def name and field names
        if (table_def.name) free(table_def.name);
        for (int i = 0; i < table_def.fields_count; i++) {
            if (table_def.fields[i].name) free(table_def.fields[i].name);
        }
    }
};

// Helper to map field_type_t to ibd_column_type_t
static ibd_column_type_t map_field_type(field_type_t ft) {
    switch (ft) {
        case FT_INT: return IBD_COL_INT;
        case FT_UINT: return IBD_COL_UINT;
        case FT_FLOAT: return IBD_COL_FLOAT;
        case FT_DOUBLE: return IBD_COL_DOUBLE;
        case FT_CHAR:
        case FT_TEXT: return IBD_COL_STRING;
        case FT_BIN:
        case FT_BLOB: return IBD_COL_BINARY;
        case FT_DATETIME: return IBD_COL_DATETIME;
        case FT_DATE: return IBD_COL_DATE;
        case FT_TIME: return IBD_COL_TIME;
        case FT_TIMESTAMP: return IBD_COL_TIMESTAMP;
        case FT_DECIMAL: return IBD_COL_DECIMAL;
        case FT_INTERNAL: return IBD_COL_INTERNAL;
        default: return IBD_COL_STRING;
    }
}

// Compute min/max sizes for table record validation
static void compute_table_sizes(table_def_t* table, bool comp = true) {
    table->n_nullable = 0;
    table->min_rec_header_len = 0;
    table->data_min_size = 0;
    table->data_max_size = 0;

    for (int j = 0; j < table->fields_count; j++) {
        field_def_t* fld = &table->fields[j];

        if (fld->can_be_null) {
            table->n_nullable++;
        } else {
            table->data_min_size += fld->min_length + fld->fixed_length;
            int size = (fld->fixed_length ? fld->fixed_length : fld->max_length);
            table->min_rec_header_len += (size > 255 ? 2 : 1);
        }

        table->data_max_size += fld->max_length + fld->fixed_length;
    }

    table->min_rec_header_len += (table->n_nullable + 7) / 8;
}

// Load next valid page for iteration
static bool load_next_leaf_page(ibd_table_iterator* iter) {
    while (iter->current_page < iter->total_pages) {
        off_t offset = static_cast<off_t>(iter->current_page) * iter->physical_page_size;
        ssize_t rd = pread(iter->fd, iter->page_buf.data(), iter->physical_page_size, offset);
        if (rd != static_cast<ssize_t>(iter->physical_page_size)) {
            iter->current_page++;
            continue;
        }

        // Check if FIL_PAGE_INDEX
        if (fil_page_get_type(iter->page_buf.data()) != FIL_PAGE_INDEX) {
            iter->current_page++;
            continue;
        }

        const unsigned char* page_data = iter->page_buf.data();

        // Decompress if needed
        if (iter->tablespace_compressed) {
            size_t actual_size = 0;
            if (!decompress_page_inplace(iter->page_buf.data(),
                                         iter->physical_page_size,
                                         iter->logical_page_size,
                                         iter->logical_buf.data(),
                                         iter->logical_page_size,
                                         &actual_size)) {
                iter->current_page++;
                continue;
            }
            page_data = iter->logical_buf.data();
        }

        // Check if COMPACT format
        if (!page_is_comp(page_data)) {
            iter->current_page++;
            continue;
        }

        // Check if target index
        if (!is_target_index(page_data, &iter->parser_ctx)) {
            iter->current_page++;
            continue;
        }

        // Check if leaf page
        ulint page_level = mach_read_from_2(page_data + PAGE_HEADER + PAGE_LEVEL);
        if (page_level != 0) {
            iter->current_page++;
            continue;
        }

        // Found a valid leaf page - copy decompressed data if needed
        if (iter->tablespace_compressed) {
            memcpy(iter->page_buf.data(), iter->logical_buf.data(), iter->logical_page_size);
        }

        return true;
    }

    return false;
}

// Callback context for row parsing
struct row_parse_context {
    ibd_table_iterator* iter;
    int rows_parsed;
};

// Callback function that converts parsed_row_t to ibd_row_data and queues it
static bool row_parse_callback(const parsed_row_t* parsed_row, void* user_data) {
    row_parse_context* ctx = static_cast<row_parse_context*>(user_data);
    ibd_table_iterator* iter = ctx->iter;

    // Skip deleted records
    if (parsed_row->deleted) {
        return true;  // Continue parsing
    }

    // Skip internal columns (DB_TRX_ID, DB_ROLL_PTR)
    int user_col_count = 0;
    for (int i = 0; i < parsed_row->column_count; i++) {
        if (!parsed_row->columns[i].is_internal) {
            user_col_count++;
        }
    }

    // Create row data
    ibd_row_data* row = new ibd_row_data();
    row->columns.reserve(user_col_count);

    for (int i = 0; i < parsed_row->column_count; i++) {
        const parsed_column_t& pcol = parsed_row->columns[i];

        // Skip internal columns
        if (pcol.is_internal) {
            continue;
        }

        ibd_column_storage col;
        col.name = pcol.name ? pcol.name : "";
        col.type = map_field_type(static_cast<field_type_t>(pcol.field_type));
        col.is_null = pcol.is_null;

        if (pcol.is_null) {
            col.formatted = "NULL";
        } else {
            // Copy formatted string
            col.formatted = pcol.formatted;

            // Copy numeric values
            switch (col.type) {
                case IBD_COL_INT:
                    col.num.int_val = pcol.int_val;
                    break;
                case IBD_COL_UINT:
                    col.num.uint_val = pcol.uint_val;
                    break;
                case IBD_COL_FLOAT:
                case IBD_COL_DOUBLE:
                    col.num.float_val = pcol.double_val;
                    break;
                case IBD_COL_STRING:
                case IBD_COL_BINARY:
                    if (pcol.data && pcol.data_len > 0) {
                        col.str_data.assign(reinterpret_cast<const char*>(pcol.data),
                                            pcol.data_len);
                    }
                    break;
                default:
                    if (pcol.data && pcol.data_len > 0) {
                        col.str_data.assign(reinterpret_cast<const char*>(pcol.data),
                                            pcol.data_len);
                    }
                    break;
            }
        }

        row->columns.push_back(std::move(col));
    }

    iter->row_queue.push(row);
    ctx->rows_parsed++;
    return true;  // Continue parsing
}

// Read next record - uses callback-based parsing
static ibd_row_t read_next_record(ibd_table_iterator* iter) {
    // If we have buffered rows, return from queue
    if (!iter->row_queue.empty()) {
        ibd_row_data* row = iter->row_queue.front();
        iter->row_queue.pop();
        iter->rows_read++;
        return row;
    }

    // Need to parse more pages
    while (!iter->at_end) {
        // Load next valid page
        if (!load_next_leaf_page(iter)) {
            iter->at_end = true;
            return nullptr;
        }

        size_t page_size = iter->tablespace_compressed ?
                           iter->logical_page_size : iter->physical_page_size;

        // Parse all records on this page using callback
        row_parse_context ctx;
        ctx.iter = iter;
        ctx.rows_parsed = 0;

        (void)parse_records_with_callback(
            iter->page_buf.data(),
            page_size,
            iter->current_page,
            &iter->table_def,
            &iter->parser_ctx,
            row_parse_callback,
            &ctx);

        // Move to next page for next call
        iter->current_page++;

        // If we got rows, return the first one
        if (!iter->row_queue.empty()) {
            ibd_row_data* row = iter->row_queue.front();
            iter->row_queue.pop();
            iter->rows_read++;
            return row;
        }
        // No rows on this page, continue to next
    }

    return nullptr;
}

IBD_API ibd_result_t ibd_open_table(ibd_reader_t reader,
                                     const char* ibd_path,
                                     const char* sdi_json_path,
                                     ibd_table_t* table_out) {
    if (!ibd_path || !sdi_json_path || !table_out) {
        if (reader) reader->set_error("Invalid parameters");
        return IBD_ERROR_INVALID_PARAM;
    }

    *table_out = nullptr;

    try {
        // Create iterator
        ibd_table_iterator* iter = new ibd_table_iterator();
        iter->reader = reader;

        // Load schema from SDI JSON
        if (load_ib2sdi_table_columns(sdi_json_path, iter->table_name, &iter->parser_ctx) != 0) {
            iter->last_error = "Failed to load SDI JSON";
            if (reader) reader->set_error(iter->last_error);
            delete iter;
            return IBD_ERROR_INVALID_FORMAT;
        }

        // Build table definition
        if (build_table_def_from_json(&iter->table_def, iter->table_name.c_str()) != 0) {
            iter->last_error = "Failed to build table definition";
            if (reader) reader->set_error(iter->last_error);
            delete iter;
            return IBD_ERROR_INVALID_FORMAT;
        }

        // Compute min/max sizes for record validation
        compute_table_sizes(&iter->table_def);

        // Copy to global table_definitions for check_for_a_record
        memcpy(&table_definitions[0], &iter->table_def, sizeof(table_def_t));
        table_definitions_cnt = 1;

        // Open the IBD file
        iter->fd = open(ibd_path, O_RDONLY);
        if (iter->fd < 0) {
            iter->last_error = std::string("Cannot open file: ") + ibd_path;
            if (reader) reader->set_error(iter->last_error);
            delete iter;
            return IBD_ERROR_FILE_NOT_FOUND;
        }

        // Determine page size
        page_size_t pg_sz(0, 0, false);
        if (!determine_page_size(static_cast<File>(iter->fd), pg_sz)) {
            iter->last_error = "Cannot determine page size";
            if (reader) reader->set_error(iter->last_error);
            delete iter;
            return IBD_ERROR_INVALID_FORMAT;
        }

        iter->physical_page_size = pg_sz.physical();
        iter->logical_page_size = pg_sz.logical();
        iter->tablespace_compressed = (iter->physical_page_size < iter->logical_page_size);

        // Allocate page buffers
        iter->page_buf.resize(std::max(iter->physical_page_size, iter->logical_page_size));
        if (iter->tablespace_compressed) {
            iter->logical_buf.resize(iter->logical_page_size);
        }

        // Get file size and page count
        struct stat st;
        if (fstat(iter->fd, &st) != 0) {
            iter->last_error = "Cannot stat file";
            if (reader) reader->set_error(iter->last_error);
            delete iter;
            return IBD_ERROR_FILE_READ;
        }
        iter->total_pages = st.st_size / iter->physical_page_size;

        // Discover target index
        if (discover_target_index_id(iter->fd, &iter->parser_ctx) != 0) {
            iter->last_error = "Cannot discover index ID";
            if (reader) reader->set_error(iter->last_error);
            delete iter;
            return IBD_ERROR_INVALID_FORMAT;
        }

        iter->current_page = 0;
        iter->at_end = false;

        *table_out = iter;
        return IBD_SUCCESS;

    } catch (const std::exception& e) {
        if (reader) reader->set_error(std::string("Exception: ") + e.what());
        return IBD_ERROR_UNKNOWN;
    }
}

IBD_API ibd_result_t ibd_get_table_info(ibd_table_t table,
                                         char* table_name,
                                         size_t table_name_size,
                                         uint32_t* column_count) {
    if (!table) return IBD_ERROR_INVALID_PARAM;

    if (table_name && table_name_size > 0) {
        strncpy(table_name, table->table_name.c_str(), table_name_size - 1);
        table_name[table_name_size - 1] = '\0';
    }

    if (column_count) {
        *column_count = static_cast<uint32_t>(table->table_def.fields_count);
    }

    return IBD_SUCCESS;
}

IBD_API ibd_result_t ibd_get_column_info(ibd_table_t table,
                                          uint32_t column_index,
                                          char* name,
                                          size_t name_size,
                                          ibd_column_type_t* type) {
    if (!table || column_index >= static_cast<uint32_t>(table->table_def.fields_count)) {
        return IBD_ERROR_INVALID_PARAM;
    }

    const field_def_t* fld = &table->table_def.fields[column_index];

    if (name && name_size > 0 && fld->name) {
        strncpy(name, fld->name, name_size - 1);
        name[name_size - 1] = '\0';
    }

    if (type) {
        *type = map_field_type(fld->type);
    }

    return IBD_SUCCESS;
}

IBD_API ibd_result_t ibd_read_row(ibd_table_t table, ibd_row_t* row_out) {
    if (!table || !row_out) return IBD_ERROR_INVALID_PARAM;

    *row_out = nullptr;

    if (table->at_end) {
        return IBD_ERROR_FILE_READ; // No more rows
    }

    ibd_row_t row = read_next_record(table);
    if (!row) {
        return IBD_ERROR_FILE_READ; // No more rows
    }

    *row_out = row;
    return IBD_SUCCESS;
}

IBD_API uint32_t ibd_row_column_count(ibd_row_t row) {
    if (!row) return 0;
    return static_cast<uint32_t>(row->columns.size());
}

IBD_API ibd_result_t ibd_row_get_column(ibd_row_t row,
                                         uint32_t column_index,
                                         ibd_column_value_t* value) {
    if (!row || !value || column_index >= row->columns.size()) {
        return IBD_ERROR_INVALID_PARAM;
    }

    const ibd_column_storage& col = row->columns[column_index];

    value->name = col.name.c_str();
    value->type = col.type;
    value->is_null = col.is_null ? 1 : 0;

    if (!col.is_null) {
        switch (col.type) {
            case IBD_COL_INT:
                value->value.int_val = col.num.int_val;
                break;
            case IBD_COL_UINT:
                value->value.uint_val = col.num.uint_val;
                break;
            case IBD_COL_FLOAT:
            case IBD_COL_DOUBLE:
                value->value.float_val = col.num.float_val;
                break;
            case IBD_COL_STRING:
            case IBD_COL_BINARY:
                value->value.str_val.data = col.str_data.c_str();
                value->value.str_val.length = col.str_data.size();
                break;
            default:
                value->value.str_val.data = col.str_data.c_str();
                value->value.str_val.length = col.str_data.size();
                break;
        }
    }

    // Copy formatted string
    strncpy(value->formatted, col.formatted.c_str(), sizeof(value->formatted) - 1);
    value->formatted[sizeof(value->formatted) - 1] = '\0';

    return IBD_SUCCESS;
}

IBD_API size_t ibd_row_to_string(ibd_row_t row, char* buffer, size_t buffer_size) {
    if (!row || !buffer || buffer_size == 0) return 0;

    std::string result;
    for (size_t i = 0; i < row->columns.size(); i++) {
        if (i > 0) result += "\t";
        result += row->columns[i].formatted;
    }

    size_t to_copy = std::min(result.size(), buffer_size - 1);
    memcpy(buffer, result.c_str(), to_copy);
    buffer[to_copy] = '\0';

    return result.size();
}

IBD_API void ibd_free_row(ibd_row_t row) {
    if (row) {
        delete row;
    }
}

IBD_API void ibd_close_table(ibd_table_t table) {
    if (table) {
        delete table;
    }
}

IBD_API uint64_t ibd_get_row_count(ibd_table_t table) {
    if (!table) return 0;
    return table->rows_read;
}
