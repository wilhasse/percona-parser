// ---------------
// Declarations from decompress.cc
// ---------------
#include <cstddef>
#include <cstdint>

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
 * validate_index_id_remap():
 *   Dry-run SDI comparison for index-id remap. Prints per-index diffs and
 *   returns false on missing/ambiguous mappings.
 */
bool validate_index_id_remap(const char* source_sdi_json_path,
                             const char* target_sdi_json_path,
                             const char* index_id_map_path);

/**
 * rebuild_uncompressed_ibd():
 *   Experimental converter: reads a compressed tablespace (physical < logical),
 *   expands all pages to logical size, clears ZIP_SSIZE in FSP flags, updates
 *   space_id fields, and writes CRC32 checksums for 16KB pages.
 *   If source_sdi_json_path is provided, it is used for index-id mapping.
 *   If target_sdi_json_path is provided, SDI output/.cfg metadata is rebuilt from it.
 *   If index_id_map_path is provided, it is merged into the remap table.
 *   If target_sdi_root_override is provided, compare with source SDI root and warn.
 *   use_target_sdi_root/use_source_sdi_root control which root page is used.
 *   If target_space_id_override is provided, compare with source space_id and warn.
 *   use_target_space_id/use_source_space_id control which space_id is written.
 *   If cfg_out_path is provided, writes a .cfg file from SDI metadata.
 */
bool rebuild_uncompressed_ibd(File in_fd, File out_fd,
                              const char* source_sdi_json_path,
                              const char* target_sdi_json_path,
                              const char* index_id_map_path,
                              const char* cfg_out_path,
                              bool use_target_sdi_root,
                              bool use_source_sdi_root,
                              bool target_sdi_root_override_set,
                              uint32_t target_sdi_root_override,
                              const char* target_ibd_path,
                              bool use_target_space_id,
                              bool use_source_space_id,
                              bool target_space_id_override_set,
                              uint32_t target_space_id_override);

bool determine_page_size(File file_in, page_size_t &page_sz);

bool should_decompress_page(const unsigned char* page_data,
                           size_t physical_size,
                           size_t logical_size);
