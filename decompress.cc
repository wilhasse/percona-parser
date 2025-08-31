/*****************************************************************************
  Code that reads a possibly compressed .ibd (or ibdata*) and writes out 
  an "uncompressed" copy of every page to a new output file.

  Key behavior for ROW_FORMAT=COMPRESSED tablespaces:
  - Only INDEX (17855) and RTREE (17854) pages are zlib-compressed  
  - Metadata pages (FSP_HDR, XDES, INODE, etc.) are written at physical size, not compressed
  - This code properly decompresses only INDEX/RTREE pages using page_zip_decompress_low()
  - Metadata pages are kept at their natural physical size (as stored on disk)
  - Output file has mixed page sizes: INDEX at logical size, metadata at physical size

  Includes STUBS for references like:
    - ib::logger, ib::warn, ib::error, ib::fatal

  So that linking won't fail. Real InnoDB logic is not performed.
*****************************************************************************/

#include "my_config.h"

// Standard headers
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <errno.h>
#include <string.h>
#include <limits>

// MySQL/Percona headers
#include "my_dbug.h"
#include "my_dir.h"
#include "my_getopt.h"
#include "my_io.h"
//#include "mysys.h"
#include "print_version.h"
#include "welcome_copyright_notice.h"

// InnoDB headers needed for decompress, page size, etc.
#include "fil0fil.h"
#include "fsp0fsp.h"
#include "mach0data.h"
#include "page0page.h"
#include "page0size.h"
#include "page0types.h"
#include "univ.i"
#include "ut0byte.h"
#include "ut0crc32.h"
//#include "page/zipdecompress.h" // Has page_zip_decompress_low()

/*
  In real InnoDB code, these are declared in e.g. "srv0srv.h" or "univ.i"
  but for our demo, we define them here:
*/
ulong srv_page_size       = 0;
ulong srv_page_size_shift = 0;
page_size_t univ_page_size(0, 0, false);

// Provide minimal stubs for the ib::logger family, so vtables are satisfied.
/** Error logging classes. */
namespace ib {

logger::~logger() = default;

info::~info() {
  std::cerr << "[INFO] decompress: " << m_oss.str() << "." << std::endl;
}

warn::~warn() {
  std::cerr << "[WARNING] decompress: " << m_oss.str() << "." << std::endl;
}

error::~error() {
  std::cerr << "[ERROR] decompress: " << m_oss.str() << "." << std::endl;
}

/*
MSVS complains: Warning C4722: destructor never returns, potential memory leak.
But, the whole point of using ib::fatal temporary object is to cause an abort.
*/
MY_COMPILER_DIAGNOSTIC_PUSH()
MY_COMPILER_MSVC_DIAGNOSTIC_IGNORE(4722)

fatal::~fatal() {
  std::cerr << "[FATAL] decompress: " << m_oss.str() << "." << std::endl;
  ut_error;
}

// Restore the MSVS checks for Warning C4722, silenced for ib::fatal::~fatal().
MY_COMPILER_DIAGNOSTIC_POP()

/* TODO: Improve Object creation & destruction on NDEBUG */
class dbug : public logger {
 public:
  ~dbug() override { DBUG_PRINT("decompress", ("%s", m_oss.str().c_str())); }
};
}  // namespace ib

/** Report a failed assertion.
@param[in]	expr	the failed assertion if not NULL
@param[in]	file	source file containing the assertion
@param[in]	line	line number of the assertion */
[[noreturn]] void ut_dbg_assertion_failed(const char *expr, const char *file,
                                          uint64_t line) {
  fprintf(stderr, "decompress: Assertion failure in file %s line " UINT64PF "\n",
          file, line);

  if (expr != nullptr) {
    fprintf(stderr, "decompress: Failing assertion: %s\n", expr);
  }

  fflush(stderr);
  fflush(stdout);
  abort();
}

// ----------------------------------------------------------------
// Minimal usage/option flags
// ----------------------------------------------------------------
static bool opt_version = false;
static bool opt_help    = false;

