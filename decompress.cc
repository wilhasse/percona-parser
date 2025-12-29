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
#include <algorithm>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <zlib.h>

// MySQL/Percona headers
#include "my_dbug.h"
#include "my_dir.h"
#include "my_getopt.h"
#include "my_io.h"
//#include "mysys.h"
#include "print_version.h"
#include "welcome_copyright_notice.h"

// RapidJSON for SDI JSON parsing
// Must define this BEFORE including rapidjson to use MySQL's size_t typedef
#define RAPIDJSON_NO_SIZETYPEDEFINE
namespace rapidjson { typedef ::std::size_t SizeType; }
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

// InnoDB headers needed for decompress, page size, etc.
#include "data0type.h"
#include "dict0dict.h"
#include "fil0fil.h"
#include "fsp0fsp.h"
#include "fsp0types.h"
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

// Stub for page_zip_rec_set_owned - only called when page_zip is non-null,
// but linker still needs the symbol even though we pass nullptr.
void page_zip_rec_set_owned(page_zip_des_t* /*page_zip*/, const byte* /*rec*/,
                            ulint /*n_owned*/) {
  // This should never be called since we pass nullptr for page_zip
}

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
  
  // INDEX, RTREE, and SDI pages are compressed in ROW_FORMAT=COMPRESSED tablespaces
  static const uint16_t FIL_PAGE_INDEX = 17855;
  static const uint16_t FIL_PAGE_RTREE = 17854; // Spatial index pages
  static const uint16_t FIL_PAGE_SDI = 17853;   // Serialized Dictionary Info
  
  if (page_type == FIL_PAGE_INDEX ||
      page_type == FIL_PAGE_RTREE ||
      page_type == FIL_PAGE_SDI) {
    fprintf(stderr, "  [DEBUG] Page should be decompressed (type=%u in compressed tablespace)\n", page_type);
    return true;
  }
  
  fprintf(stderr, "  [DEBUG] Page type %u in compressed tablespace - metadata page, no decompression needed\n", page_type);
  return false;
}

// ----------------------------------------------------------------
// SDI rebuild helpers (Option B)
// ----------------------------------------------------------------
struct SdiEntry {
  uint64_t type;
  uint64_t id;
  std::string json;
};

static const byte kInfimumSupremumCompact[] = {
    /* infimum record */
    0x01, 0x00, 0x02, 0x00, 0x0d,
    'i', 'n', 'f', 'i', 'm', 'u', 'm', 0x00,
    /* supremum record */
    0x01, 0x00, 0x0b, 0x00, 0x00,
    's', 'u', 'p', 'r', 'e', 'm', 'u', 'm'};

constexpr uint32_t kSdiRecTypeLen = 4;
constexpr uint32_t kSdiRecIdLen = 8;
constexpr uint32_t kSdiRecUncompLen = 4;
constexpr uint32_t kSdiRecCompLen = 4;
constexpr uint32_t kSdiRecOrigin = 0;
constexpr uint32_t kSdiRecHeaderSize = REC_N_NEW_EXTRA_BYTES;
constexpr uint32_t kSdiRecOffType = kSdiRecOrigin;
constexpr uint32_t kSdiRecOffId = kSdiRecOffType + kSdiRecTypeLen;
constexpr uint32_t kSdiRecOffTrxId = kSdiRecOffId + kSdiRecIdLen;
constexpr uint32_t kSdiRecOffRollPtr = kSdiRecOffTrxId + DATA_TRX_ID_LEN;
constexpr uint32_t kSdiRecOffUncompLen =
    kSdiRecOffRollPtr + DATA_ROLL_PTR_LEN;
constexpr uint32_t kSdiRecOffCompLen = kSdiRecOffUncompLen + kSdiRecUncompLen;
constexpr uint32_t kSdiRecOffVar = kSdiRecOffCompLen + kSdiRecCompLen;

static bool sdi_read_uint64(const rapidjson::Value& val, uint64_t* out) {
  if (val.IsUint64()) {
    *out = val.GetUint64();
    return true;
  }
  if (val.IsInt64() && val.GetInt64() >= 0) {
    *out = static_cast<uint64_t>(val.GetInt64());
    return true;
  }
  if (val.IsUint()) {
    *out = val.GetUint();
    return true;
  }
  if (val.IsInt() && val.GetInt() >= 0) {
    *out = static_cast<uint64_t>(val.GetInt());
    return true;
  }
  return false;
}

