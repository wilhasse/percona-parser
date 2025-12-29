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
#include <cctype>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <zlib.h>

// MySQL/Percona headers
#include "my_dbug.h"
#include "my_dir.h"
#include "my_getopt.h"
#include "my_io.h"
#include "my_sys.h"
#include "mysql_com.h"
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
// dict0dd.h excluded - pulls in inline code incompatible with standalone build
#include "fil0fil.h"
#include "fsp0fsp.h"
#include "fsp0types.h"
#include "mach0data.h"
#include "page0page.h"
#include "page0size.h"
#include "page0types.h"
#include "sql/dd/types/column.h"
#include "sql/dd/types/index.h"
#include "sql/dd/types/table.h"
#include "sql/my_decimal.h"
#include "sql/sql_const.h"
#include "univ.i"
#include "ut0byte.h"
#include "ut0crc32.h"
#include "m_ctype.h"
//#include "page/zipdecompress.h" // Has page_zip_decompress_low()

/*
  In real InnoDB code, these are declared in e.g. "srv0srv.h" or "univ.i"
  but for our demo, we define them here:
*/
ulong srv_page_size       = 0;
ulong srv_page_size_shift = 0;
page_size_t univ_page_size(0, 0, false);

// CFG export version - from row0quiesce.h (standalone definition to avoid dependencies)
constexpr uint32_t IB_EXPORT_CFG_VERSION_V7 = 7;

// From dict0dd.h - defined locally to avoid pulling in incompatible inline code
enum dd_index_keys {
  DD_INDEX_ID,
  DD_INDEX_SPACE_ID,
  DD_TABLE_ID,
  DD_INDEX_ROOT,
  DD_INDEX_TRX_ID,
  DD_INDEX__LAST
};
static const char* const dd_index_key_strings[DD_INDEX__LAST] = {
    "id", "space_id", "table_id", "root", "trx_id"};

enum dd_column_keys {
  DD_INSTANT_COLUMN_DEFAULT,
  DD_INSTANT_COLUMN_DEFAULT_NULL,
  DD_INSTANT_VERSION_ADDED,
  DD_INSTANT_VERSION_DROPPED,
  DD_INSTANT_PHYSICAL_POS,
  DD_COLUMN__LAST
};
static const char* const dd_column_key_strings[DD_COLUMN__LAST] = {
    "default", "default_null", "version_added", "version_dropped",
    "physical_pos"};

enum dd_space_keys {
  DD_SPACE_FLAGS,
  DD_SPACE_ID,
  DD_SPACE_DISCARD,
  DD_SPACE_SERVER_VERSION,
  DD_SPACE_VERSION,
  DD_SPACE_STATE,
  DD_SPACE__LAST
};
static const char* const dd_space_key_strings[DD_SPACE__LAST] = {
    "flags", "id", "discard", "server_version", "space_version", "state"};

enum dd_table_keys {
  DD_TABLE_AUTOINC,
  DD_TABLE_DATA_DIRECTORY,
  DD_TABLE_VERSION,
  DD_TABLE_DISCARD,
  DD_TABLE_INSTANT_COLS,
  DD_TABLE__LAST
};
static const char* const dd_table_key_strings[DD_TABLE__LAST] = {
    "autoinc", "data_directory", "version", "discard", "instant_col"};

// From sql/dd/types/index_element.h
namespace dd {
class Index_element {
 public:
  enum enum_index_element_order { ORDER_UNDEF = 1, ORDER_ASC, ORDER_DESC };
};
}  // namespace dd

// Hex decoder to replace DD_instant_col_val_coder from dict0dd.h
class DD_instant_col_val_coder {
 public:
  DD_instant_col_val_coder() : m_result(nullptr), m_result_len(0) {}
  ~DD_instant_col_val_coder() { cleanup(); }

  const byte* decode(const char* stream, size_t in_len, size_t* out_len) {
    cleanup();
    if (in_len == 0 || (in_len % 2) != 0) {
      if (out_len != nullptr) {
        *out_len = 0;
      }
      return nullptr;
    }

    auto hex_val = [](unsigned char c) -> int {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
      }
      if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
      }
      return -1;
    };

    size_t decoded_len = in_len / 2;
    m_result = new byte[decoded_len];
    m_result_len = decoded_len;

    for (size_t i = 0; i < decoded_len; ++i) {
      int hi = hex_val(static_cast<unsigned char>(stream[i * 2]));
      int lo = hex_val(static_cast<unsigned char>(stream[i * 2 + 1]));
      if (hi < 0 || lo < 0) {
        cleanup();
        if (out_len != nullptr) {
          *out_len = 0;
        }
        return nullptr;
      }
      m_result[i] = static_cast<byte>((hi << 4) | lo);
    }

    if (out_len != nullptr) {
      *out_len = decoded_len;
    }
    return m_result;
  }

 private:
  void cleanup() {
    delete[] m_result;
    m_result = nullptr;
    m_result_len = 0;
  }
  byte* m_result;
  size_t m_result_len;
};

// Stubs for dtype functions from data0type.cc
bool dtype_is_string_type(ulint mtype) {
  if (mtype <= DATA_BLOB || mtype == DATA_MYSQL || mtype == DATA_VARMYSQL) {
    return true;
  }
  return false;
}

ulint dtype_form_prtype(ulint old_prtype, ulint charset_coll) {
  return (old_prtype + (charset_coll << 16));
}

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
constexpr uint32_t kSdiExternRefSize = FIELD_REF_SIZE;
constexpr uint32_t kSdiExternSpaceId = 0;
constexpr uint32_t kSdiExternPageNo = 4;
constexpr uint32_t kSdiExternOffset = 8;
constexpr uint32_t kSdiExternLen = 12;
constexpr uint32_t kSdiLobHdrPartLen = 0;
constexpr uint32_t kSdiLobHdrNextPageNo = 4;
constexpr uint32_t kSdiLobHdrSize = 8;

struct SdiBlobAlloc {
  const std::vector<page_no_t>* pages{nullptr};
  size_t next{0};
  size_t page_size{0};
  space_id_t space_id{SPACE_UNKNOWN};
  std::unordered_map<page_no_t, std::vector<byte>>* out_pages{nullptr};
};

static void stamp_page_lsn_and_crc32(unsigned char* page,
                                     size_t page_size,
                                     uint64_t lsn);

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

// ----------------------------------------------------------------
// SDI metadata parsing for .cfg generation
// ----------------------------------------------------------------
constexpr uint32_t kPortableSizeOfCharPtr = 8;

struct SdiColumnInfo {
  std::string name;
  dd::enum_column_types type{dd::enum_column_types::TYPE_NULL};
  bool is_nullable{true};
  bool is_unsigned{false};
  bool is_virtual{false};
  uint32_t hidden{0};
  uint32_t char_length{0};
  uint32_t numeric_scale{0};
  uint32_t collation_id{0};
  std::string se_private_data;
  std::vector<std::string> elements;
};

struct SdiIndexElementInfo {
  int column_opx{-1};
  uint32_t length{UINT32_MAX};
  uint32_t order{0};
  bool hidden{false};
};

struct SdiIndexInfo {
  std::string name;
  uint32_t type{0};
  std::string options;
  std::string se_private_data;
  std::vector<SdiIndexElementInfo> elements;
};

struct SdiTableInfo {
  std::string name;
  std::string schema;
  std::string options;
  std::string se_private_data;
  uint32_t row_format{0};
  std::vector<SdiColumnInfo> columns;
  std::vector<SdiIndexInfo> indexes;
};

struct SdiTablespaceInfo {
  std::string name;
  std::string options;
  std::string se_private_data;
  std::vector<std::string> files;
};

struct SdiMetadata {
  bool has_table{false};
  bool has_tablespace{false};
  SdiTableInfo table;
  SdiTablespaceInfo tablespace;
};

static bool sdi_read_string(const rapidjson::Value& val, std::string* out) {
  if (val.IsString()) {
    *out = val.GetString();
    return true;
  }
  return false;
}

static bool sdi_read_bool(const rapidjson::Value& val, bool* out) {
  if (val.IsBool()) {
    *out = val.GetBool();
    return true;
  }
  if (val.IsInt()) {
    *out = (val.GetInt() != 0);
    return true;
  }
  return false;
}