// Callback for the standard MySQL option parser
extern "C" bool decompress_get_one_option(int optid, const struct my_option *,
                                          char *argument) {
  switch (optid) {
    case 'h':
      opt_help = true;
      break;
    case 'v':
      opt_version = true;
      break;
    default:
      break;
  }
  return false;
}

// ----------------------------------------------------------------
// A small block to read entire page or return SIZE_MAX on error
// ----------------------------------------------------------------
static bool seek_page(File file_in, const page_size_t &page_sz, page_no_t page_no) {
  my_off_t offset = page_no * page_sz.physical();
  if (my_seek(file_in, offset, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
    fprintf(stderr, "Error: my_seek failed for page %u. Errno=%d (%s)\n",
            page_no, errno, strerror(errno));
    return false;
  }
  return true;
}

// ----------------------------------------------------------------
// Minimal “determine page size” logic (like in ibd2sdi).
// Reads page 0, parse fsp header, etc.
// ----------------------------------------------------------------
bool determine_page_size(File file_in, page_size_t &page_sz)
{
  // Temporarily read smallest page
  unsigned char buf[UNIV_ZIP_SIZE_MIN];
  memset(buf, 0, UNIV_ZIP_SIZE_MIN);

  // read the first 1KB from file
  if (my_seek(file_in, 0, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
    fprintf(stderr, "Error: cannot seek to start. %s\n", strerror(errno));
    return false;
  }
  size_t r = my_read(file_in, buf, UNIV_ZIP_SIZE_MIN, MYF(0));
  if (r != UNIV_ZIP_SIZE_MIN) {
    fprintf(stderr, "Cannot read first %u bytes from file.\n",
            UNIV_ZIP_SIZE_MIN);
    return false;
  }

  // parse fsp header
  uint32_t flags = fsp_header_get_flags(buf);
  bool valid = fsp_flags_is_valid(flags);
  if (!valid) {
    fprintf(stderr, "Page 0 is corrupted or invalid fsp flags\n");
    return false;
  }

  // set page size from flags
  ulint ssize = FSP_FLAGS_GET_PAGE_SSIZE(flags);
  if (ssize == 0) {
    srv_page_size = UNIV_PAGE_SIZE_ORIG; // 16k
  } else {
    // e.g. if ssize=4 => (UNIV_ZIP_SIZE_MIN >> 1) << 4 => 16k
    srv_page_size = ((UNIV_ZIP_SIZE_MIN >> 1) << ssize);
  }
  srv_page_size_shift = page_size_validate(srv_page_size);
  if (srv_page_size_shift == 0) {
    fprintf(stderr, "Detected invalid page size shift.\n");
    return false;
  }

  // store in univ_page_size
  univ_page_size.copy_from(page_size_t(srv_page_size, srv_page_size, false));

  // Actually set "page_sz"
  page_sz.copy_from(page_size_t(flags));

  // we also reset file pointer to 0
  my_seek(file_in, 0, MY_SEEK_SET, MYF(0));
  return true;
}

// ----------------------------------------------------------------
// Helper function to determine if a page should be decompressed
// Only INDEX and RTREE pages are zlib-compressed in ROW_FORMAT=COMPRESSED
// ----------------------------------------------------------------
bool should_decompress_page(const unsigned char* page_data,
                           size_t physical_size,
                           size_t logical_size)
{
  // Only decompress if tablespace is compressed (physical < logical)
  if (physical_size >= logical_size) {
    return false;
  }

  // Check if this is a page type that gets zlib-compressed
  uint16_t page_type = mach_read_from_2(page_data + FIL_PAGE_TYPE);
  
  // Only FIL_PAGE_INDEX (17855) and FIL_PAGE_RTREE pages are compressed
  static const uint16_t FIL_PAGE_INDEX = 17855;
  static const uint16_t FIL_PAGE_RTREE = 17854; // Spatial index pages
  
  if (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_RTREE) {
    fprintf(stderr, "  [DEBUG] Page should be decompressed (type=%u in compressed tablespace)\n", page_type);
    return true;
  }
  
  fprintf(stderr, "  [DEBUG] Page type %u in compressed tablespace - metadata page, no decompression needed\n", page_type);
  return false;
}

// ----------------------------------------------------------------
// decompress_page_inplace()
// Returns the actual size of the processed page (logical for decompressed, physical for metadata)
// ----------------------------------------------------------------
bool decompress_page_inplace(
    const unsigned char* src_buf,
    size_t               physical_size,
    size_t               logical_size,
    unsigned char*       out_buf,
    size_t               out_buf_len,
    size_t*              actual_size)
{
    memset(out_buf, 0, out_buf_len);
    *actual_size = physical_size; // Default to physical size

    uint16_t page_type = mach_read_from_2(src_buf + FIL_PAGE_TYPE);
    
    // Check if this page should be decompressed
    bool should_decompress = should_decompress_page(src_buf, physical_size, logical_size);
    
    if (!should_decompress) {
        // For non-compressed tablespaces OR metadata pages in compressed tablespaces:
        // Copy as-is at physical size (metadata pages are naturally at physical size)
        fprintf(stderr, "  [DEBUG] Copying page as-is at physical size (type=%u, size=%zu)\n", page_type, physical_size);
        memcpy(out_buf, src_buf, physical_size);
        *actual_size = physical_size;
        return true;
    }

    // This is an INDEX or RTREE page in a compressed tablespace - decompress it
    fprintf(stderr, "  [DEBUG] Decompressing page (type=%u, phys=%zu->logical=%zu)\n",
            page_type, physical_size, logical_size);

    // Allocate temporary buffer for decompressed data
    unsigned char* temp = (unsigned char*)ut::malloc(2 * logical_size);
    unsigned char* aligned_temp = (unsigned char*)ut_align(temp, logical_size);
    memset(aligned_temp, 0, logical_size);

    // Set up the page_zip descriptor
    page_zip_des_t page_zip;
    page_zip_des_init(&page_zip);
    page_zip.data  = reinterpret_cast<page_zip_t*>(const_cast<unsigned char*>(src_buf));
    page_zip.ssize = page_size_to_ssize(physical_size);

    bool success = false;
    
    if (page_type == FIL_PAGE_INDEX) {
        success = page_zip_decompress_low(&page_zip, aligned_temp, true);
        if (success) {
            fprintf(stderr, "  [DEBUG] Successfully decompressed INDEX page\n");
            memcpy(out_buf, aligned_temp, logical_size);
            *actual_size = logical_size;
        } else {
            fprintf(stderr, "  [ERROR] Failed to decompress INDEX page\n");
        }
    } else {
        // FIL_PAGE_RTREE - treat similarly to INDEX
        fprintf(stderr, "  [DEBUG] Attempting RTREE decompression (experimental)\n");
        success = page_zip_decompress_low(&page_zip, aligned_temp, true);
        if (success) {
            memcpy(out_buf, aligned_temp, logical_size);
            *actual_size = logical_size;
        } else {
            fprintf(stderr, "  [WARNING] RTREE decompression failed, copying as-is\n");
            memcpy(out_buf, src_buf, physical_size);
            *actual_size = physical_size;
            success = true; // Don't fail the whole operation
        }
    }

    ut::free(temp);
    return success;
}

// ----------------------------------------------------------------
// Helper to get page type name for debugging
// ----------------------------------------------------------------
static const char* get_page_type_name(uint16_t page_type) {
    switch(page_type) {
        case 0: return "FIL_PAGE_TYPE_ALLOCATED";
        case 2: return "FIL_PAGE_UNDO_LOG";
        case 3: return "FIL_PAGE_INODE";
        case 4: return "FIL_PAGE_IBUF_FREE_LIST";
        case 5: return "FIL_PAGE_IBUF_BITMAP";
        case 6: return "FIL_PAGE_TYPE_SYS";
        case 7: return "FIL_PAGE_TYPE_TRX_SYS";
        case 8: return "FIL_PAGE_TYPE_FSP_HDR";
        case 9: return "FIL_PAGE_TYPE_XDES";
        case 10: return "FIL_PAGE_TYPE_BLOB";
        case 11: return "FIL_PAGE_TYPE_ZBLOB";
        case 12: return "FIL_PAGE_TYPE_ZBLOB2";
        case 14: return "FIL_PAGE_COMPRESSED";
        case 15: return "FIL_PAGE_ENCRYPTED";
        case 16: return "FIL_PAGE_COMPRESSED_AND_ENCRYPTED";
        case 17: return "FIL_PAGE_ENCRYPTED_RTREE";
        case 17855: return "FIL_PAGE_INDEX";
        default: return "UNKNOWN";
    }
}

// ----------------------------------------------------------------
// fetch_page() calls decompress_page_inplace() to get the final
// processed data into 'uncompressed_buf' and returns actual size used
// ----------------------------------------------------------------
static bool fetch_page(
    File file_in,
    page_no_t page_no,
    const page_size_t &page_sz,
    unsigned char *uncompressed_buf,
    size_t uncompressed_buf_len,
    size_t *actual_page_size)
{
    size_t psize      = page_sz.physical();   // e.g. 8 KB
    size_t logical_sz = page_sz.logical();    // e.g. 16 KB

    fprintf(stderr, "[Page %u] Reading page (physical size=%zu, logical size=%zu)\n", 
            page_no, psize, logical_sz);

    // Allocate a buffer for the raw on-disk page (psize bytes).
    unsigned char* disk_buf = (unsigned char*)malloc(psize);
    if (!disk_buf) {
        fprintf(stderr, "Out of memory for disk_buf\n");
        return false;
    }

    // 1) Read the page from disk
    if (!seek_page(file_in, page_sz, page_no)) {
        free(disk_buf);
        return false;
    }
    size_t r = my_read(file_in, disk_buf, psize, MYF(0));
    if (r != psize) {
        fprintf(stderr, "Could not read physical page %u correctly.\n", page_no);
        free(disk_buf);
        return false;
    }

    // Get page type for debug info
    uint16_t page_type = mach_read_from_2(disk_buf + FIL_PAGE_TYPE);
    fprintf(stderr, "[Page %u] Page type: %u (%s)\n", 
            page_no, page_type, get_page_type_name(page_type));

    // Process the page (decompress INDEX/RTREE pages, copy metadata pages as-is)
    bool ok = decompress_page_inplace(
                  disk_buf,
                  psize,
                  logical_sz,
                  uncompressed_buf,
                  uncompressed_buf_len,
                  actual_page_size);

    if (ok) {
        fprintf(stderr, "[Page %u] Processing completed successfully (output size=%zu)\n", 
                page_no, *actual_page_size);
    } else {
        fprintf(stderr, "[Page %u] Processing failed!\n", page_no);
    }

    free(disk_buf);
    return ok;
}

// ----------------------------------------------------------------
// The main logic that reads each page from input, decompresses if needed,
// writes out the uncompressed page to the output.
// ----------------------------------------------------------------
bool decompress_ibd(File in_fd, File out_fd)
{
  // 1) Determine size of in_fd
  MY_STAT stat_info;
  if (my_fstat(in_fd, &stat_info) != 0) {
    fprintf(stderr, "Cannot fstat() input file.\n");
    return false;
  }
  uint64_t total_bytes = stat_info.st_size;

  // 2) Determine page size
  page_size_t pg_sz(0, 0, false);
  if (!determine_page_size(in_fd, pg_sz)) {
    fprintf(stderr, "Could not determine page size.\n");
    return false;
  }

  // 3) Calculate number of pages
  uint64_t page_physical = static_cast<uint64_t>(pg_sz.physical());
  uint64_t page_logical = static_cast<uint64_t>(pg_sz.logical());
  uint64_t num_pages = total_bytes / page_physical;
  
  fprintf(stderr, "\n========================================\n");
  fprintf(stderr, "DECOMPRESSION STARTING\n");
  fprintf(stderr, "========================================\n");
  fprintf(stderr, "Input file size: %llu bytes\n", (unsigned long long)total_bytes);
  fprintf(stderr, "Page size - Physical: %llu bytes, Logical: %llu bytes\n",
          (unsigned long long)page_physical,
          (unsigned long long)page_logical);
  fprintf(stderr, "Total pages to process: %llu\n", (unsigned long long)num_pages);
  fprintf(stderr, "Compression ratio: %.2f:1 (if compressed)\n", 
          page_physical != page_logical ? (double)page_logical/page_physical : 1.0);
  fprintf(stderr, "========================================\n\n");

  // 4) For each page, fetch + decompress, then write out
  size_t buf_size = std::max(pg_sz.physical(), pg_sz.logical());
  unsigned char* page_buf = (unsigned char*)malloc(buf_size);
  if (!page_buf) {
    fprintf(stderr, "malloc of %llu bytes for page_buf failed.\n",
            (unsigned long long)buf_size);
    return false;
  }

  // Statistics counters
  uint64_t pages_processed = 0;
  uint64_t pages_compressed = 0;
  uint64_t pages_failed = 0;
  uint64_t pages_written = 0;

  for (uint64_t i = 0; i < num_pages; i++) {
    size_t actual_page_size = 0;
    if (!fetch_page(in_fd, (page_no_t)i, pg_sz, page_buf, buf_size, &actual_page_size)) {
      fprintf(stderr, "[ERROR] Failed to process page %llu.\n",
              (unsigned long long)i);
      pages_failed++;
      //free(page_buf);
      //return false;      
    } else {
      pages_processed++;
      
      // Check if this page was compressed (for statistics)
      if (pg_sz.physical() < pg_sz.logical()) {
        pages_compressed++;
      }
      
      // Write out the processed page at its actual size
      size_t w = my_write(out_fd, (uchar*)page_buf, actual_page_size, MYF(0));
      if (w != actual_page_size) {
        fprintf(stderr, "[ERROR] Write failed on page %llu (wrote %zu of %zu bytes).\n", 
                (unsigned long long)i, w, actual_page_size);
        free(page_buf);
        return false;
      }
      pages_written++;
    }
    
    // Progress indicator every 100 pages
    if ((i + 1) % 100 == 0 || (i + 1) == num_pages) {
      fprintf(stderr, "[PROGRESS] Processed %llu/%llu pages (%.1f%%)\n",
              (unsigned long long)(i + 1),
              (unsigned long long)num_pages,
              100.0 * (i + 1) / num_pages);
    }
  }

  // Final summary
  fprintf(stderr, "\n========================================\n");
  fprintf(stderr, "DECOMPRESSION COMPLETE\n");
  fprintf(stderr, "========================================\n");
  fprintf(stderr, "Total pages: %llu\n", (unsigned long long)num_pages);
  fprintf(stderr, "Successfully processed: %llu\n", (unsigned long long)pages_processed);
  fprintf(stderr, "Pages written: %llu\n", (unsigned long long)pages_written);
  fprintf(stderr, "Failed pages: %llu\n", (unsigned long long)pages_failed);
  if (page_physical < page_logical) {
    fprintf(stderr, "Tablespace was compressed (physical=%llu, logical=%llu)\n", 
            (unsigned long long)page_physical, (unsigned long long)page_logical);
    fprintf(stderr, "INDEX pages decompressed with zlib to logical size\n");
    fprintf(stderr, "Metadata pages kept at physical size (as stored on disk)\n");
    fprintf(stderr, "Output has mixed page sizes - INDEX pages at logical size, metadata at physical\n");
  } else {
    fprintf(stderr, "Tablespace was not compressed\n");
  }
  fprintf(stderr, "Output file written successfully\n");
  fprintf(stderr, "========================================\n\n");

  free(page_buf);
  return pages_failed == 0;
}