static bool load_sdi_json_entries(const char* json_path,
                                  std::vector<SdiEntry>& entries) {
  std::ifstream ifs(json_path);
  if (!ifs.is_open()) {
    fprintf(stderr, "Error: cannot open SDI JSON file: %s\n", json_path);
    return false;
  }

  rapidjson::IStreamWrapper isw(ifs);
  rapidjson::Document doc;
  doc.ParseStream(isw);
  if (doc.HasParseError()) {
    fprintf(stderr, "Error: SDI JSON parse error: %s at offset %zu\n",
            rapidjson::GetParseError_En(doc.GetParseError()),
            doc.GetErrorOffset());
    return false;
  }

  if (!doc.IsArray()) {
    fprintf(stderr, "Error: SDI JSON top-level is not an array.\n");
    return false;
  }

  entries.clear();
  for (const auto& elem : doc.GetArray()) {
    if (elem.IsString()) {
      continue;  // "ibd2sdi" marker
    }
    if (!elem.IsObject()) {
      continue;
    }
    if (!elem.HasMember("type") || !elem.HasMember("id") ||
        !elem.HasMember("object")) {
      continue;
    }

    uint64_t type = 0;
    uint64_t id = 0;
    if (!sdi_read_uint64(elem["type"], &type) ||
        !sdi_read_uint64(elem["id"], &id)) {
      fprintf(stderr, "Warning: skipping SDI entry with non-numeric id/type\n");
      continue;
    }

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    elem["object"].Accept(writer);

    entries.push_back(SdiEntry{type, id, sb.GetString()});
  }

  if (entries.empty()) {
    fprintf(stderr, "Error: no SDI records found in %s\n", json_path);
    return false;
  }

  std::sort(entries.begin(), entries.end(),
            [](const SdiEntry& a, const SdiEntry& b) {
              if (a.type != b.type) {
                return a.type < b.type;
              }
              return a.id < b.id;
            });

  return true;
}

static bool compress_sdi_json(const std::string& json,
                              std::vector<byte>& out,
                              uint32_t* out_len) {
  if (json.size() > UINT32_MAX) {
    fprintf(stderr, "Error: SDI JSON too large (%zu bytes)\n", json.size());
    return false;
  }

  uLongf bound = compressBound(static_cast<uLong>(json.size()));
  out.resize(static_cast<size_t>(bound));
  uLongf dest_len = bound;

  int ret = compress2(out.data(), &dest_len,
                      reinterpret_cast<const Bytef*>(json.data()),
                      static_cast<uLong>(json.size()), 6);
  if (ret != Z_OK) {
    fprintf(stderr, "Error: zlib compress failed (%d)\n", ret);
    return false;
  }

  out.resize(static_cast<size_t>(dest_len));
  *out_len = static_cast<uint32_t>(dest_len);
  return true;
}

static void write_compact_next_offs(byte* rec, uint16_t rec_off,
                                    uint16_t next_off) {
  uint16_t diff = 0;
  if (next_off != 0) {
    diff = static_cast<uint16_t>(next_off - rec_off);
  }
  mach_write_to_2(rec - REC_NEXT, diff);
}

static std::vector<ulint> build_dir_groups(size_t user_recs) {
  std::vector<ulint> groups;
  size_t remaining = user_recs + 1;  // user recs + supremum

  while (remaining > PAGE_DIR_SLOT_MAX_N_OWNED) {
    groups.push_back(PAGE_DIR_SLOT_MAX_N_OWNED);
    remaining -= PAGE_DIR_SLOT_MAX_N_OWNED;
  }

  groups.push_back(static_cast<ulint>(remaining));
  return groups;
}

static void init_empty_sdi_page(byte* page, size_t page_size,
                                page_no_t page_no) {
  memset(page, 0, page_size);
  mach_write_to_4(page + FIL_PAGE_OFFSET, page_no);
  mach_write_to_4(page + FIL_PAGE_PREV, FIL_NULL);
  mach_write_to_4(page + FIL_PAGE_NEXT, FIL_NULL);
  mach_write_to_2(page + FIL_PAGE_TYPE, FIL_PAGE_SDI);

  memset(page + PAGE_HEADER, 0, PAGE_HEADER_PRIV_END);
  mach_write_to_2(page + PAGE_HEADER + PAGE_N_DIR_SLOTS, 2);
  mach_write_to_2(page + PAGE_HEADER + PAGE_DIRECTION, PAGE_NO_DIRECTION);
  mach_write_to_2(page + PAGE_HEADER + PAGE_N_HEAP,
                  static_cast<uint16_t>(0x8000 | PAGE_HEAP_NO_USER_LOW));
  mach_write_to_2(page + PAGE_HEADER + PAGE_HEAP_TOP, PAGE_NEW_SUPREMUM_END);

  memcpy(page + PAGE_DATA, kInfimumSupremumCompact,
         sizeof kInfimumSupremumCompact);
  memset(page + PAGE_NEW_SUPREMUM_END, 0,
         page_size - PAGE_DIR - PAGE_NEW_SUPREMUM_END);

  byte* slot0 = page + page_size - PAGE_DIR - PAGE_DIR_SLOT_SIZE;
  byte* slot1 = page + page_size - PAGE_DIR - PAGE_DIR_SLOT_SIZE * 2;
  mach_write_to_2(slot0, PAGE_NEW_INFIMUM);
  mach_write_to_2(slot1, PAGE_NEW_SUPREMUM);
}

