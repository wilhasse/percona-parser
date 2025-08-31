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

bool determine_page_size(File file_in, page_size_t &page_sz);

bool should_decompress_page(const unsigned char* page_data,
                           size_t physical_size,
                           size_t logical_size);

