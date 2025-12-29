// ---------------
// Declarations from decompress.cc
// ---------------
#include <cstddef>

/**
 * decompress_page_inplace():
 *   Process one page in memory from src_buf -> out_buf.
 *   
 *   For compressed tablespaces:
 *   - Decompresses INDEX/RTREE pages using page_zip_decompress_low()  
 *   - Copies and pads metadata pages to logical size
 *   
 *   Returns actual_size used (logical for decompressed pages, may vary for others)
 */
extern bool decompress_page_inplace(
    const unsigned char* src_buf,
    size_t               physical_size,
    size_t               logical_size,
    unsigned char*       out_buf,
    size_t               out_buf_len,
    size_t*              actual_size
);

/**
 * decompress_ibd():
 *   The function that reads each page from `in_fd`, checks if compressed,
 *   and writes uncompressed pages to `out_fd`.
 */
bool decompress_ibd(File in_fd, File out_fd);

/**
 * rebuild_uncompressed_ibd():
 *   Experimental converter: reads a compressed tablespace (physical < logical),
 *   expands all pages to logical size, clears ZIP_SSIZE in FSP flags, updates
 *   space_id fields, and writes CRC32 checksums for 16KB pages.
 *   If sdi_json_path is provided, rebuilds the SDI root page from JSON.
 */
bool rebuild_uncompressed_ibd(File in_fd, File out_fd,
                              const char* sdi_json_path);

bool determine_page_size(File file_in, page_size_t &page_sz);

bool should_decompress_page(const unsigned char* page_data,
                           size_t physical_size,
                           size_t logical_size);