static bool populate_sdi_root_page(byte* page, size_t page_size,
                                   const std::vector<SdiEntry>& entries) {
  struct RecInfo {
    byte* rec;
    uint16_t offs;
  };
  std::vector<RecInfo> recs;
  recs.reserve(entries.size());

  const std::vector<ulint> groups = build_dir_groups(entries.size());
  const ulint n_slots = 1 + static_cast<ulint>(groups.size());
  const size_t dir_start =
      page_size - PAGE_DIR - PAGE_DIR_SLOT_SIZE * n_slots;

  size_t heap_top = PAGE_NEW_SUPREMUM_END;

  for (size_t i = 0; i < entries.size(); ++i) {
    std::vector<byte> comp;
    uint32_t comp_len = 0;
    if (!compress_sdi_json(entries[i].json, comp, &comp_len)) {
      return false;
    }

    const uint32_t uncomp_len = static_cast<uint32_t>(entries[i].json.size());
    if (comp_len > 0x3fff) {
      fprintf(stderr,
              "Error: SDI record too large for in-page storage (%u bytes)\n",
              comp_len);
      return false;
    }

    const size_t len_bytes = (comp_len <= 127) ? 1 : 2;
    const size_t rec_data_len = kSdiRecOffVar + comp_len;
    const size_t rec_size = kSdiRecHeaderSize + len_bytes + rec_data_len;

    if (heap_top + rec_size > dir_start) {
      fprintf(stderr, "Error: SDI records exceed SDI root page capacity\n");
      return false;
    }

    byte* rec_base = page + heap_top;
    byte* rec = rec_base + len_bytes + kSdiRecHeaderSize;
    memset(rec_base, 0, rec_size);

    if (len_bytes == 1) {
      rec_base[0] = static_cast<byte>(comp_len);
    } else {
      rec_base[0] = static_cast<byte>(comp_len & 0xFF);
      rec_base[1] = static_cast<byte>((comp_len >> 8) | 0x80);
    }

    rec_set_heap_no_new(rec, PAGE_HEAP_NO_USER_LOW + i);
    rec_set_status(rec, REC_STATUS_ORDINARY);
    rec_set_n_owned_new(rec, nullptr, 0);

    mach_write_to_4(rec + kSdiRecOffType, static_cast<uint32_t>(entries[i].type));
    mach_write_to_8(rec + kSdiRecOffId, entries[i].id);
    mach_write_to_6(rec + kSdiRecOffTrxId, 0);
    mach_write_to_7(rec + kSdiRecOffRollPtr, 0);
    mach_write_to_4(rec + kSdiRecOffUncompLen, uncomp_len);
    mach_write_to_4(rec + kSdiRecOffCompLen, comp_len);
    memcpy(rec + kSdiRecOffVar, comp.data(), comp_len);

    const uint16_t rec_off = static_cast<uint16_t>(rec - page);
    recs.push_back(RecInfo{rec, rec_off});
    heap_top += rec_size;
  }

  mach_write_to_2(page + PAGE_HEADER + PAGE_N_RECS,
                  static_cast<uint16_t>(entries.size()));
  mach_write_to_2(page + PAGE_HEADER + PAGE_HEAP_TOP,
                  static_cast<uint16_t>(heap_top));
  mach_write_to_2(page + PAGE_HEADER + PAGE_N_HEAP,
                  static_cast<uint16_t>(0x8000 |
                                        (PAGE_HEAP_NO_USER_LOW +
                                         entries.size())));
  mach_write_to_2(page + PAGE_HEADER + PAGE_N_DIR_SLOTS,
                  static_cast<uint16_t>(n_slots));
  mach_write_to_2(page + PAGE_HEADER + PAGE_LEVEL, 0);
  mach_write_to_8(page + PAGE_HEADER + PAGE_INDEX_ID,
                  std::numeric_limits<uint64_t>::max());
  mach_write_to_8(page + PAGE_HEADER + PAGE_MAX_TRX_ID, 0);

  const uint16_t infimum_off = PAGE_NEW_INFIMUM;
  const uint16_t supremum_off = PAGE_NEW_SUPREMUM;
  byte* infimum = page + infimum_off;
  byte* supremum = page + supremum_off;

  rec_set_n_owned_new(infimum, nullptr, 1);
  write_compact_next_offs(infimum, infimum_off,
                          recs.empty() ? supremum_off : recs[0].offs);

  for (size_t i = 0; i < recs.size(); ++i) {
    const uint16_t next_off =
        (i + 1 < recs.size()) ? recs[i + 1].offs : supremum_off;
    write_compact_next_offs(recs[i].rec, recs[i].offs, next_off);
  }

  write_compact_next_offs(supremum, supremum_off, 0);

  size_t rec_index = 0;
  for (ulint group : groups) {
    rec_index += group - 1;
    if (rec_index >= recs.size()) {
      rec_set_n_owned_new(supremum, nullptr, group);
    } else {
      rec_set_n_owned_new(recs[rec_index].rec, nullptr, group);
    }
    rec_index += 1;
  }

  byte* slot0 = page + page_size - PAGE_DIR - PAGE_DIR_SLOT_SIZE;
  mach_write_to_2(slot0, infimum_off);

  size_t slot = 1;
  rec_index = 0;
  for (ulint group : groups) {
    rec_index += group - 1;
    uint16_t owner_off = supremum_off;
    if (rec_index < recs.size()) {
      owner_off = recs[rec_index].offs;
    }
    byte* slot_ptr =
        page + page_size - PAGE_DIR - PAGE_DIR_SLOT_SIZE * (slot + 1);
    mach_write_to_2(slot_ptr, owner_off);
    slot++;
    rec_index += 1;
  }

  return true;
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
    } else if (page_type == FIL_PAGE_RTREE) {
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
    } else {
        // FIL_PAGE_SDI - treat like INDEX
        fprintf(stderr, "  [DEBUG] Decompressing SDI page\n");
        success = page_zip_decompress_low(&page_zip, aligned_temp, true);
        if (success) {
            memcpy(out_buf, aligned_temp, logical_size);
            *actual_size = logical_size;
        } else {
            fprintf(stderr, "  [ERROR] Failed to decompress SDI page\n");
        }
    }

    ut::free(temp);
    return success;
}