static bool sdi_read_uint32(const rapidjson::Value& val, uint32_t* out) {
  uint64_t tmp = 0;
  if (!sdi_read_uint64(val, &tmp)) {
    return false;
  }
  if (tmp > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  *out = static_cast<uint32_t>(tmp);
  return true;
}

static std::unordered_map<std::string, std::string> parse_kv_string(
    const std::string& input) {
  std::unordered_map<std::string, std::string> out;
  size_t pos = 0;
  while (pos < input.size()) {
    const size_t end = input.find(';', pos);
    const size_t len = (end == std::string::npos) ? input.size() - pos : end - pos;
    if (len > 0) {
      const std::string token = input.substr(pos, len);
      const size_t eq = token.find('=');
      if (eq != std::string::npos) {
        const std::string key = token.substr(0, eq);
        const std::string value = token.substr(eq + 1);
        if (!key.empty()) {
          out[key] = value;
        }
      } else if (!token.empty()) {
        out[token] = "";
      }
    }
    if (end == std::string::npos) {
      break;
    }
    pos = end + 1;
  }
  return out;
}

static bool parse_uint64_value(const std::string& s, uint64_t* out) {
  if (s.empty()) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  unsigned long long v = strtoull(s.c_str(), &end, 10);
  if (errno != 0 || end == s.c_str()) {
    return false;
  }
  *out = static_cast<uint64_t>(v);
  return true;
}

static bool parse_uint32_value(const std::string& s, uint32_t* out) {
  uint64_t tmp = 0;
  if (!parse_uint64_value(s, &tmp) ||
      tmp > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  *out = static_cast<uint32_t>(tmp);
  return true;
}

static bool file_exists(const std::string& path) {
  struct stat st;
  return !path.empty() && stat(path.c_str(), &st) == 0;
}

static bool resolve_tablespace_path(const std::string& path,
                                    std::string* resolved) {
  if (resolved == nullptr || path.empty()) {
    return false;
  }

  if (file_exists(path)) {
    *resolved = path;
    return true;
  }

  std::string trimmed = path;
  if (trimmed.rfind("./", 0) == 0) {
    trimmed = trimmed.substr(2);
  } else if (trimmed.rfind(".\\", 0) == 0) {
    trimmed = trimmed.substr(2);
  }

  const char* datadir = std::getenv("MYSQL_DATADIR");
  if (datadir == nullptr || *datadir == '\0') {
    datadir = std::getenv("IB_PARSER_DATADIR");
  }

  if (datadir != nullptr && *datadir != '\0') {
    std::string candidate(datadir);
    if (!candidate.empty() && candidate.back() != '/') {
      candidate.push_back('/');
    }
    candidate.append(trimmed);
    if (file_exists(candidate)) {
      *resolved = std::move(candidate);
      return true;
    }
  }

  return false;
}

static bool read_sdi_root_from_tablespace(const std::string& path,
                                          page_no_t* root_page,
                                          uint32_t* version,
                                          std::string* err) {
  if (root_page == nullptr || version == nullptr) {
    if (err) {
      *err = "invalid output pointers";
    }
    return false;
  }

  File fd = my_open(path.c_str(), O_RDONLY, MYF(0));
  if (fd < 0) {
    if (err) {
      *err = "cannot open target tablespace file";
    }
    return false;
  }

  page_size_t pg_sz(0, 0, false);
  if (!determine_page_size(fd, pg_sz)) {
    my_close(fd, MYF(0));
    if (err) {
      *err = "could not determine target page size";
    }
    return false;
  }

  const size_t physical_size = pg_sz.physical();
  std::vector<byte> buf(physical_size);
  if (my_seek(fd, 0, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
    my_close(fd, MYF(0));
    if (err) {
      *err = "seek failed";
    }
    return false;
  }
  size_t r = my_read(fd, buf.data(), physical_size, MYF(0));
  my_close(fd, MYF(0));
  if (r != physical_size) {
    if (err) {
      *err = "failed to read page 0";
    }
    return false;
  }

  const uint32_t flags = fsp_header_get_flags(buf.data());
  if (!fsp_flags_is_valid(flags)) {
    if (err) {
      *err = "invalid FSP flags";
    }
    return false;
  }
  if (!FSP_FLAGS_HAS_SDI(flags)) {
    if (err) {
      *err = "tablespace has no SDI flag";
    }
    return false;
  }

  const page_size_t page_size(flags);
  const ulint sdi_offset = fsp_header_get_sdi_offset(page_size);
  *version = mach_read_from_4(buf.data() + sdi_offset);
  *root_page = mach_read_from_4(buf.data() + sdi_offset + 4);
  return true;
}

static std::string to_lower_copy(const std::string& input) {
  std::string out = input;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return out;
}

static bool extract_index_ids_by_name(
    const SdiMetadata& meta,
    std::unordered_map<std::string, uint64_t>* out,
    std::string* err) {
  if (out == nullptr) {
    if (err) {
      *err = "index-id output map is null";
    }
    return false;
  }
  out->clear();

  if (!meta.has_table) {
    if (err) {
      *err = "SDI metadata missing table object";
    }
    return false;
  }

  for (const auto& idx : meta.table.indexes) {
    if (idx.name.empty()) {
      continue;
    }
    const auto kv = parse_kv_string(idx.se_private_data);
    auto id_it = kv.find(dd_index_key_strings[DD_INDEX_ID]);
    if (id_it == kv.end()) {
      continue;
    }
    uint64_t id = 0;
    if (!parse_uint64_value(id_it->second, &id)) {
      continue;
    }
    (*out)[to_lower_copy(idx.name)] = id;
  }

  if (out->empty()) {
    if (err) {
      *err = "no index ids found in SDI metadata";
    }
    return false;
  }
  return true;
}

static bool build_index_id_remap_from_sdi(
    const SdiMetadata& source,
    const SdiMetadata& target,
    std::unordered_map<uint64_t, uint64_t>* out,
    std::string* err) {
  if (out == nullptr) {
    if (err) {
      *err = "index-id remap output is null";
    }
    return false;
  }

  std::unordered_map<std::string, uint64_t> src_by_name;
  std::unordered_map<std::string, uint64_t> tgt_by_name;
  std::string src_err;
  std::string tgt_err;
  if (!extract_index_ids_by_name(source, &src_by_name, &src_err)) {
    if (err) {
      *err = "source SDI: " + src_err;
    }
    return false;
  }
  if (!extract_index_ids_by_name(target, &tgt_by_name, &tgt_err)) {
    if (err) {
      *err = "target SDI: " + tgt_err;
    }
    return false;
  }

  out->clear();
  for (const auto& entry : src_by_name) {
    auto it = tgt_by_name.find(entry.first);
    if (it == tgt_by_name.end()) {
      continue;
    }
    if (entry.second != 0 && it->second != 0) {
      (*out)[entry.second] = it->second;
    }
  }

  if (out->empty()) {
    if (err) {
      *err = "no matching index ids between source and target SDI";
    }
    return false;
  }
  return true;
}

static bool load_index_id_map_file(
    const char* path,
    std::unordered_map<uint64_t, uint64_t>* out,
    std::string* err) {
  if (out == nullptr) {
    if (err) {
      *err = "index-id map output is null";
    }
    return false;
  }

  std::ifstream ifs(path);
  if (!ifs.is_open()) {
    if (err) {
      *err = "cannot open index-id map file";
    }
    return false;
  }

  std::string line;
  size_t line_no = 0;
  while (std::getline(ifs, line)) {
    line_no++;
    auto hash = line.find('#');
    if (hash != std::string::npos) {
      line.resize(hash);
    }
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    line.erase(line.begin(),
               std::find_if(line.begin(), line.end(),
                            [&](unsigned char ch) { return !is_space(ch); }));
    while (!line.empty() && is_space(static_cast<unsigned char>(line.back()))) {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }

    std::string left;
    std::string right;
    auto eq = line.find('=');
    if (eq != std::string::npos) {
      left = line.substr(0, eq);
      right = line.substr(eq + 1);
    } else {
      std::istringstream iss(line);
      iss >> left >> right;
    }
    if (left.empty() || right.empty()) {
      if (err) {
        *err = "invalid mapping at line " + std::to_string(line_no);
      }
      return false;
    }

    uint64_t src = 0;
    uint64_t dst = 0;
    if (!parse_uint64_value(left, &src) || !parse_uint64_value(right, &dst)) {
      if (err) {
        *err = "invalid mapping at line " + std::to_string(line_no);
      }
      return false;
    }
    (*out)[src] = dst;
  }

  if (out->empty()) {
    if (err) {
      *err = "index-id map file is empty";
    }
    return false;
  }
  return true;
}

static bool load_sdi_metadata(const char* json_path, SdiMetadata* meta) {
  if (meta == nullptr) {
    return false;
  }

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

  meta->has_table = false;
  meta->has_tablespace = false;

  for (const auto& elem : doc.GetArray()) {
    if (!elem.IsObject() || !elem.HasMember("object")) {
      continue;
    }
    const auto& obj = elem["object"];
    if (!obj.HasMember("dd_object_type") || !obj["dd_object_type"].IsString()) {
      continue;
    }

    const std::string dd_type = obj["dd_object_type"].GetString();
    if (!obj.HasMember("dd_object") || !obj["dd_object"].IsObject()) {
      continue;
    }
    const auto& dd_obj = obj["dd_object"];

    if (dd_type == "Table") {
      meta->has_table = true;
      SdiTableInfo info;
      if (dd_obj.HasMember("name")) {
        sdi_read_string(dd_obj["name"], &info.name);
      }
      if (dd_obj.HasMember("schema_ref")) {
        sdi_read_string(dd_obj["schema_ref"], &info.schema);
      }
      if (dd_obj.HasMember("options")) {
        sdi_read_string(dd_obj["options"], &info.options);
      }
      if (dd_obj.HasMember("se_private_data")) {
        sdi_read_string(dd_obj["se_private_data"], &info.se_private_data);
      }
      if (dd_obj.HasMember("row_format")) {
        sdi_read_uint32(dd_obj["row_format"], &info.row_format);
      }

      if (dd_obj.HasMember("columns") && dd_obj["columns"].IsArray()) {
        const auto& cols = dd_obj["columns"].GetArray();
        info.columns.reserve(cols.Size());
        for (const auto& c : cols) {
          if (!c.IsObject()) {
            continue;
          }
          SdiColumnInfo col;
          if (c.HasMember("name")) {
            sdi_read_string(c["name"], &col.name);
          }
          if (c.HasMember("type") && c["type"].IsUint()) {
            col.type = static_cast<dd::enum_column_types>(c["type"].GetUint());
          }
          if (c.HasMember("is_nullable")) {
            sdi_read_bool(c["is_nullable"], &col.is_nullable);
          }
          if (c.HasMember("is_unsigned")) {
            sdi_read_bool(c["is_unsigned"], &col.is_unsigned);
          }
          if (c.HasMember("is_virtual")) {
            sdi_read_bool(c["is_virtual"], &col.is_virtual);
          }
          if (c.HasMember("hidden")) {
            sdi_read_uint32(c["hidden"], &col.hidden);
          }
          if (c.HasMember("char_length")) {
            sdi_read_uint32(c["char_length"], &col.char_length);
          }
          if (c.HasMember("numeric_scale")) {
            sdi_read_uint32(c["numeric_scale"], &col.numeric_scale);
          }
          if (c.HasMember("collation_id")) {
            sdi_read_uint32(c["collation_id"], &col.collation_id);
          }
          if (c.HasMember("se_private_data")) {
            sdi_read_string(c["se_private_data"], &col.se_private_data);
          }
          if (c.HasMember("elements") && c["elements"].IsArray()) {
            for (const auto& el : c["elements"].GetArray()) {
              if (el.IsString()) {
                col.elements.emplace_back(el.GetString());
              } else if (el.IsObject() && el.HasMember("name") &&
                         el["name"].IsString()) {
                col.elements.emplace_back(el["name"].GetString());
              }
            }
          }
          info.columns.push_back(std::move(col));
        }
      }

      if (dd_obj.HasMember("indexes") && dd_obj["indexes"].IsArray()) {
        const auto& idxs = dd_obj["indexes"].GetArray();
        info.indexes.reserve(idxs.Size());
        for (const auto& idx : idxs) {
          if (!idx.IsObject()) {
            continue;
          }
          SdiIndexInfo index;
          if (idx.HasMember("name")) {
            sdi_read_string(idx["name"], &index.name);
          }
          if (idx.HasMember("type")) {
            sdi_read_uint32(idx["type"], &index.type);
          }
          if (idx.HasMember("options")) {
            sdi_read_string(idx["options"], &index.options);
          }
          if (idx.HasMember("se_private_data")) {
            sdi_read_string(idx["se_private_data"], &index.se_private_data);
          }
          if (idx.HasMember("elements") && idx["elements"].IsArray()) {
            const auto& elements = idx["elements"].GetArray();
            index.elements.reserve(elements.Size());
            for (const auto& el : elements) {
              if (!el.IsObject()) {
                continue;
              }
              SdiIndexElementInfo e;
              if (el.HasMember("column_opx") && el["column_opx"].IsInt()) {
                e.column_opx = el["column_opx"].GetInt();
              }
              if (el.HasMember("length")) {
                sdi_read_uint32(el["length"], &e.length);
              }
              if (el.HasMember("order")) {
                sdi_read_uint32(el["order"], &e.order);
              }
              if (el.HasMember("hidden")) {
                sdi_read_bool(el["hidden"], &e.hidden);
              }
              index.elements.push_back(e);
            }
          }
          info.indexes.push_back(std::move(index));
        }
      }

      meta->table = std::move(info);
    } else if (dd_type == "Tablespace") {
      meta->has_tablespace = true;
      SdiTablespaceInfo space;
      if (dd_obj.HasMember("name")) {
        sdi_read_string(dd_obj["name"], &space.name);
      }
      if (dd_obj.HasMember("options")) {
        sdi_read_string(dd_obj["options"], &space.options);
      }
      if (dd_obj.HasMember("se_private_data")) {
        sdi_read_string(dd_obj["se_private_data"], &space.se_private_data);
      }
      if (dd_obj.HasMember("files") && dd_obj["files"].IsArray()) {
        for (const auto& file : dd_obj["files"].GetArray()) {
          if (!file.IsObject()) {
            continue;
          }
          if (file.HasMember("filename") && file["filename"].IsString()) {
            space.files.emplace_back(file["filename"].GetString());
          }
        }
      }
      meta->tablespace = std::move(space);
    }
  }

  if (!meta->has_table) {
    fprintf(stderr, "Error: SDI JSON missing Table object\n");
    return false;
  }
  if (!meta->has_tablespace) {
    fprintf(stderr, "Warning: SDI JSON missing Tablespace object\n");
  }

  return true;
}

// ----------------------------------------------------------------
// Minimal type/length helpers for cfg generation (mirrors MySQL logic)
// ----------------------------------------------------------------
static unsigned int my_time_binary_length_local(unsigned int dec) {
  return 3 + (dec + 1) / 2;
}

static unsigned int my_datetime_binary_length_local(unsigned int dec) {
  return 5 + (dec + 1) / 2;
}

static unsigned int my_timestamp_binary_length_local(unsigned int dec) {
  return 4 + (dec + 1) / 2;
}

static uint32_t get_enum_pack_length_local(uint32_t elements) {
  return elements < 256 ? 1 : 2;
}

static uint32_t get_set_pack_length_local(uint32_t elements) {
  uint32_t len = (elements + 7) / 8;
  return len > 4 ? 8 : len;
}

static enum_field_types dd_get_old_field_type_local(dd::enum_column_types type) {
  switch (type) {
    case dd::enum_column_types::DECIMAL:
      return MYSQL_TYPE_DECIMAL;
    case dd::enum_column_types::TINY:
      return MYSQL_TYPE_TINY;
    case dd::enum_column_types::SHORT:
      return MYSQL_TYPE_SHORT;
    case dd::enum_column_types::LONG:
      return MYSQL_TYPE_LONG;
    case dd::enum_column_types::FLOAT:
      return MYSQL_TYPE_FLOAT;
    case dd::enum_column_types::DOUBLE:
      return MYSQL_TYPE_DOUBLE;
    case dd::enum_column_types::TYPE_NULL:
      return MYSQL_TYPE_NULL;
    case dd::enum_column_types::TIMESTAMP:
      return MYSQL_TYPE_TIMESTAMP;
    case dd::enum_column_types::LONGLONG:
      return MYSQL_TYPE_LONGLONG;
    case dd::enum_column_types::INT24:
      return MYSQL_TYPE_INT24;
    case dd::enum_column_types::DATE:
      return MYSQL_TYPE_DATE;
    case dd::enum_column_types::TIME:
      return MYSQL_TYPE_TIME;
    case dd::enum_column_types::DATETIME:
      return MYSQL_TYPE_DATETIME;
    case dd::enum_column_types::YEAR:
      return MYSQL_TYPE_YEAR;
    case dd::enum_column_types::NEWDATE:
      return MYSQL_TYPE_NEWDATE;
    case dd::enum_column_types::VARCHAR:
      return MYSQL_TYPE_VARCHAR;
    case dd::enum_column_types::BIT:
      return MYSQL_TYPE_BIT;
    case dd::enum_column_types::TIMESTAMP2:
      return MYSQL_TYPE_TIMESTAMP2;
    case dd::enum_column_types::DATETIME2:
      return MYSQL_TYPE_DATETIME2;
    case dd::enum_column_types::TIME2:
      return MYSQL_TYPE_TIME2;
    case dd::enum_column_types::NEWDECIMAL:
      return MYSQL_TYPE_NEWDECIMAL;
    case dd::enum_column_types::ENUM:
      return MYSQL_TYPE_ENUM;
    case dd::enum_column_types::SET:
      return MYSQL_TYPE_SET;
    case dd::enum_column_types::TINY_BLOB:
      return MYSQL_TYPE_TINY_BLOB;
    case dd::enum_column_types::MEDIUM_BLOB:
      return MYSQL_TYPE_MEDIUM_BLOB;
    case dd::enum_column_types::LONG_BLOB:
      return MYSQL_TYPE_LONG_BLOB;
    case dd::enum_column_types::BLOB:
      return MYSQL_TYPE_BLOB;
    case dd::enum_column_types::VAR_STRING:
      return MYSQL_TYPE_VAR_STRING;
    case dd::enum_column_types::STRING:
      return MYSQL_TYPE_STRING;
    case dd::enum_column_types::GEOMETRY:
      return MYSQL_TYPE_GEOMETRY;
    case dd::enum_column_types::JSON:
      return MYSQL_TYPE_JSON;
    default:
      break;
  }
  return MYSQL_TYPE_LONG;
}

static size_t calc_pack_length_local(enum_field_types type, size_t length) {
  switch (type) {
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_DECIMAL:
      return length;
    case MYSQL_TYPE_VARCHAR:
      return length + (length < 256 ? 1 : 2);
    case MYSQL_TYPE_BOOL:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_TINY:
      return 1;
    case MYSQL_TYPE_SHORT:
      return 2;
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_NEWDATE:
      return 3;
    case MYSQL_TYPE_TIME:
      return 3;
    case MYSQL_TYPE_TIME2:
      return length > MAX_TIME_WIDTH
                 ? my_time_binary_length_local(
                       length - MAX_TIME_WIDTH - 1)
                 : 3;
    case MYSQL_TYPE_TIMESTAMP:
      return 4;
    case MYSQL_TYPE_TIMESTAMP2:
      return length > MAX_DATETIME_WIDTH
                 ? my_timestamp_binary_length_local(
                       length - MAX_DATETIME_WIDTH - 1)
                 : 4;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_LONG:
      return 4;
    case MYSQL_TYPE_FLOAT:
      return sizeof(float);
    case MYSQL_TYPE_DOUBLE:
      return sizeof(double);
    case MYSQL_TYPE_DATETIME:
      return 8;
    case MYSQL_TYPE_DATETIME2:
      return length > MAX_DATETIME_WIDTH
                 ? my_datetime_binary_length_local(
                       length - MAX_DATETIME_WIDTH - 1)
                 : 5;
    case MYSQL_TYPE_LONGLONG:
      return 8;
    case MYSQL_TYPE_NULL:
      return 0;
    case MYSQL_TYPE_TINY_BLOB:
      return 1 + kPortableSizeOfCharPtr;
    case MYSQL_TYPE_BLOB:
      return 2 + kPortableSizeOfCharPtr;
    case MYSQL_TYPE_MEDIUM_BLOB:
      return 3 + kPortableSizeOfCharPtr;
    case MYSQL_TYPE_LONG_BLOB:
      return 4 + kPortableSizeOfCharPtr;
    case MYSQL_TYPE_GEOMETRY:
      return 4 + kPortableSizeOfCharPtr;
    case MYSQL_TYPE_JSON:
      return 4 + kPortableSizeOfCharPtr;
    case MYSQL_TYPE_BIT:
      return length / 8;
    default:
      break;
  }
  return 0;
}

static uint32_t calc_key_length_local(enum_field_types sql_type, uint32_t length,
                                      uint32_t decimals, bool is_unsigned,
                                      uint32_t elements) {
  uint precision;
  switch (sql_type) {
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_JSON:
      return 0;
    case MYSQL_TYPE_VARCHAR:
      return length;
    case MYSQL_TYPE_ENUM:
      return get_enum_pack_length_local(elements);
    case MYSQL_TYPE_SET:
      return get_set_pack_length_local(elements);
    case MYSQL_TYPE_BIT:
      return length / 8 + (length & 7 ? 1 : 0);
    case MYSQL_TYPE_NEWDECIMAL:
      precision = std::min<uint>(
          my_decimal_length_to_precision(length, decimals, is_unsigned),
          DECIMAL_MAX_PRECISION);
      return my_decimal_get_binary_size(precision, decimals);
    default:
      return static_cast<uint32_t>(calc_pack_length_local(sql_type, length));
  }
}

static size_t calc_pack_length_dd_local(dd::enum_column_types type,
                                        size_t char_length,
                                        size_t elements_count,
                                        bool treat_bit_as_char,
                                        uint numeric_scale,
                                        bool is_unsigned) {
  size_t pack_length = 0;
  switch (type) {
    case dd::enum_column_types::TINY_BLOB:
    case dd::enum_column_types::MEDIUM_BLOB:
    case dd::enum_column_types::LONG_BLOB:
    case dd::enum_column_types::BLOB:
    case dd::enum_column_types::GEOMETRY:
    case dd::enum_column_types::VAR_STRING:
    case dd::enum_column_types::STRING:
    case dd::enum_column_types::VARCHAR:
      pack_length =
          calc_pack_length_local(dd_get_old_field_type_local(type), char_length);
      break;
    case dd::enum_column_types::ENUM:
      pack_length = get_enum_pack_length_local(
          static_cast<uint32_t>(elements_count));
      break;
    case dd::enum_column_types::SET:
      pack_length =
          get_set_pack_length_local(static_cast<uint32_t>(elements_count));
      break;
    case dd::enum_column_types::BIT:
      pack_length =
          treat_bit_as_char ? ((char_length + 7) & ~7) / 8 : char_length / 8;
      break;
    case dd::enum_column_types::NEWDECIMAL: {
      uint decimals = numeric_scale;
      uint precision = std::min<uint>(
          my_decimal_length_to_precision(char_length, decimals, is_unsigned),
          DECIMAL_MAX_PRECISION);
      pack_length = my_decimal_get_binary_size(precision, decimals);
      break;
    }
    default:
      pack_length =
          calc_pack_length_local(dd_get_old_field_type_local(type), char_length);
      break;
  }
  return pack_length;
}

static ulint get_innobase_type_from_mysql_dd_type_local(
    ulint* unsigned_flag, ulint* binary_type, ulint* charset_no,
    dd::enum_column_types dd_type, const CHARSET_INFO* field_charset,
    bool is_unsigned) {
  *unsigned_flag = 0;
  *binary_type = DATA_BINARY_TYPE;
  *charset_no = 0;

  switch (dd_type) {
    case dd::enum_column_types::ENUM:
    case dd::enum_column_types::SET:
      *unsigned_flag = DATA_UNSIGNED;
      if (field_charset != &my_charset_bin) {
        *binary_type = 0;
      }
      return DATA_INT;
    case dd::enum_column_types::VAR_STRING:
    case dd::enum_column_types::VARCHAR:
      *charset_no = field_charset->number;
      if (field_charset == &my_charset_bin) {
        return DATA_BINARY;
      }
      *binary_type = 0;
      return (field_charset == &my_charset_latin1) ? DATA_VARCHAR
                                                   : DATA_VARMYSQL;
    case dd::enum_column_types::BIT:
      *unsigned_flag = DATA_UNSIGNED;
      *charset_no = my_charset_bin.number;
      return DATA_FIXBINARY;
    case dd::enum_column_types::STRING:
      *charset_no = field_charset->number;
      if (field_charset == &my_charset_bin) {
        return DATA_FIXBINARY;
      }
      *binary_type = 0;
      return (field_charset == &my_charset_latin1) ? DATA_CHAR : DATA_MYSQL;
    case dd::enum_column_types::DECIMAL:
    case dd::enum_column_types::FLOAT:
    case dd::enum_column_types::DOUBLE:
    case dd::enum_column_types::NEWDECIMAL:
    case dd::enum_column_types::LONG:
    case dd::enum_column_types::LONGLONG:
    case dd::enum_column_types::TINY:
    case dd::enum_column_types::SHORT:
    case dd::enum_column_types::INT24:
      if (is_unsigned) {
        *unsigned_flag = DATA_UNSIGNED;
      }
      if (dd_type == dd::enum_column_types::NEWDECIMAL) {
        *charset_no = my_charset_bin.number;
        return DATA_FIXBINARY;
      }
      return DATA_INT;
    case dd::enum_column_types::DATE:
    case dd::enum_column_types::NEWDATE:
    case dd::enum_column_types::TIME:
    case dd::enum_column_types::DATETIME:
      return DATA_INT;
    case dd::enum_column_types::YEAR:
    case dd::enum_column_types::TIMESTAMP:
      *unsigned_flag = DATA_UNSIGNED;
      return DATA_INT;
    case dd::enum_column_types::TIME2:
    case dd::enum_column_types::DATETIME2:
    case dd::enum_column_types::TIMESTAMP2:
      *charset_no = my_charset_bin.number;
      return DATA_FIXBINARY;
    case dd::enum_column_types::GEOMETRY:
      return DATA_GEOMETRY;
    case dd::enum_column_types::TINY_BLOB:
    case dd::enum_column_types::MEDIUM_BLOB:
    case dd::enum_column_types::BLOB:
    case dd::enum_column_types::LONG_BLOB:
      *charset_no = field_charset->number;
      if (field_charset != &my_charset_bin) {
        *binary_type = 0;
      }
      return DATA_BLOB;
    case dd::enum_column_types::JSON:
      *charset_no = my_charset_utf8mb4_bin.number;
      return DATA_BLOB;
    case dd::enum_column_types::TYPE_NULL:
      *charset_no = field_charset->number;
      if (field_charset != &my_charset_bin) {
        *binary_type = 0;
      }
      break;
    default:
      break;
  }

  return 0;
}

struct ColumnTypeInfo {
  ulint mtype{0};
  ulint prtype{0};
  ulint len{0};
  ulint mbminmaxlen{0};
  bool is_nullable{true};
  bool is_unsigned{false};
};

struct CfgColumn {
  std::string name;
  dd::enum_column_types dd_type{dd::enum_column_types::TYPE_NULL};
  uint32_t prtype{0};
  uint32_t mtype{0};
  uint32_t len{0};
  uint32_t mbminmaxlen{0};
  uint32_t ind{0};
  uint32_t ord_part{0};
  uint32_t max_prefix{0};
  uint32_t char_length{0};
  uint32_t numeric_scale{0};
  uint64_t collation_id{0};
  bool is_nullable{true};
  bool is_unsigned{false};
  bool is_instant_dropped{false};
  uint8_t version_added{UINT8_UNDEFINED};
  uint8_t version_dropped{UINT8_UNDEFINED};
  uint32_t phy_pos{UINT32_UNDEFINED};
  bool has_instant_default{false};
  bool instant_default_null{false};
  std::vector<byte> instant_default_value;
  std::vector<std::string> elements;
};

struct CfgIndexField {
  std::string name;
  uint32_t prefix_len{0};
  uint32_t fixed_len{0};
  uint32_t is_ascending{1};
};

struct CfgIndex {
  std::string name;
  uint64_t id{0};
  uint32_t space{0};
  uint32_t page{0};
  uint32_t type{0};
  uint32_t trx_id_offset{0};
  uint32_t n_user_defined_cols{0};
  uint32_t n_uniq{0};
  uint32_t n_nullable{0};
  uint32_t n_fields{0};
  std::vector<CfgIndexField> fields;
};

struct CfgTable {
  std::string name;
  uint64_t autoinc{0};
  uint32_t table_flags{0};
  uint32_t space_flags{0};
  uint32_t n_instant_nullable{0};
  uint32_t initial_col_count{0};
  uint32_t current_col_count{0};
  uint32_t total_col_count{0};
  uint32_t n_instant_drop_cols{0};
  uint32_t current_row_version{0};
  uint8_t compression_type{0};
  bool has_row_versions{false};
  bool is_comp{true};
  std::vector<CfgColumn> columns;
  std::vector<CfgIndex> indexes;
};

static bool is_system_column_name(const std::string& name) {
  return name.rfind("DB_ROW_ID", 0) == 0 ||
         name.rfind("DB_TRX_ID", 0) == 0 ||
         name.rfind("DB_ROLL_PTR", 0) == 0;
}

static const CHARSET_INFO* resolve_charset(uint32_t collation_id) {
  if (collation_id == 0) {
    return &my_charset_bin;
  }
  CHARSET_INFO* cs = get_charset(collation_id, MYF(0));
  return cs != nullptr ? cs : &my_charset_bin;
}

static bool build_column_type_info(const SdiColumnInfo& col,
                                   ColumnTypeInfo* out) {
  if (out == nullptr) {
    return false;
  }

  const CHARSET_INFO* charset = resolve_charset(col.collation_id);

  ulint unsigned_flag = 0;
  ulint binary_type = 0;
  ulint charset_no = 0;
  ulint mtype = get_innobase_type_from_mysql_dd_type_local(
      &unsigned_flag, &binary_type, &charset_no, col.type, charset,
      col.is_unsigned);

  size_t col_len = calc_pack_length_dd_local(
      col.type, col.char_length, col.elements.size(), true, col.numeric_scale,
      col.is_unsigned);

  ulint long_true_varchar = 0;
  if (col.type == dd::enum_column_types::VARCHAR) {
    size_t length_bytes = col.char_length > 255 ? 2 : 1;
    if (col_len >= length_bytes) {
      col_len -= length_bytes;
    }
    if (length_bytes == 2) {
      long_true_varchar = DATA_LONG_TRUE_VARCHAR;
    }
  }

  ulint nulls_allowed = col.is_nullable ? 0 : DATA_NOT_NULL;
  ulint prtype = dtype_form_prtype(
      static_cast<ulint>(dd_get_old_field_type_local(col.type)) |
          unsigned_flag | binary_type | nulls_allowed | long_true_varchar,
      charset_no);

  ulint mbminmaxlen = 0;
  if (dtype_is_string_type(mtype)) {
    mbminmaxlen = DATA_MBMINMAXLEN(charset->mbminlen, charset->mbmaxlen);
  }

  out->mtype = mtype;
  out->prtype = prtype;
  out->len = col_len;
  out->mbminmaxlen = mbminmaxlen;
  out->is_nullable = col.is_nullable;
  out->is_unsigned = col.is_unsigned;
  return true;
}

static uint32_t calc_prefix_len(const SdiColumnInfo& col,
                                const SdiIndexElementInfo& elem) {
  if (elem.length == UINT32_MAX) {
    return 0;
  }
  const enum_field_types sql_type = dd_get_old_field_type_local(col.type);
  const uint32_t full_len = calc_key_length_local(
      sql_type, col.char_length, col.numeric_scale, col.is_unsigned,
      static_cast<uint32_t>(col.elements.size()));
  if (full_len != 0 && elem.length >= full_len) {
    return 0;
  }
  return elem.length;
}

static uint32_t calc_fixed_len(const ColumnTypeInfo& type_info, bool comp,
                               uint32_t prefix_len, bool is_spatial,
                               bool is_first_field) {
  ulint fixed_len =
      dtype_get_fixed_size_low(type_info.mtype, type_info.prtype,
                               type_info.len, type_info.mbminmaxlen, comp);

  if (is_spatial && is_first_field && DATA_POINT_MTYPE(type_info.mtype)) {
    fixed_len = DATA_MBR_LEN;
  }

  if (prefix_len && fixed_len > prefix_len) {
    fixed_len = prefix_len;
  }

  if (fixed_len > DICT_MAX_FIXED_COL_LEN) {
    fixed_len = 0;
  }

  return static_cast<uint32_t>(fixed_len);
}

static std::string table_full_name(const SdiTableInfo& table) {
  if (!table.schema.empty()) {
    return table.schema + "/" + table.name;
  }
  return table.name;
}

static bool decode_instant_default(
    const std::unordered_map<std::string, std::string>& kv,
    std::vector<byte>* out_value, bool* out_null, bool* out_has_default) {
  if (out_value == nullptr || out_null == nullptr || out_has_default == nullptr) {
    return false;
  }

  const auto def_it = kv.find(dd_column_key_strings[DD_INSTANT_COLUMN_DEFAULT]);
  const auto def_null_it =
      kv.find(dd_column_key_strings[DD_INSTANT_COLUMN_DEFAULT_NULL]);

  if (def_null_it != kv.end()) {
    *out_has_default = true;
    *out_null = true;
    return true;
  }

  if (def_it == kv.end()) {
    *out_has_default = false;
    *out_null = false;
    return true;
  }

  DD_instant_col_val_coder coder;
  size_t out_len = 0;
  const byte* decoded = coder.decode(def_it->second.c_str(),
                                     def_it->second.size(), &out_len);
  if (decoded == nullptr) {
    return false;
  }

  out_value->assign(decoded, decoded + out_len);
  *out_has_default = true;
  *out_null = false;
  return true;
}

static uint8_t parse_row_version(const std::unordered_map<std::string, std::string>& kv,
                                 size_t key_index) {
  uint32_t version = UINT32_UNDEFINED;
  const auto it = kv.find(dd_column_key_strings[key_index]);
  if (it != kv.end() && parse_uint32_value(it->second, &version)) {
    if (version <= std::numeric_limits<uint8_t>::max()) {
      return static_cast<uint8_t>(version);
    }
  }
  return UINT8_UNDEFINED;
}

static bool build_cfg_table_from_sdi(const SdiMetadata& meta,
                                     uint32_t space_flags,
                                     page_no_t sdi_root_page,
                                     space_id_t space_id,
                                     CfgTable* out) {
  if (out == nullptr) {
    return false;
  }

  CfgTable cfg;
  cfg.name = table_full_name(meta.table);
  cfg.space_flags = space_flags;

  const auto table_kv = parse_kv_string(meta.table.se_private_data);
  const auto space_kv = parse_kv_string(meta.tablespace.se_private_data);
  const auto options_kv = parse_kv_string(meta.table.options);

  uint64_t autoinc = 0;
  auto autoinc_it = table_kv.find(dd_table_key_strings[DD_TABLE_AUTOINC]);
  if (autoinc_it != table_kv.end()) {
    parse_uint64_value(autoinc_it->second, &autoinc);
  }
  cfg.autoinc = autoinc;

  bool data_dir = false;
  if (table_kv.find(dd_table_key_strings[DD_TABLE_DATA_DIRECTORY]) !=
      table_kv.end()) {
    data_dir = true;
  }

  bool shared_space = true;
  if (!meta.tablespace.name.empty()) {
    if (meta.tablespace.name.find('/') != std::string::npos) {
      shared_space = false;
    }
  } else {
    shared_space = false;
  }

  uint32_t zip_ssize = FSP_FLAGS_GET_ZIP_SSIZE(space_flags);
  if (zip_ssize != 0) {
    auto kb_it = options_kv.find("key_block_size");
    if (kb_it != options_kv.end()) {
      uint32_t kb = 0;
      if (parse_uint32_value(kb_it->second, &kb) && kb > 0) {
        uint32_t zip_size = kb * 1024;
        uint32_t shift = 0;
        while (zip_size > 512) {
          zip_size >>= 1;
          shift++;
        }
        if (shift > 0) {
          zip_ssize = shift - 1;
        }
      }
    }
  }

  bool compact = true;
  bool atomic_blobs = true;
  switch (static_cast<dd::Table::enum_row_format>(meta.table.row_format)) {
    case dd::Table::RF_REDUNDANT:
      compact = false;
      atomic_blobs = false;
      zip_ssize = 0;
      break;
    case dd::Table::RF_COMPACT:
      compact = true;
      atomic_blobs = false;
      zip_ssize = 0;
      break;
    case dd::Table::RF_COMPRESSED:
      compact = true;
      atomic_blobs = true;
      break;
    case dd::Table::RF_DYNAMIC:
    default:
      compact = true;
      atomic_blobs = true;
      zip_ssize = 0;
      break;
  }

  cfg.table_flags = dict_tf_init(compact, zip_ssize, atomic_blobs, data_dir,
                                 shared_space);
  cfg.is_comp = compact;

  auto compress_it = options_kv.find("compress");
  if (compress_it != options_kv.end()) {
    std::string c = compress_it->second;
    std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    if (c == "zlib") {
      cfg.compression_type = 1;
    } else if (c == "lz4") {
      cfg.compression_type = 2;
    }
  }

  const size_t total_cols = meta.table.columns.size();
  std::vector<ColumnTypeInfo> col_types(total_cols);
  std::vector<bool> col_dropped(total_cols, false);
  std::vector<bool> col_has_phy(total_cols, false);

  cfg.columns.clear();
  cfg.columns.reserve(total_cols);
  std::vector<int> opx_to_col_index(total_cols, -1);

  for (size_t i = 0; i < total_cols; ++i) {
    const auto& col = meta.table.columns[i];

    ColumnTypeInfo type_info;
    if (!build_column_type_info(col, &type_info)) {
      fprintf(stderr, "Error: failed to build column type for %s\n",
              col.name.c_str());
      return false;
    }
    col_types[i] = type_info;

    const auto kv = parse_kv_string(col.se_private_data);
    const uint8_t v_added =
        parse_row_version(kv, DD_INSTANT_VERSION_ADDED);
    const uint8_t v_dropped =
        parse_row_version(kv, DD_INSTANT_VERSION_DROPPED);
    if (v_dropped != UINT8_UNDEFINED && v_dropped > 0) {
      col_dropped[i] = true;
    }

    const auto phy_it = kv.find(dd_column_key_strings[DD_INSTANT_PHYSICAL_POS]);
    uint32_t phy_pos = UINT32_UNDEFINED;
    if (phy_it != kv.end() && parse_uint32_value(phy_it->second, &phy_pos)) {
      col_has_phy[i] = true;
    }

    if (col.is_virtual) {
      continue;
    }

    CfgColumn cfg_col;
    cfg_col.name = col.name;
    cfg_col.dd_type = col.type;

    // System columns need special handling - they use mtype=DATA_SYS
    // and prtype encodes the column identifier + DATA_NOT_NULL.
    // Also update col_types so calc_fixed_len works correctly for indexes.
    if (col.name == "DB_TRX_ID") {
      cfg_col.prtype = DATA_TRX_ID | DATA_NOT_NULL;  // 1 | 256 = 257
      cfg_col.mtype = DATA_SYS;
      cfg_col.len = DATA_TRX_ID_LEN;
      cfg_col.mbminmaxlen = 0;
      col_types[i].mtype = DATA_SYS;
      col_types[i].prtype = DATA_TRX_ID | DATA_NOT_NULL;
      col_types[i].len = DATA_TRX_ID_LEN;
      col_types[i].mbminmaxlen = 0;
    } else if (col.name == "DB_ROLL_PTR") {
      cfg_col.prtype = DATA_ROLL_PTR | DATA_NOT_NULL;  // 2 | 256 = 258
      cfg_col.mtype = DATA_SYS;
      cfg_col.len = DATA_ROLL_PTR_LEN;
      cfg_col.mbminmaxlen = 0;
      col_types[i].mtype = DATA_SYS;
      col_types[i].prtype = DATA_ROLL_PTR | DATA_NOT_NULL;
      col_types[i].len = DATA_ROLL_PTR_LEN;
      col_types[i].mbminmaxlen = 0;
    } else {
      cfg_col.prtype = static_cast<uint32_t>(type_info.prtype);
      cfg_col.mtype = static_cast<uint32_t>(type_info.mtype);
      cfg_col.len = static_cast<uint32_t>(type_info.len);
      cfg_col.mbminmaxlen = static_cast<uint32_t>(type_info.mbminmaxlen);
    }
    cfg_col.char_length = col.char_length;
    cfg_col.numeric_scale = col.numeric_scale;
    cfg_col.collation_id = col.collation_id;
    cfg_col.is_nullable = col.is_nullable;
    cfg_col.is_unsigned = col.is_unsigned;
    cfg_col.elements = col.elements;
    cfg_col.ind = static_cast<uint32_t>(cfg.columns.size());

    cfg_col.version_added = v_added;
    cfg_col.version_dropped = v_dropped;
    cfg_col.is_instant_dropped = col_dropped[i];
    cfg_col.phy_pos = phy_pos;
    if (col_has_phy[i]) {
      cfg.has_row_versions = true;
    }

    bool default_null = false;
    bool has_default = false;
    if (!decode_instant_default(kv, &cfg_col.instant_default_value,
                                &default_null, &has_default)) {
      fprintf(stderr, "Warning: failed to decode instant default for %s\n",
              col.name.c_str());
    }
    cfg_col.has_instant_default = has_default;
    cfg_col.instant_default_null = default_null;

    cfg.columns.push_back(std::move(cfg_col));
    opx_to_col_index[i] = static_cast<int>(cfg.columns.size()) - 1;
  }

  // MySQL 8.0.29+ includes DB_ROW_ID in n_cols even for tables with explicit PK.
  // The SDI doesn't include it, but we need to add it for .cfg compatibility.
  // Insert DB_ROW_ID before DB_TRX_ID if missing.
  {
    bool has_row_id = false;
    size_t trx_id_pos = cfg.columns.size();
    for (size_t i = 0; i < cfg.columns.size(); ++i) {
      if (cfg.columns[i].name == "DB_ROW_ID") {
        has_row_id = true;
      }
      if (cfg.columns[i].name == "DB_TRX_ID") {
        trx_id_pos = i;
      }
    }

    if (!has_row_id) {
      CfgColumn row_id_col;
      row_id_col.name = "DB_ROW_ID";
      row_id_col.dd_type = dd::enum_column_types::LONG;  // Placeholder type
      row_id_col.prtype = DATA_ROW_ID | DATA_NOT_NULL;  // 0 | 256 = 256
      row_id_col.mtype = DATA_SYS;
      row_id_col.len = DATA_ROW_ID_LEN;
      row_id_col.mbminmaxlen = 0;
      row_id_col.char_length = 0;
      row_id_col.numeric_scale = 0;
      row_id_col.collation_id = 0;
      row_id_col.is_nullable = false;
      row_id_col.is_unsigned = false;
      row_id_col.ind = static_cast<uint32_t>(trx_id_pos);
      row_id_col.version_added = UINT8_UNDEFINED;
      row_id_col.version_dropped = UINT8_UNDEFINED;
      row_id_col.is_instant_dropped = false;
      row_id_col.phy_pos = UINT32_UNDEFINED;
      row_id_col.has_instant_default = false;
      row_id_col.instant_default_null = false;

      cfg.columns.insert(cfg.columns.begin() + trx_id_pos, std::move(row_id_col));

      // Update indices for columns after DB_ROW_ID
      for (size_t i = trx_id_pos + 1; i < cfg.columns.size(); ++i) {
        cfg.columns[i].ind = static_cast<uint32_t>(i);
      }

      // Shift opx-to-column mapping for inserted DB_ROW_ID.
      for (auto& idx : opx_to_col_index) {
        if (idx >= 0 && static_cast<size_t>(idx) >= trx_id_pos) {
          idx += 1;
        }
      }
    }
  }

  uint32_t space_id_val = static_cast<uint32_t>(space_id);
  auto space_id_it = space_kv.find(dd_space_key_strings[DD_SPACE_ID]);
  if (space_id_it != space_kv.end()) {
    parse_uint32_value(space_id_it->second, &space_id_val);
  }

  // Compute column counters for instant metadata
  size_t n_dropped_cols = 0;
  size_t n_added_cols = 0;
  size_t n_added_and_dropped_cols = 0;
  size_t n_current_cols = 0;
  uint32_t current_row_version = 0;

  for (const auto& col : meta.table.columns) {
    if (col.is_virtual || is_system_column_name(col.name)) {
      continue;
    }

    const auto kv = parse_kv_string(col.se_private_data);
    const uint8_t v_added =
        parse_row_version(kv, DD_INSTANT_VERSION_ADDED);
    const uint8_t v_dropped =
        parse_row_version(kv, DD_INSTANT_VERSION_DROPPED);

    if (v_dropped != UINT8_UNDEFINED && v_dropped > 0) {
      n_dropped_cols++;
      if (v_added != UINT8_UNDEFINED && v_added > 0) {
        n_added_and_dropped_cols++;
      }
      current_row_version = std::max<uint32_t>(current_row_version, v_dropped);
      continue;
    }

    if (v_added != UINT8_UNDEFINED && v_added > 0) {
      n_added_cols++;
      current_row_version = std::max<uint32_t>(current_row_version, v_added);
    }

    n_current_cols++;
  }

  const size_t n_orig_dropped_cols = n_dropped_cols - n_added_and_dropped_cols;
  cfg.current_col_count = static_cast<uint32_t>(n_current_cols);
  cfg.initial_col_count =
      static_cast<uint32_t>((n_current_cols - n_added_cols) + n_orig_dropped_cols);
  cfg.total_col_count = static_cast<uint32_t>(n_current_cols + n_dropped_cols);
  cfg.n_instant_drop_cols = static_cast<uint32_t>(n_dropped_cols);
  cfg.current_row_version = current_row_version;

  if (cfg.current_row_version > 0) {
    uint32_t nullable_before_instant = 0;
    for (const auto& col : meta.table.columns) {
      if (col.is_virtual || is_system_column_name(col.name)) {
        continue;
      }
      const auto kv = parse_kv_string(col.se_private_data);
      const uint8_t v_added =
          parse_row_version(kv, DD_INSTANT_VERSION_ADDED);
      if (v_added == UINT8_UNDEFINED || v_added == 0) {
        if (col.is_nullable) {
          nullable_before_instant++;
        }
      }
    }
    cfg.n_instant_nullable = nullable_before_instant;
  }

  // Build indexes from SDI
  cfg.indexes.clear();

  // Optional SDI index first
  if (FSP_FLAGS_HAS_SDI(space_flags)) {
    CfgIndex sdi_index;
    sdi_index.name = "CLUST_IND_SDI";
    sdi_index.id = dict_sdi_get_index_id();
    sdi_index.space = space_id_val;
    sdi_index.page = static_cast<uint32_t>(sdi_root_page);
    sdi_index.type = DICT_CLUSTERED | DICT_UNIQUE | DICT_SDI;
    sdi_index.n_user_defined_cols = 2;
    sdi_index.n_uniq = 2;
    sdi_index.n_nullable = 0;
    sdi_index.trx_id_offset = 0;

    auto add_sdi_field = [&](const char* name, uint32_t fixed_len) {
      CfgIndexField field;
      field.name = name;
      field.prefix_len = 0;
      field.fixed_len = fixed_len;
      field.is_ascending = 1;
      sdi_index.fields.push_back(field);
    };

    add_sdi_field("type", 4);
    add_sdi_field("id", 8);
    add_sdi_field("DB_TRX_ID", DATA_TRX_ID_LEN);
    add_sdi_field("DB_ROLL_PTR", DATA_ROLL_PTR_LEN);
    add_sdi_field("compressed_len", 4);
    add_sdi_field("uncompressed_len", 4);
    add_sdi_field("data", 0);

    sdi_index.n_fields = static_cast<uint32_t>(sdi_index.fields.size());
    cfg.indexes.push_back(std::move(sdi_index));
  }

  for (const auto& idx : meta.table.indexes) {
    CfgIndex cfg_index;
    cfg_index.name = idx.name;

    bool is_unique = false;
    bool is_spatial = false;
    bool is_fulltext = false;
    switch (static_cast<dd::Index::enum_index_type>(idx.type)) {
      case dd::Index::IT_PRIMARY:
        cfg_index.type = DICT_CLUSTERED | DICT_UNIQUE;
        is_unique = true;
        break;
      case dd::Index::IT_UNIQUE:
        cfg_index.type = DICT_UNIQUE;
        is_unique = true;
        break;
      case dd::Index::IT_FULLTEXT:
        cfg_index.type = DICT_FTS;
        is_fulltext = true;
        break;
      case dd::Index::IT_SPATIAL:
        cfg_index.type = DICT_SPATIAL;
        is_spatial = true;
        break;
      case dd::Index::IT_MULTIPLE:
      default:
        cfg_index.type = 0;
        break;
    }

    const auto idx_kv = parse_kv_string(idx.se_private_data);
    auto id_it = idx_kv.find(dd_index_key_strings[DD_INDEX_ID]);
    auto space_it = idx_kv.find(dd_index_key_strings[DD_INDEX_SPACE_ID]);
    auto root_it = idx_kv.find(dd_index_key_strings[DD_INDEX_ROOT]);
    if (id_it != idx_kv.end()) {
      parse_uint64_value(id_it->second, &cfg_index.id);
    }
    if (space_it != idx_kv.end()) {
      parse_uint32_value(space_it->second, &cfg_index.space);
    } else {
      cfg_index.space = space_id_val;
    }
    if (root_it != idx_kv.end()) {
      parse_uint32_value(root_it->second, &cfg_index.page);
    }

    cfg_index.n_user_defined_cols = 0;
    cfg_index.n_nullable = 0;

    for (size_t i = 0; i < idx.elements.size(); ++i) {
      const auto& elem = idx.elements[i];
      if (elem.column_opx < 0 ||
          static_cast<size_t>(elem.column_opx) >= meta.table.columns.size()) {
        continue;
      }
      const auto& col = meta.table.columns[elem.column_opx];
      const ColumnTypeInfo& type_info = col_types[elem.column_opx];

      CfgIndexField field;
      field.name = col.name;
      field.prefix_len = calc_prefix_len(col, elem);
      field.is_ascending = (elem.order != dd::Index_element::ORDER_DESC);
      field.fixed_len =
          calc_fixed_len(type_info, cfg.is_comp, field.prefix_len, is_spatial,
                         i == 0);
      cfg_index.fields.push_back(field);

      if (!elem.hidden) {
        cfg_index.n_user_defined_cols++;
      }

      if (col.is_nullable && !col_dropped[elem.column_opx]) {
        cfg_index.n_nullable++;
      }
    }

    cfg_index.n_fields = static_cast<uint32_t>(cfg_index.fields.size());

    if (is_fulltext) {
      cfg_index.n_uniq = 0;
    } else if (is_unique) {
      cfg_index.n_uniq = cfg_index.n_user_defined_cols;
    } else {
      cfg_index.n_uniq = cfg_index.n_fields;
    }

    cfg.indexes.push_back(std::move(cfg_index));
  }

  // Set ord_part and max_prefix based on ordering columns
  std::unordered_map<std::string, size_t> name_to_col;
  for (size_t i = 0; i < cfg.columns.size(); ++i) {
    name_to_col[cfg.columns[i].name] = i;
  }

  for (const auto& index : cfg.indexes) {
    if (index.name == "CLUST_IND_SDI") {
      continue;
    }
    const uint32_t n_ord =
        std::min<uint32_t>(index.n_uniq, static_cast<uint32_t>(index.fields.size()));
    for (uint32_t i = 0; i < n_ord; ++i) {
      const auto& field = index.fields[i];
      const auto it = name_to_col.find(field.name);
      if (it == name_to_col.end()) {
        continue;
      }
      CfgColumn& col = cfg.columns[it->second];
      if (col.ord_part == 0) {
        col.max_prefix = field.prefix_len;
        col.ord_part = 1;
      } else if (field.prefix_len == 0) {
        col.max_prefix = 0;
      } else if (col.max_prefix != 0 && field.prefix_len > col.max_prefix) {
        col.max_prefix = field.prefix_len;
      }
    }
  }

  if (!cfg.has_row_versions) {
    const SdiIndexInfo* primary = nullptr;
    for (const auto& idx : meta.table.indexes) {
      if (idx.type == dd::Index::IT_PRIMARY || idx.name == "PRIMARY") {
        primary = &idx;
        break;
      }
    }

    std::vector<bool> assigned(cfg.columns.size(), false);
    uint32_t pos = 0;
    if (primary != nullptr) {
      for (const auto& elem : primary->elements) {
        if (elem.column_opx < 0 ||
            static_cast<size_t>(elem.column_opx) >= opx_to_col_index.size()) {
          continue;
        }
        int idx = opx_to_col_index[elem.column_opx];
        if (idx < 0 || static_cast<size_t>(idx) >= cfg.columns.size()) {
          continue;
        }
        if (!assigned[idx]) {
          cfg.columns[idx].phy_pos = pos++;
          assigned[idx] = true;
        }
      }
    }

    for (size_t i = 0; i < cfg.columns.size(); ++i) {
      if (!assigned[i]) {
        cfg.columns[i].phy_pos = pos++;
      }
    }
  }

  *out = std::move(cfg);
  return true;
}

static bool write_cfg_file(const char* path, const CfgTable& cfg) {
  FILE* file = fopen(path, "w+b");
  if (file == nullptr) {
    fprintf(stderr, "Error: cannot open cfg output: %s\n", path);
    return false;
  }

  auto write_bytes = [&](const void* buf, size_t len) -> bool {
    return fwrite(buf, 1, len, file) == len;
  };
  auto write_u32 = [&](uint32_t val) -> bool {
    byte buf[4];
    mach_write_to_4(buf, val);
    return write_bytes(buf, sizeof(buf));
  };
  auto write_u64 = [&](uint64_t val) -> bool {
    byte buf[8];
    mach_write_to_8(buf, val);
    return write_bytes(buf, sizeof(buf));
  };

  if (!write_u32(IB_EXPORT_CFG_VERSION_V7)) {
    fclose(file);
    return false;
  }

  char hostbuf[256];
  std::string hostname = "percona-parser";
  if (gethostname(hostbuf, sizeof(hostbuf)) == 0) {
    hostbuf[sizeof(hostbuf) - 1] = '\0';
    if (hostbuf[0] != '\0') {
      hostname = hostbuf;
    }
  }
  const uint32_t host_len = static_cast<uint32_t>(hostname.size() + 1);
  if (!write_u32(host_len) || !write_bytes(hostname.c_str(), host_len)) {
    fclose(file);
    return false;
  }

  const uint32_t table_len = static_cast<uint32_t>(cfg.name.size() + 1);
  if (!write_u32(table_len) || !write_bytes(cfg.name.c_str(), table_len)) {
    fclose(file);
    return false;
  }

  if (!write_u64(cfg.autoinc)) {
    fclose(file);
    return false;
  }

  if (!write_u32(univ_page_size.logical()) ||
      !write_u32(cfg.table_flags) ||
      !write_u32(static_cast<uint32_t>(cfg.columns.size()))) {
    fclose(file);
    return false;
  }

  if (!write_u32(cfg.n_instant_nullable)) {
    fclose(file);
    return false;
  }

  if (!write_u32(cfg.initial_col_count) || !write_u32(cfg.current_col_count) ||
      !write_u32(cfg.total_col_count) ||
      !write_u32(cfg.n_instant_drop_cols) ||
      !write_u32(cfg.current_row_version)) {
    fclose(file);
    return false;
  }

  if (!write_u32(cfg.space_flags)) {
    fclose(file);
    return false;
  }

  if (!write_bytes(&cfg.compression_type, sizeof(cfg.compression_type))) {
    fclose(file);
    return false;
  }

  for (const auto& col : cfg.columns) {
    if (!write_u32(col.prtype) || !write_u32(col.mtype) ||
        !write_u32(col.len) || !write_u32(col.mbminmaxlen) ||
        !write_u32(col.ind) || !write_u32(col.ord_part) ||
        !write_u32(col.max_prefix)) {
      fclose(file);
      return false;
    }

    const uint32_t name_len = static_cast<uint32_t>(col.name.size() + 1);
    if (!write_u32(name_len) || !write_bytes(col.name.c_str(), name_len)) {
      fclose(file);
      return false;
    }

    byte meta_buf[2 + sizeof(uint32_t)];
    meta_buf[0] = col.version_added;
    meta_buf[1] = col.version_dropped;
    mach_write_to_4(meta_buf + 2, col.phy_pos);
    if (!write_bytes(meta_buf, sizeof(meta_buf))) {
      fclose(file);
      return false;
    }

    if (col.is_instant_dropped) {
      byte dropped_buf[22];
      byte* ptr = dropped_buf;
      mach_write_to_1(ptr, col.is_nullable);
      ptr += 1;
      mach_write_to_1(ptr, col.is_unsigned);
      ptr += 1;
      mach_write_to_4(ptr, col.char_length);
      ptr += 4;
      mach_write_to_4(ptr, static_cast<uint32_t>(col.dd_type));
      ptr += 4;
      mach_write_to_4(ptr, col.numeric_scale);
      ptr += 4;
      mach_write_to_8(ptr, col.collation_id);
      if (!write_bytes(dropped_buf, sizeof(dropped_buf))) {
        fclose(file);
        return false;
      }

      if (col.dd_type == dd::enum_column_types::ENUM ||
          col.dd_type == dd::enum_column_types::SET) {
        const uint32_t elem_count =
            static_cast<uint32_t>(col.elements.size());
        if (!write_u32(elem_count)) {
          fclose(file);
          return false;
        }
        for (const auto& elem : col.elements) {
          const uint32_t elem_len =
              static_cast<uint32_t>(elem.size() + 1);
          if (!write_u32(elem_len) ||
              !write_bytes(elem.c_str(), elem_len)) {
            fclose(file);
            return false;
          }
        }
      }
    }

    if (col.has_instant_default) {
      byte flag = 1;
      if (!write_bytes(&flag, 1)) {
        fclose(file);
        return false;
      }
      byte null_flag = col.instant_default_null ? 1 : 0;
      if (!write_bytes(&null_flag, 1)) {
        fclose(file);
        return false;
      }
      if (!col.instant_default_null) {
        const uint32_t len =
            static_cast<uint32_t>(col.instant_default_value.size());
        if (!write_u32(len) ||
            (len > 0 &&
             !write_bytes(col.instant_default_value.data(), len))) {
          fclose(file);
          return false;
        }
      }
    } else {
      byte flag = 0;
      if (!write_bytes(&flag, 1)) {
        fclose(file);
        return false;
      }
    }
  }

  if (!write_u32(static_cast<uint32_t>(cfg.indexes.size()))) {
    fclose(file);
    return false;
  }

  for (const auto& index : cfg.indexes) {
    if (!write_u64(index.id) || !write_u32(index.space) ||
        !write_u32(index.page) || !write_u32(index.type) ||
        !write_u32(index.trx_id_offset) ||
        !write_u32(index.n_user_defined_cols) ||
        !write_u32(index.n_uniq) || !write_u32(index.n_nullable) ||
        !write_u32(index.n_fields)) {
      fclose(file);
      return false;
    }

    const uint32_t idx_len = static_cast<uint32_t>(index.name.size() + 1);
    if (!write_u32(idx_len) || !write_bytes(index.name.c_str(), idx_len)) {
      fclose(file);
      return false;
    }

    for (const auto& field : index.fields) {
      if (!write_u32(field.prefix_len) || !write_u32(field.fixed_len) ||
          !write_u32(field.is_ascending)) {
        fclose(file);
        return false;
      }

      const uint32_t field_len =
          static_cast<uint32_t>(field.name.size() + 1);
      if (!write_u32(field_len) ||
          !write_bytes(field.name.c_str(), field_len)) {
        fclose(file);
        return false;
      }
    }
  }

  if (fflush(file) != 0) {
    fclose(file);
    return false;
  }

  fclose(file);
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

static size_t sdi_blob_payload_size(size_t page_size) {
  if (page_size <= FIL_PAGE_DATA + kSdiLobHdrSize + FIL_PAGE_END_LSN_OLD_CHKSUM) {
    return 0;
  }
  return page_size - FIL_PAGE_DATA - kSdiLobHdrSize -
         FIL_PAGE_END_LSN_OLD_CHKSUM;
}

static bool emit_sdi_blob_chain(SdiBlobAlloc* alloc,
                                const std::vector<byte>& comp,
                                page_no_t* first_page_out) {
  if (alloc == nullptr || alloc->pages == nullptr || alloc->out_pages == nullptr) {
    fprintf(stderr, "Error: SDI blob allocator not configured.\n");
    return false;
  }

  const size_t payload_size = sdi_blob_payload_size(alloc->page_size);
  if (payload_size == 0) {
    fprintf(stderr, "Error: invalid SDI blob page size %zu.\n", alloc->page_size);
    return false;
  }

  if (comp.empty()) {
    fprintf(stderr, "Error: SDI compressed payload is empty.\n");
    return false;
  }

  size_t remaining = comp.size();
  size_t offset = 0;
  page_no_t first_page = FIL_NULL;

  while (remaining > 0) {
    if (alloc->next >= alloc->pages->size()) {
      fprintf(stderr,
              "Error: not enough SDI blob pages (need %zu bytes).\n",
              comp.size());
      return false;
    }

    page_no_t page_no = (*alloc->pages)[alloc->next++];
    if (first_page == FIL_NULL) {
      first_page = page_no;
    }

    std::vector<byte> page(alloc->page_size);
    memset(page.data(), 0, page.size());
    mach_write_to_4(page.data() + FIL_PAGE_OFFSET, page_no);
    mach_write_to_4(page.data() + FIL_PAGE_PREV, FIL_NULL);
    mach_write_to_4(page.data() + FIL_PAGE_NEXT, FIL_NULL);
    mach_write_to_2(page.data() + FIL_PAGE_TYPE, FIL_PAGE_SDI_BLOB);
    mach_write_to_4(page.data() + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID,
                    alloc->space_id);

    const size_t part_len = std::min(payload_size, remaining);
    const page_no_t next_page =
        (remaining > part_len && alloc->next < alloc->pages->size())
            ? (*alloc->pages)[alloc->next]
            : FIL_NULL;

    byte* data = page.data() + FIL_PAGE_DATA;
    mach_write_to_4(data + kSdiLobHdrPartLen,
                    static_cast<uint32_t>(part_len));
    mach_write_to_4(data + kSdiLobHdrNextPageNo, next_page);
    memcpy(data + kSdiLobHdrSize, comp.data() + offset, part_len);

    stamp_page_lsn_and_crc32(page.data(), alloc->page_size, 0);
    (*alloc->out_pages)[page_no] = std::move(page);

    remaining -= part_len;
    offset += part_len;
  }

  if (first_page_out != nullptr) {
    *first_page_out = first_page;
  }
  return true;
}

static bool collect_sdi_blob_pages(File in_fd, const page_size_t& page_sz,
                                   uint64_t num_pages,
                                   std::vector<page_no_t>* pages) {
  if (pages == nullptr) {
    return false;
  }
  pages->clear();

  const size_t physical_size = page_sz.physical();
  std::unique_ptr<unsigned char[]> buf(new unsigned char[physical_size]);

  for (uint64_t page_no = 0; page_no < num_pages; ++page_no) {
    if (!seek_page(in_fd, page_sz, static_cast<page_no_t>(page_no))) {
      return false;
    }

    size_t r = my_read(in_fd, buf.get(), physical_size, MYF(0));
    if (r != physical_size) {
      fprintf(stderr, "Failed to read page %llu during SDI scan.\n",
              (unsigned long long)page_no);
      return false;
    }

    const uint16_t page_type = mach_read_from_2(buf.get() + FIL_PAGE_TYPE);
    if (page_type == FIL_PAGE_SDI_BLOB || page_type == FIL_PAGE_SDI_ZBLOB) {
      pages->push_back(static_cast<page_no_t>(page_no));
    }
  }

  return true;
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
                                   const std::vector<SdiEntry>& entries,
                                   SdiBlobAlloc* blob_alloc) {
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

    bool use_external = false;
    size_t len_bytes = 0;
    size_t rec_data_len = 0;
    size_t rec_size = 0;
    page_no_t first_blob_page = FIL_NULL;

    if (comp_len > 0x3fff) {
      use_external = true;
    } else {
      len_bytes = (comp_len <= 127) ? 1 : 2;
      rec_data_len = kSdiRecOffVar + comp_len;
      rec_size = kSdiRecHeaderSize + len_bytes + rec_data_len;
      if (heap_top + rec_size > dir_start) {
        use_external = true;
      }
    }

    if (use_external) {
      if (blob_alloc == nullptr) {
        fprintf(stderr,
                "Error: SDI record requires external storage but no SDI "
                "blob pages are available.\n");
        return false;
      }

      const size_t local_prefix = 0;
      len_bytes = 2;
      rec_data_len = kSdiRecOffVar + local_prefix + kSdiExternRefSize;
      rec_size = kSdiRecHeaderSize + len_bytes + rec_data_len;

      if (heap_top + rec_size > dir_start) {
        fprintf(stderr,
                "Error: SDI external records exceed SDI root page capacity\n");
        return false;
      }

      if (!emit_sdi_blob_chain(blob_alloc, comp, &first_blob_page)) {
        return false;
      }
      if (first_blob_page == FIL_NULL) {
        fprintf(stderr, "Error: SDI external chain did not allocate a page.\n");
        return false;
      }
    }

    if (heap_top + rec_size > dir_start) {
      fprintf(stderr, "Error: SDI records exceed SDI root page capacity\n");
      return false;
    }

    byte* rec_base = page + heap_top;
    byte* rec = rec_base + len_bytes + kSdiRecHeaderSize;
    memset(rec_base, 0, rec_size);

    if (use_external) {
      rec_base[0] = 0;
      rec_base[1] = static_cast<byte>(0xC0);
    } else if (len_bytes == 1) {
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
    if (use_external) {
      byte* ref = rec + kSdiRecOffVar;
      memset(ref, 0, kSdiExternRefSize);
      mach_write_to_4(ref + kSdiExternSpaceId, blob_alloc->space_id);
      mach_write_to_4(ref + kSdiExternPageNo, first_blob_page);
      mach_write_to_4(ref + kSdiExternOffset, FIL_PAGE_DATA);
      mach_write_to_8(ref + kSdiExternLen, static_cast<uint64_t>(comp_len));
    } else {
      memcpy(rec + kSdiRecOffVar, comp.data(), comp_len);
    }

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
        case FIL_PAGE_SDI_BLOB: return "FIL_PAGE_SDI_BLOB";
        case FIL_PAGE_SDI_ZBLOB: return "FIL_PAGE_SDI_ZBLOB";
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
bool rebuild_uncompressed_ibd(File in_fd, File out_fd,
                              const char* source_sdi_json_path,
                              const char* target_sdi_json_path,
                              const char* index_id_map_path,
                              const char* cfg_out_path,
                              bool use_target_sdi_root,
                              bool use_source_sdi_root,
                              bool target_sdi_root_override_set,
                              uint32_t target_sdi_root_override,
                              const char* target_ibd_path)
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

  const char* output_sdi_json_path =
      (target_sdi_json_path != nullptr) ? target_sdi_json_path
                                        : source_sdi_json_path;
  const bool have_output_sdi_json = (output_sdi_json_path != nullptr);
  const bool have_source_sdi_json = (source_sdi_json_path != nullptr);
  const bool have_target_sdi_json = (target_sdi_json_path != nullptr);

  std::vector<SdiEntry> sdi_entries;
  std::vector<page_no_t> sdi_blob_pages;
  std::unordered_map<page_no_t, std::vector<byte>> sdi_blob_output;
  std::unordered_map<uint64_t, uint64_t> index_id_remap;
  bool want_cfg = (cfg_out_path != nullptr);
  page_no_t sdi_root_page = FIL_NULL;
  page_no_t source_sdi_root_page = FIL_NULL;
  page_no_t target_sdi_root_page = FIL_NULL;
  bool target_sdi_root_set = false;
  uint32_t target_sdi_root_version = 0;
  bool sdi_root_set = false;
  uint32_t space_flags = 0;
  bool space_flags_set = false;
  SdiMetadata sdi_meta;
  SdiMetadata source_meta;
  SdiMetadata target_meta;
  bool have_source_meta = false;
  bool have_target_meta = false;

  if (have_source_sdi_json) {
    if (!load_sdi_metadata(source_sdi_json_path, &source_meta)) {
      return false;
    }
    have_source_meta = true;
  }

  if (have_target_sdi_json) {
    if (!load_sdi_metadata(target_sdi_json_path, &target_meta)) {
      return false;
    }
    have_target_meta = true;
  }

  if (have_output_sdi_json) {
    if (!load_sdi_json_entries(output_sdi_json_path, sdi_entries)) {
      return false;
    }
    if (!collect_sdi_blob_pages(in_fd, pg_sz, num_pages, &sdi_blob_pages)) {
      return false;
    }
  }

  if (have_target_meta) {
    sdi_meta = target_meta;
  } else if (have_source_meta) {
    sdi_meta = source_meta;
  }

  if (have_source_meta && have_target_meta) {
    std::string err;
    if (!build_index_id_remap_from_sdi(source_meta, target_meta,
                                       &index_id_remap, &err)) {
      fprintf(stderr, "Error: failed to build index-id remap: %s\n",
              err.c_str());
      return false;
    }
  }

  if (index_id_map_path != nullptr) {
    std::string err;
    std::unordered_map<uint64_t, uint64_t> file_map;
    if (!load_index_id_map_file(index_id_map_path, &file_map, &err)) {
      fprintf(stderr, "Error: failed to load index-id map: %s\n",
              err.c_str());
      return false;
    }
    for (const auto& entry : file_map) {
      auto it = index_id_remap.find(entry.first);
      if (it != index_id_remap.end() && it->second != entry.second) {
        fprintf(stderr,
                "Warning: index-id map override for %llu (%llu -> %llu)\n",
                static_cast<unsigned long long>(entry.first),
                static_cast<unsigned long long>(it->second),
                static_cast<unsigned long long>(entry.second));
      }
      index_id_remap[entry.first] = entry.second;
    }
  }

  if (target_sdi_root_override_set) {
    target_sdi_root_page = static_cast<page_no_t>(target_sdi_root_override);
    target_sdi_root_set = true;
  } else if (target_ibd_path != nullptr) {
    std::string err;
    if (read_sdi_root_from_tablespace(target_ibd_path, &target_sdi_root_page,
                                      &target_sdi_root_version, &err)) {
      target_sdi_root_set = true;
      fprintf(stderr,
              "Target SDI header: version=%u root_page=%u (file=%s)\n",
              target_sdi_root_version,
              static_cast<unsigned int>(target_sdi_root_page),
              target_ibd_path);
    } else {
      fprintf(stderr,
              "Warning: unable to read target SDI root from %s: %s\n",
              target_ibd_path, err.c_str());
    }
  } else if (have_target_meta && !target_meta.tablespace.files.empty()) {
    const std::string& raw_path = target_meta.tablespace.files.front();
    std::string resolved;
    if (resolve_tablespace_path(raw_path, &resolved)) {
      std::string err;
      if (read_sdi_root_from_tablespace(resolved, &target_sdi_root_page,
                                        &target_sdi_root_version, &err)) {
        target_sdi_root_set = true;
        fprintf(stderr,
                "Target SDI header: version=%u root_page=%u (file=%s)\n",
                target_sdi_root_version,
                static_cast<unsigned int>(target_sdi_root_page),
                resolved.c_str());
      } else {
        fprintf(stderr,
                "Warning: unable to read target SDI root from %s: %s\n",
                resolved.c_str(), err.c_str());
      }
    } else {
      fprintf(stderr,
              "Warning: target SDI root lookup skipped (cannot resolve '%s').\n"
              "         Set MYSQL_DATADIR, use --target-ibd, or pass --target-sdi-root.\n",
              raw_path.c_str());
    }
  }

  if (use_target_sdi_root && !target_sdi_root_set) {
    fprintf(stderr,
            "Error: --use-target-sdi-root requires target SDI root data.\n");
    return false;
  }

  if (want_cfg && !have_output_sdi_json) {
    fprintf(stderr, "Error: --cfg-out requires SDI JSON metadata.\n");
    return false;
  }

  if (!index_id_remap.empty()) {
    fprintf(stderr, "Index-id remap entries: %zu\n", index_id_remap.size());
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
      if (have_output_sdi_json) {
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
        source_sdi_root_page = mach_read_from_4(in_buf.get() + sdi_offset + 4);
        sdi_root_page = source_sdi_root_page;
        if (target_sdi_root_set &&
            (target_sdi_root_page == 0 || target_sdi_root_page == FIL_NULL)) {
          fprintf(stderr,
                  "Warning: target SDI root page is invalid (%u); ignoring.\n",
                  static_cast<unsigned int>(target_sdi_root_page));
          target_sdi_root_set = false;
        }
        if (target_sdi_root_set &&
            target_sdi_root_page != source_sdi_root_page) {
          fprintf(stderr,
                  "Warning: SDI root mismatch (source=%u target=%u).\n",
                  static_cast<unsigned int>(source_sdi_root_page),
                  static_cast<unsigned int>(target_sdi_root_page));
          if (use_target_sdi_root) {
            sdi_root_page = target_sdi_root_page;
            fprintf(stderr,
                    "         Using target SDI root page as requested.\n");
          } else {
            fprintf(stderr,
                    "         Using source SDI root page (default).\n");
          }
        } else if (use_target_sdi_root && target_sdi_root_set) {
          sdi_root_page = target_sdi_root_page;
        }
        if (use_source_sdi_root) {
          sdi_root_page = source_sdi_root_page;
        }
        sdi_root_set = (sdi_root_page != 0 && sdi_root_page != FIL_NULL);
        fprintf(stderr,
                "SDI header: version=%u root_page=%u (json=%s)\n",
                sdi_version, sdi_root_page,
                output_sdi_json_path ? output_sdi_json_path : "(none)");
      }

      if (!update_tablespace_header_for_uncompressed(out_buf.get(),
                                                     logical_size,
                                                     &space_id)) {
        return false;
      }
      space_flags = fsp_header_get_flags(out_buf.get());
      space_flags_set = true;

      if (have_output_sdi_json) {
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

    if (have_output_sdi_json && sdi_root_set && page_no == sdi_root_page) {
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

      SdiBlobAlloc blob_alloc;
      SdiBlobAlloc* blob_alloc_ptr = nullptr;
      if (!sdi_blob_pages.empty()) {
        blob_alloc.pages = &sdi_blob_pages;
        blob_alloc.next = 0;
        blob_alloc.page_size = logical_size;
        blob_alloc.space_id = space_id;
        blob_alloc.out_pages = &sdi_blob_output;
        blob_alloc_ptr = &blob_alloc;
      }

      if (!populate_sdi_root_page(out_buf.get(), logical_size, sdi_entries,
                                  blob_alloc_ptr)) {
        fprintf(stderr, "Error: SDI root page rebuild failed.\n");
        return false;
      }
    }

    if (!index_id_remap.empty()) {
      const uint16_t page_type =
          mach_read_from_2(out_buf.get() + FIL_PAGE_TYPE);
      if (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_RTREE) {
        const uint64_t old_id =
            mach_read_from_8(out_buf.get() + PAGE_HEADER + PAGE_INDEX_ID);
        auto it = index_id_remap.find(old_id);
        if (it != index_id_remap.end()) {
          mach_write_to_8(out_buf.get() + PAGE_HEADER + PAGE_INDEX_ID,
                          it->second);
        }
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

  if (!sdi_blob_output.empty()) {
    for (const auto& entry : sdi_blob_output) {
      const page_no_t page_no = entry.first;
      const std::vector<byte>& page = entry.second;
      if (page.size() != logical_size) {
        fprintf(stderr,
                "Error: SDI blob page %u size mismatch (%zu != %zu).\n",
                page_no, page.size(), logical_size);
        return false;
      }

      const my_off_t offset =
          static_cast<my_off_t>(page_no) * static_cast<my_off_t>(logical_size);
      if (my_seek(out_fd, offset, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
        fprintf(stderr,
                "Error: my_seek failed for SDI blob page %u. Errno=%d (%s)\n",
                page_no, errno, strerror(errno));
        return false;
      }
      size_t w = my_write(out_fd, (uchar*)page.data(), logical_size, MYF(0));
      if (w != logical_size) {
        fprintf(stderr, "Failed to write SDI blob page %u.\n", page_no);
        return false;
      }
    }
  }

  fprintf(stderr, "\n========================================\n");
  fprintf(stderr, "REBUILD COMPLETE (EXPERIMENTAL)\n");
  fprintf(stderr, "========================================\n");
  fprintf(stderr, "Output pages written: %llu\n", (unsigned long long)num_pages);
  fprintf(stderr, "========================================\n\n");

  if (want_cfg) {
    if (!space_flags_set) {
      fprintf(stderr, "Error: space flags not captured for cfg output.\n");
      return false;
    }
    if (FSP_FLAGS_HAS_SDI(space_flags) && !sdi_root_set) {
      fprintf(stderr, "Error: SDI root page not set for cfg output.\n");
      return false;
    }

    CfgTable cfg_table;
    if (!build_cfg_table_from_sdi(sdi_meta, space_flags,
                                  sdi_root_set ? sdi_root_page : FIL_NULL,
                                  space_id, &cfg_table)) {
      fprintf(stderr, "Error: failed to build cfg metadata.\n");
      return false;
    }
    if (!write_cfg_file(cfg_out_path, cfg_table)) {
      fprintf(stderr, "Error: failed to write cfg file.\n");
      return false;
    }
    fprintf(stderr, "CFG written to: %s\n", cfg_out_path);
  }

  return true;
}