static uint32_t calc_page_crc32(const unsigned char* page, size_t page_size) {
  const uint32_t c1 = ut_crc32(page + FIL_PAGE_OFFSET,
                               FIL_PAGE_FILE_FLUSH_LSN - FIL_PAGE_OFFSET);
  const uint32_t c2 = ut_crc32(page + FIL_PAGE_DATA,
                               page_size - FIL_PAGE_DATA -
                                   FIL_PAGE_END_LSN_OLD_CHKSUM);
  return (c1 ^ c2);
}

static void stamp_page_lsn_and_crc32(unsigned char* page,
                                     size_t page_size,
                                     uint64_t lsn) {
  mach_write_to_8(page + FIL_PAGE_LSN, lsn);
  mach_write_to_8(page + page_size - FIL_PAGE_END_LSN_OLD_CHKSUM, lsn);

  const uint32_t checksum = calc_page_crc32(page, page_size);
  mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
  mach_write_to_4(page + page_size - FIL_PAGE_END_LSN_OLD_CHKSUM, checksum);
}

static bool update_tablespace_header_for_uncompressed(
    unsigned char* page,
    size_t page_size,
    space_id_t* out_space_id) {
  if (page_size != UNIV_PAGE_SIZE_ORIG) {
    fprintf(stderr,
            "Unsupported logical page size %zu (only 16KB supported for "
            "rebuild).\n",
            page_size);
    return false;
  }

  const space_id_t space_id = fsp_header_get_field(page, FSP_SPACE_ID);
  if (space_id == 0 || space_id == SPACE_UNKNOWN) {
    fprintf(stderr, "Invalid space id in page 0 header: %u\n", space_id);
    return false;
  }

  const uint32_t old_flags = fsp_header_get_flags(page);
  if (!fsp_flags_is_valid(old_flags)) {
    fprintf(stderr, "Invalid FSP flags in page 0: 0x%x\n", old_flags);
    return false;
  }

  uint32_t new_flags = old_flags;
  new_flags &= ~FSP_FLAGS_MASK_ZIP_SSIZE;
  new_flags &= ~FSP_FLAGS_MASK_PAGE_SSIZE;

  const page_size_t old_page_size(old_flags);
  const page_size_t new_page_size(new_flags);
  const ulint old_sdi_offset = fsp_header_get_sdi_offset(old_page_size);
  const ulint new_sdi_offset = fsp_header_get_sdi_offset(new_page_size);

  if (FSP_FLAGS_HAS_SDI(old_flags) && old_sdi_offset != new_sdi_offset) {
    const uint32_t sdi_version = mach_read_from_4(page + old_sdi_offset);
    const uint32_t sdi_root = mach_read_from_4(page + old_sdi_offset + 4);

    if (sdi_version != 0) {
      mach_write_to_4(page + new_sdi_offset, sdi_version);
      mach_write_to_4(page + new_sdi_offset + 4, sdi_root);
      mach_write_to_4(page + old_sdi_offset, 0);
      mach_write_to_4(page + old_sdi_offset + 4, 0);
    }
  }

  fsp_header_set_field(page, FSP_SPACE_FLAGS, new_flags);
  fsp_header_set_field(page, FSP_SPACE_ID, space_id);

  *out_space_id = space_id;
  return true;
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
        case 17853: return "FIL_PAGE_SDI";
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

// ----------------------------------------------------------------
// Experimental: rebuild compressed tablespace into 16KB pages
// ----------------------------------------------------------------
bool rebuild_uncompressed_ibd(File in_fd, File out_fd, const char* sdi_json_path)
{
  MY_STAT stat_info;
  if (my_fstat(in_fd, &stat_info) != 0) {
    fprintf(stderr, "Cannot fstat() input file.\n");
    return false;
  }

  page_size_t pg_sz(0, 0, false);
  if (!determine_page_size(in_fd, pg_sz)) {
    fprintf(stderr, "Could not determine page size.\n");
    return false;
  }

  const size_t physical_size = pg_sz.physical();
  const size_t logical_size = pg_sz.logical();

  if (physical_size >= logical_size) {
    fprintf(stderr, "Input tablespace does not appear compressed.\n");
    return false;
  }

  if (logical_size != UNIV_PAGE_SIZE_ORIG) {
    fprintf(stderr, "Only 16KB logical pages are supported for rebuild.\n");
    return false;
  }

  if (stat_info.st_size % physical_size != 0) {
    fprintf(stderr, "File size is not a multiple of physical page size.\n");
    return false;
  }

  ut_crc32_init();

  const uint64_t total_bytes = stat_info.st_size;
  const uint64_t num_pages = total_bytes / physical_size;

  std::unique_ptr<unsigned char[]> in_buf(new unsigned char[physical_size]);
  std::unique_ptr<unsigned char[]> out_buf(new unsigned char[logical_size]);

  std::vector<SdiEntry> sdi_entries;
  bool have_sdi_json = (sdi_json_path != nullptr);
  page_no_t sdi_root_page = FIL_NULL;
  bool sdi_root_set = false;

  if (have_sdi_json) {
    if (!load_sdi_json_entries(sdi_json_path, sdi_entries)) {
      return false;
    }
  }

  space_id_t space_id = SPACE_UNKNOWN;

  fprintf(stderr, "\n========================================\n");
  fprintf(stderr, "REBUILD STARTING (EXPERIMENTAL)\n");
  fprintf(stderr, "========================================\n");
  fprintf(stderr, "Input file size: %llu bytes\n",
          (unsigned long long)total_bytes);
  fprintf(stderr, "Physical page size: %zu, Logical page size: %zu\n",
          physical_size, logical_size);
  fprintf(stderr, "Total pages: %llu\n", (unsigned long long)num_pages);
  fprintf(stderr, "========================================\n\n");

  for (uint64_t page_no = 0; page_no < num_pages; ++page_no) {
    if (!seek_page(in_fd, pg_sz, static_cast<page_no_t>(page_no))) {
      return false;
    }

    size_t r = my_read(in_fd, in_buf.get(), physical_size, MYF(0));
    if (r != physical_size) {
      fprintf(stderr, "Failed to read page %llu.\n",
              (unsigned long long)page_no);
      return false;
    }

    size_t actual_size = 0;
    if (!decompress_page_inplace(in_buf.get(), physical_size, logical_size,
                                 out_buf.get(), logical_size, &actual_size)) {
      fprintf(stderr, "Failed to decompress page %llu.\n",
              (unsigned long long)page_no);
      return false;
    }

    if (page_no == 0) {
      if (have_sdi_json) {
        const uint32_t old_flags = fsp_header_get_flags(in_buf.get());
        if (!FSP_FLAGS_HAS_SDI(old_flags)) {
          fprintf(stderr,
                  "Error: SDI JSON provided but tablespace has no SDI flag.\n");
          return false;
        }
        const page_size_t old_page_size(old_flags);
        const ulint sdi_offset = fsp_header_get_sdi_offset(old_page_size);
        const uint32_t sdi_version =
            mach_read_from_4(in_buf.get() + sdi_offset);
        sdi_root_page = mach_read_from_4(in_buf.get() + sdi_offset + 4);
        sdi_root_set = (sdi_root_page != 0 && sdi_root_page != FIL_NULL);
        fprintf(stderr,
                "SDI header: version=%u root_page=%u (json=%s)\n",
                sdi_version, sdi_root_page, sdi_json_path);
      }

      if (!update_tablespace_header_for_uncompressed(out_buf.get(),
                                                     logical_size,
                                                     &space_id)) {
        return false;
      }

      if (have_sdi_json) {
        if (!sdi_root_set || sdi_root_page >= num_pages) {
          fprintf(stderr,
                  "Error: invalid SDI root page (%u) for %llu pages\n",
                  sdi_root_page, (unsigned long long)num_pages);
          return false;
        }
        const uint32_t new_flags = fsp_header_get_flags(out_buf.get());
        const page_size_t new_page_size(new_flags);
        const ulint sdi_offset = fsp_header_get_sdi_offset(new_page_size);
        mach_write_to_4(out_buf.get() + sdi_offset, SDI_VERSION);
        mach_write_to_4(out_buf.get() + sdi_offset + 4, sdi_root_page);
      }
    }

    if (space_id == SPACE_UNKNOWN) {
      fprintf(stderr, "Space id not set after page 0 processing.\n");
      return false;
    }

    if (have_sdi_json && sdi_root_set && page_no == sdi_root_page) {
      byte fseg_leaf[FSEG_HEADER_SIZE];
      byte fseg_top[FSEG_HEADER_SIZE];
      memcpy(fseg_leaf,
             out_buf.get() + FIL_PAGE_DATA + PAGE_BTR_SEG_LEAF,
             FSEG_HEADER_SIZE);
      memcpy(fseg_top,
             out_buf.get() + FIL_PAGE_DATA + PAGE_BTR_SEG_TOP,
             FSEG_HEADER_SIZE);

      init_empty_sdi_page(out_buf.get(), logical_size,
                          static_cast<page_no_t>(page_no));
      memcpy(out_buf.get() + FIL_PAGE_DATA + PAGE_BTR_SEG_LEAF, fseg_leaf,
             FSEG_HEADER_SIZE);
      memcpy(out_buf.get() + FIL_PAGE_DATA + PAGE_BTR_SEG_TOP, fseg_top,
             FSEG_HEADER_SIZE);

      if (!populate_sdi_root_page(out_buf.get(), logical_size, sdi_entries)) {
        fprintf(stderr, "Error: SDI root page rebuild failed.\n");
        return false;
      }
    }

    mach_write_to_4(out_buf.get() + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, space_id);
    stamp_page_lsn_and_crc32(out_buf.get(), logical_size, 0);

    size_t w = my_write(out_fd, (uchar*)out_buf.get(), logical_size, MYF(0));
    if (w != logical_size) {
      fprintf(stderr, "Failed to write page %llu.\n",
              (unsigned long long)page_no);
      return false;
    }

    if ((page_no + 1) % 100 == 0 || (page_no + 1) == num_pages) {
      fprintf(stderr, "[PROGRESS] Rebuilt %llu/%llu pages (%.1f%%)\n",
              (unsigned long long)(page_no + 1),
              (unsigned long long)num_pages,
              100.0 * (page_no + 1) / num_pages);
    }
  }

  fprintf(stderr, "\n========================================\n");
  fprintf(stderr, "REBUILD COMPLETE (EXPERIMENTAL)\n");
  fprintf(stderr, "========================================\n");
  fprintf(stderr, "Output pages written: %llu\n", (unsigned long long)num_pages);
  fprintf(stderr, "========================================\n\n");

  return true;
}
