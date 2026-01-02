// Standard C++ includes
#include <iostream>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unordered_map>
// Keep RapidJSON SizeType consistent across translation units.
#define RAPIDJSON_NO_SIZETYPEDEFINE
namespace rapidjson { typedef ::std::size_t SizeType; }
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/filereadstream.h>
#include <fstream>
#include <iostream>

#include "parser.h"

// InnoDB includes
#include "page0page.h"  // For page structure constants
#include "rem0rec.h"    // For record handling
#include "mach0data.h"  // For mach_read_from_2
#include "ut0byte.h"    // For byte utilities
#include "page0page.h"  // FIL_PAGE_INDEX, PAGE_HEADER, etc.
#include "fil0fil.h"    // fil_page_get_type()
#include "rem0rec.h"    // btr_root_fseg_validate() might be declared
#include "mach0data.h"  // mach_read_from_8(), mach_read_from_4()
#include "fsp0fsp.h"    // FSP_SPACE_ID
#include "fsp0types.h"  // btr_root_fseg_validate() signature
#include "my_time.h"
#include "decimal.h"
#include "my_sys.h"
#include "m_ctype.h"

#include "tables_dict.h"
#include "undrop_for_innodb.h"
#include "decompress.h"

struct XdesCache {
  page_no_t page_no = FIL_NULL;
  std::unique_ptr<unsigned char[]> buf;
  size_t buf_size = 0;

  void update(page_no_t new_page_no, const unsigned char* page, size_t size) {
    if (!buf || buf_size != size) {
      buf.reset(new unsigned char[size]);
      buf_size = size;
    }
    std::memcpy(buf.get(), page, size);
    page_no = new_page_no;
  }

  bool is_free(page_no_t target, const page_size_t& page_sz) const {
    if (!buf || page_no == FIL_NULL) {
      return false;
    }

    if (xdes_calc_descriptor_page(page_sz, target) != page_no) {
      return false;
    }

    const auto* descr = reinterpret_cast<const xdes_t*>(
        buf.get() + XDES_ARR_OFFSET +
        XDES_SIZE * xdes_calc_descriptor_index(page_sz, target));
    const page_no_t pos = target % FSP_EXTENT_SIZE;
    return xdes_get_bit(descr, XDES_FREE_BIT, pos);
  }
};

/** A minimal column-definition struct */
struct MyColumnDef {
    std::string name;            // e.g., "id", "name", ...
    std::string type_utf8;       // e.g., "int", "char", "varchar"
    uint32_t    char_length = 0;
    uint32_t    collation_id = 0;
    bool        is_nullable = false;
    bool        is_unsigned = false;
    bool        is_virtual = false;
    int         hidden = 0;
    int         ordinal_position = 0;
    int         column_opx = -1;
    int         numeric_precision = 0;
    int         numeric_scale = 0;
    int         datetime_precision = 0;
    size_t      elements_count = 0;
    std::vector<std::string> elements;
    bool        elements_complete = false;
};

struct IndexElementDef {
    int column_opx = -1;
    uint32_t length = 0xFFFFFFFFu;
    int ordinal_position = 0;
    bool hidden = false;
};

struct IndexDef {
    std::string name;
    uint64_t id = 0;
    page_no_t root = FIL_NULL;
    std::vector<IndexElementDef> elements;
    bool is_primary = false;
};

/** We store the columns here, loaded from JSON. */
static std::vector<MyColumnDef> g_columns;
static std::vector<MyColumnDef> g_columns_by_opx;
static std::vector<IndexDef> g_index_defs;

static uint64_t read_be_uint(const unsigned char* ptr, size_t len);
static int64_t read_be_int_signed(const unsigned char* ptr, size_t len);
static std::string to_lower_copy(const std::string& in);
static std::unordered_map<std::string, std::string> parse_kv_string(
    const std::string& input);
static bool parse_uint64_value(const std::string& s, uint64_t* out);
static bool parse_uint32_value(const std::string& s, uint32_t* out);

parser_context_t::parser_context_t()
    : target_index_id(0),
      target_index_set(false),
      target_index_name("PRIMARY"),
      target_index_root(FIL_NULL) {}

/**
 * btr_root_fseg_validate():
 *   find the first valid root page
 */
static bool btr_root_fseg_validate(const unsigned char* page, uint32_t space_id) {
    // Simplified check - in real InnoDB this does more validation
    // For now just check if the space_id matches and basic offset is valid
    uint32_t page_space_id = mach_read_from_4(page);
    return (page_space_id == space_id) && (mach_read_from_4(page + 4) != 0);
}

/**
 * read_uint64_from_page():
 *   Utility to read a 64-bit big-endian integer from 'ptr'.
 */
static inline uint64_t read_uint64_from_page(const unsigned char* ptr) {
  return mach_read_from_8(ptr);
}

static int base64_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static bool decode_base64(const std::string& input, std::string& output) {
  if (input.empty() || (input.size() % 4) != 0) {
    return false;
  }
  output.clear();
  output.reserve(input.size() / 4 * 3);
  for (size_t i = 0; i < input.size(); i += 4) {
    int v0 = base64_value(static_cast<unsigned char>(input[i]));
    int v1 = base64_value(static_cast<unsigned char>(input[i + 1]));
    if (v0 < 0 || v1 < 0) {
      return false;
    }

    char c2 = input[i + 2];
    char c3 = input[i + 3];
    int v2 = (c2 == '=') ? -2 : base64_value(static_cast<unsigned char>(c2));
    int v3 = (c3 == '=') ? -2 : base64_value(static_cast<unsigned char>(c3));
    if ((v2 < 0 && v2 != -2) || (v3 < 0 && v3 != -2)) {
      return false;
    }

    unsigned char b0 = static_cast<unsigned char>((v0 << 2) | (v1 >> 4));
    output.push_back(static_cast<char>(b0));

    if (v2 == -2) {
      if (v3 != -2) {
        return false;
      }
      break;
    }
    unsigned char b1 = static_cast<unsigned char>(((v1 & 0x0F) << 4) | (v2 >> 2));
    output.push_back(static_cast<char>(b1));

    if (v3 == -2) {
      break;
    }
    unsigned char b2 = static_cast<unsigned char>(((v2 & 0x03) << 6) | v3);
    output.push_back(static_cast<char>(b2));
  }
  return true;
}

static std::string decode_sdi_string(const std::string& input) {
  std::string decoded;
  if (decode_base64(input, decoded)) {
    return decoded;
  }
  return input;
}

bool parser_debug_enabled() {
  static int cached = -1;
  if (cached != -1) {
    return cached == 1;
  }
  const char* env = std::getenv("IB_PARSER_DEBUG");
  cached = (env && *env && std::strcmp(env, "0") != 0) ? 1 : 0;
  return cached == 1;
}

static void init_parser_timezone() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  const char* tz = std::getenv("IB_PARSER_TZ");
  if (!tz || *tz == '\0') {
    tz = "America/Sao_Paulo";
  }
  setenv("TZ", tz, 1);
  tzset();
  initialized = true;
}

static unsigned int max_decimals_from_len(ulint len, ulint base_len) {
  if (len <= base_len) {
    return 0;
  }
  ulint frac_bytes = len - base_len;
  unsigned int max_dec = static_cast<unsigned int>(frac_bytes * 2);
  if (max_dec > 6) {
    max_dec = 6;
  }
  return max_dec;
}

bool format_innodb_datetime(const unsigned char* ptr, ulint len,
                            unsigned int dec, std::string& out) {
  if (!ptr || len < 5) {
    return false;
  }
  if (dec > 6) {
    dec = 6;
  }
  unsigned int max_dec = max_decimals_from_len(len, 5);
  if (dec > max_dec) {
    dec = max_dec;
  }
  longlong packed = my_datetime_packed_from_binary(ptr, dec);
  MYSQL_TIME tm{};
  TIME_from_longlong_datetime_packed(&tm, packed);
  char buf[MAX_DATE_STRING_REP_LENGTH];
  int n = my_datetime_to_str(tm, buf, dec);
  out.assign(buf, static_cast<size_t>(n));
  return true;
}

static int pow10_int(int exp) {
  int val = 1;
  for (int i = 0; i < exp; i++) {
    val *= 10;
  }
  return val;
}

bool format_innodb_timestamp(const unsigned char* ptr, ulint len,
                             unsigned int dec, std::string& out) {
  if (!ptr || len < 4) {
    return false;
  }
  if (dec > 6) {
    dec = 6;
  }
  unsigned int max_dec = max_decimals_from_len(len, 4);
  if (dec > max_dec) {
    dec = max_dec;
  }

  init_parser_timezone();
  my_timeval tv{};
  my_timestamp_from_binary(&tv, ptr, dec);
  time_t secs = static_cast<time_t>(tv.m_tv_sec);
  struct tm local_tm;
  if (!localtime_r(&secs, &local_tm)) {
    return false;
  }

  char buf[64];
  int n = std::snprintf(buf, sizeof(buf),
                        "%04d-%02d-%02d %02d:%02d:%02d",
                        local_tm.tm_year + 1900,
                        local_tm.tm_mon + 1,
                        local_tm.tm_mday,
                        local_tm.tm_hour,
                        local_tm.tm_min,
                        local_tm.tm_sec);
  if (dec > 0) {
    int scale = pow10_int(6 - static_cast<int>(dec));
    int frac = static_cast<int>(tv.m_tv_usec / scale);
    n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n),
                       ".%0*d", dec, frac);
  }
  out.assign(buf, static_cast<size_t>(n));
  return true;
}

// Helper to read signed big-endian integer (for DATE/TIME)
// InnoDB stores signed integers with the sign bit flipped for B-tree ordering
static int64_t read_be_int_signed(const unsigned char* ptr, size_t len) {
  if (len == 0 || len > 8) {
    return 0;
  }
  uint64_t val = 0;
  for (size_t i = 0; i < len; i++) {
    val = (val << 8) | ptr[i];
  }
  uint64_t sign_mask = 1ULL << (len * 8 - 1);
  val ^= sign_mask;
  if ((val & sign_mask) && len < 8) {
    uint64_t mask = ~0ULL << (len * 8);
    val |= mask;
  }
  return static_cast<int64_t>(val);
}

bool format_innodb_date(const unsigned char* ptr, ulint len, std::string& out) {
  if (!ptr || len < 3) {
    return false;
  }
  // Decode with sign-bit flip, then unpack as YYYYMMDD
  uint32_t raw = static_cast<uint32_t>(read_be_int_signed(ptr, len));
  unsigned int day = raw & 31;
  unsigned int month = (raw >> 5) & 15;
  unsigned int year = raw >> 9;
  char buf[16];
  int n = std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u", year, month, day);
  out.assign(buf, static_cast<size_t>(n));
  return true;
}

bool format_innodb_time(const unsigned char* ptr, ulint len,
                        unsigned int dec, std::string& out) {
  if (!ptr || len < 3) {
    return false;
  }
  if (dec > 6) {
    dec = 6;
  }
  unsigned int max_dec = max_decimals_from_len(len, 3);
  if (dec > max_dec) {
    dec = max_dec;
  }
  longlong packed = my_time_packed_from_binary(ptr, dec);
  MYSQL_TIME tm{};
  TIME_from_longlong_time_packed(&tm, packed);
  char buf[MAX_DATE_STRING_REP_LENGTH];
  int n = my_time_to_str(tm, buf, dec);
  out.assign(buf, static_cast<size_t>(n));
  return true;
}

static void set_target_index_id(parser_context_t* ctx, uint64_t id) {
  if (ctx == nullptr) {
    return;
  }
  ctx->target_index_id = id;
  ctx->target_index_set = true;
}

static const IndexDef* find_index_by_name(const std::string& name) {
  const std::string needle = to_lower_copy(name);
  for (const auto& idx : g_index_defs) {
    if (to_lower_copy(idx.name) == needle) {
      return &idx;
    }
  }
  return nullptr;
}

static const IndexDef* find_index_by_id(uint64_t id) {
  for (const auto& idx : g_index_defs) {
    if (idx.id == id && id != 0) {
      return &idx;
    }
  }
  return nullptr;
}

static bool build_index_columns(const IndexDef& idx,
                                std::vector<MyColumnDef>* out_columns) {
  if (out_columns == nullptr) {
    return false;
  }
  out_columns->clear();
  if (idx.elements.empty()) {
    return false;
  }

  std::vector<IndexElementDef> elems = idx.elements;
  std::sort(elems.begin(), elems.end(),
            [](const IndexElementDef& a, const IndexElementDef& b) {
              return a.ordinal_position < b.ordinal_position;
            });

  for (const auto& elem : elems) {
    if (elem.column_opx < 0 ||
        elem.column_opx >= static_cast<int>(g_columns_by_opx.size())) {
      std::cerr << "[Warn] Index '" << idx.name
                << "' refers to invalid column_opx=" << elem.column_opx << "\n";
      continue;
    }

    MyColumnDef col = g_columns_by_opx[elem.column_opx];
    if (elem.length != 0xFFFFFFFFu && elem.length > 0) {
      if (col.char_length == 0 || elem.length < col.char_length) {
        col.char_length = elem.length;
      }
    }
    out_columns->push_back(std::move(col));
  }

  return !out_columns->empty();
}

static bool parse_index_defs(const rapidjson::Value& dd_obj) {
  g_index_defs.clear();
  if (!dd_obj.HasMember("indexes") || !dd_obj["indexes"].IsArray()) {
    return false;
  }

  for (auto& idx : dd_obj["indexes"].GetArray()) {
    if (!idx.IsObject()) {
      continue;
    }

    IndexDef def{};
    if (idx.HasMember("name") && idx["name"].IsString()) {
      def.name = idx["name"].GetString();
      def.is_primary = (def.name == "PRIMARY");
    }

    if (idx.HasMember("se_private_data") && idx["se_private_data"].IsString()) {
      const auto kv = parse_kv_string(idx["se_private_data"].GetString());
      auto it = kv.find("id");
      if (it != kv.end()) {
        parse_uint64_value(it->second, &def.id);
      }
      auto root_it = kv.find("root");
      if (root_it != kv.end()) {
        uint32_t root = 0;
        if (parse_uint32_value(root_it->second, &root)) {
          def.root = static_cast<page_no_t>(root);
        }
      }
    }

    if (idx.HasMember("elements") && idx["elements"].IsArray()) {
      for (auto& el : idx["elements"].GetArray()) {
        if (!el.IsObject()) {
          continue;
        }
        if (!el.HasMember("column_opx") || !el["column_opx"].IsInt()) {
          continue;
        }
        IndexElementDef elem{};
        elem.column_opx = el["column_opx"].GetInt();
        if (el.HasMember("ordinal_position") && el["ordinal_position"].IsInt()) {
          elem.ordinal_position = el["ordinal_position"].GetInt();
        }
        if (el.HasMember("length") && el["length"].IsUint()) {
          elem.length = el["length"].GetUint();
        }
        if (el.HasMember("hidden") && el["hidden"].IsBool()) {
          elem.hidden = el["hidden"].GetBool();
        }
        def.elements.push_back(elem);
      }
    }

    if (!def.name.empty() && !def.elements.empty()) {
      g_index_defs.push_back(std::move(def));
    }
  }

  return !g_index_defs.empty();
}

static bool parse_index_selector(const std::string& selector, uint64_t* out) {
  if (selector.empty() || out == nullptr) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  unsigned long long val = std::strtoull(selector.c_str(), &end, 10);
  if (errno != 0 || end == selector.c_str() || *end != '\0') {
    return false;
  }
  *out = static_cast<uint64_t>(val);
  return true;
}

bool has_sdi_index_definitions() {
  return !g_index_defs.empty();
}

void print_sdi_indexes(FILE* out) {
  if (!out) {
    return;
  }
  if (g_index_defs.empty()) {
    fprintf(out, "No indexes found in SDI.\n");
    return;
  }
  fprintf(out, "Indexes in SDI:\n");
  for (const auto& idx : g_index_defs) {
    fprintf(out, "  - %s (id=%llu root=%u fields=%zu)\n",
            idx.name.c_str(),
            static_cast<unsigned long long>(idx.id),
            static_cast<unsigned int>(idx.root),
            idx.elements.size());
  }
}

bool select_index_for_parsing(parser_context_t* ctx,
                              const std::string& selector,
                              std::string* error) {
  if (ctx == nullptr) {
    if (error) {
      *error = "Parser context is null";
    }
    return false;
  }
  if (g_index_defs.empty()) {
    if (error) {
      *error = "SDI does not contain index definitions";
    }
    return false;
  }

  std::string sel = selector;
  if (sel.empty()) {
    sel = "PRIMARY";
  }

  const IndexDef* chosen = nullptr;
  uint64_t numeric_id = 0;
  if (parse_index_selector(sel, &numeric_id)) {
    chosen = find_index_by_id(numeric_id);
  }
  if (!chosen) {
    chosen = find_index_by_name(sel);
  }

  if (!chosen) {
    if (error) {
      *error = "Requested index '" + sel + "' not found in SDI";
    }
    return false;
  }

  if (!build_index_columns(*chosen, &g_columns)) {
    if (error) {
      *error = "Failed to build columns for index '" + chosen->name + "'";
    }
    return false;
  }

  ctx->target_index_name = chosen->name;
  ctx->target_index_root = chosen->root;
  if (chosen->id != 0) {
    set_target_index_id(ctx, chosen->id);
  } else {
    ctx->target_index_set = false;
  }

  return true;
}

page_no_t selected_index_root(const parser_context_t* ctx) {
  if (ctx == nullptr) {
    return FIL_NULL;
  }
  return ctx->target_index_root;
}

const std::string& selected_index_name(const parser_context_t* ctx) {
  static const std::string empty;
  if (ctx == nullptr) {
    return empty;
  }
  return ctx->target_index_name;
}

bool target_index_is_set(const parser_context_t* ctx) {
  return ctx != nullptr && ctx->target_index_set;
}

void set_target_index_id_from_value(parser_context_t* ctx, uint64_t id) {
  set_target_index_id(ctx, id);
}

// If you used "my_rec_offs_*" from the "undrop" style code:
extern ulint my_rec_offs_nth_size(const ulint* offsets, ulint i);
extern bool  my_rec_offs_nth_extern(const ulint* offsets, ulint i);
extern const unsigned char*
       my_rec_get_nth_field(const rec_t* rec, const ulint* offsets,
                            ulint i, ulint* len);

// --------------------------------------------------------------------
// 1) debug_print_table_def: print table->fields[] in a user-friendly way.
void debug_print_table_def(const table_def_t *table)
{
    if (!table) {
        printf("[debug_print_table_def] table is NULL\n");
        return;
    }

    printf("=== Table Definition for '%s' ===\n", (table->name ? table->name : "(null)"));
    printf("fields_count=%u, n_nullable=%u\n", table->fields_count, table->n_nullable);

    // Possibly also print data_min_size / data_max_size if your code uses them:
    // e.g. printf("data_min_size=%d, data_max_size=%ld\n",
    //              table->data_min_size, table->data_max_size);

    for (int i = 0; i < table->fields_count; i++) {
        const field_def_t *fld = &table->fields[i];

        // for "type" => we have an enum { FT_INT, FT_UINT, FT_CHAR, FT_TEXT, FT_DATETIME, FT_INTERNAL, etc. }
        // We'll define a helper to map enum => string:
        const char* type_str = nullptr;
        switch (fld->type) {
        case FT_INTERNAL:   type_str = "FT_INTERNAL"; break;
        case FT_INT:        type_str = "FT_INT";      break;
        case FT_UINT:       type_str = "FT_UINT";     break;
        case FT_CHAR:       type_str = "FT_CHAR";     break;
        case FT_TEXT:       type_str = "FT_TEXT";     break;
        case FT_JSON:       type_str = "FT_JSON";     break;
        case FT_BLOB:       type_str = "FT_BLOB";     break;
        case FT_BIN:        type_str = "FT_BIN";      break;
        case FT_DATE:       type_str = "FT_DATE";     break;
        case FT_TIME:       type_str = "FT_TIME";     break;
        case FT_DATETIME:   type_str = "FT_DATETIME"; break;
        case FT_TIMESTAMP:  type_str = "FT_TIMESTAMP"; break;
        case FT_YEAR:       type_str = "FT_YEAR";     break;
        case FT_ENUM:       type_str = "FT_ENUM";     break;
        case FT_SET:        type_str = "FT_SET";      break;
        case FT_BIT:        type_str = "FT_BIT";      break;
        case FT_DECIMAL:    type_str = "FT_DECIMAL";  break;
        case FT_FLOAT:      type_str = "FT_FLOAT";    break;
        case FT_DOUBLE:     type_str = "FT_DOUBLE";   break;
        // ... if you have more, add them
        default:            type_str = "FT_???";      break;
        }

        printf(" Field #%u:\n", i);
        printf("   name=%s\n", (fld->name ? fld->name : "(null)"));
        printf("   type=%s\n", type_str);
        printf("   can_be_null=%s\n", (fld->can_be_null ? "true" : "false"));
        printf("   fixed_length=%u\n", fld->fixed_length);
        printf("   min_length=%u, max_length=%u\n", fld->min_length, fld->max_length);
        printf("   decimal_precision=%d, decimal_digits=%d\n",
               fld->decimal_precision, fld->decimal_digits);
        printf("   time_precision=%d\n", fld->time_precision);
    }
    printf("=== End of Table Definition ===\n\n");
}

// --------------------------------------------------------------------
// 2) debug_print_compact_row: read each field from offsets[] 
//    and print in a "rough" typed format (e.g. int => 4 bytes, char => string).
//
//    This is for demonstration or debugging. 
//    If your code calls "check_for_a_record(...)" first to build offsets, 
//    you can then do:
//      debug_print_compact_row(page, rec, table, offsets);
// 
void debug_print_compact_row(const page_t* page,
                             const rec_t* rec,
                             const table_def_t* table,
                             const ulint* offsets)
{
    if (!page || !rec || !table || !offsets) {
        printf("[debug_print_compact_row] invalid pointer(s)\n");
        return;
    }

    // Print a header line or something
    printf("Row at rec=%p => columns:\n", (const void*)rec);

    // For each field
    for (ulint i = 0; i < (ulint)table->fields_count; i++) {

        // read the pointer and length
        ulint field_len;
        const unsigned char* field_ptr = my_rec_get_nth_field(rec, offsets, i, &field_len);

        // If length is UNIV_SQL_NULL => print "NULL"
        if (field_len == UNIV_SQL_NULL) {
            printf("  [%2lu] %-15s => NULL\n", i, table->fields[i].name);
            continue;
        }

        // Otherwise interpret based on "table->fields[i].type"
        switch (table->fields[i].type) {
        case FT_INT:
        case FT_UINT:
            // If it's truly a 4-byte int, let's read it:
            if (field_len > 0 && field_len <= 8) {
                if (table->fields[i].type == FT_UINT) {
                    uint64_t val = read_be_uint(field_ptr, field_len);
                    printf("  [%2lu] %-15s => (UINT) %llu\n",
                           i, table->fields[i].name,
                           static_cast<unsigned long long>(val));
                } else {
                    int64_t val = read_be_int_signed(field_ptr, field_len);
                    printf("  [%2lu] %-15s => (INT) %lld\n",
                           i, table->fields[i].name,
                           static_cast<long long>(val));
                }
            } else {
                // length isn't 4 => just hex-dump or do naive printing
                printf("  [%2lu] %-15s => (INT?) length=%lu => ",
                       i, table->fields[i].name, (unsigned long)field_len);
                for (ulint k=0; k<field_len && k<16; k++) {
                    printf("%02X ", field_ptr[k]);
                }
                printf("\n");
            }
            break;

        case FT_CHAR:
        case FT_TEXT:
            // Treat as textual => do a naive printing (limit ~200 bytes for safety)
            {
                ulint to_print = (field_len < 200 ? field_len : 200);
                printf("  [%2lu] %-15s => (CHAR) len=%lu => \"", 
                       i, table->fields[i].name, (unsigned long)field_len);
                for (ulint k=0; k<to_print; k++) {
                    unsigned char c = field_ptr[k];
                    if (c >= 32 && c < 127) {
                        putchar((int)c);
                    } else {
                        // print as \xNN
                        printf("\\x%02X", c);
                    }
                }
                if (field_len > 200) printf("...(truncated)...");
                printf("\"\n");
            }
            break;

        case FT_DATETIME:
        case FT_TIMESTAMP:
            {
                std::string formatted;
                unsigned int dec = static_cast<unsigned int>(table->fields[i].time_precision);
                bool ok = false;
                if (table->fields[i].type == FT_DATETIME) {
                    ok = format_innodb_datetime(field_ptr, field_len, dec, formatted);
                } else {
                    ok = format_innodb_timestamp(field_ptr, field_len, dec, formatted);
                }
                if (ok) {
                    printf("  [%2lu] %-15s => (%s) %s\n",
                           i, table->fields[i].name,
                           table->fields[i].type == FT_DATETIME ? "DATETIME" : "TIMESTAMP",
                           formatted.c_str());
                } else {
                    printf("  [%2lu] %-15s => (%s) length=%lu => raw hex ",
                           i, table->fields[i].name,
                           table->fields[i].type == FT_DATETIME ? "DATETIME" : "TIMESTAMP",
                           (unsigned long)field_len);
                    for (ulint k = 0; k < field_len && k < 16; k++) {
                        printf("%02X ", field_ptr[k]);
                    }
                    printf("\n");
                }
            }
            break;

        case FT_INTERNAL:
            // e.g. DB_TRX_ID(6 bytes) or DB_ROLL_PTR(7 bytes)
            printf("  [%2lu] %-15s => (INTERNAL) length=%lu => ", 
                   i, table->fields[i].name, (unsigned long)field_len);
            for (ulint k=0; k<field_len && k<16; k++) {
                printf("%02X ", field_ptr[k]);
            }
            printf("\n");
            break;

        // ... other types (FLOAT, DOUBLE, DECIMAL, BLOB, etc.)
        default:
            // fallback => hex-dump
            printf("  [%2lu] %-15s => (type=%d) length=%lu => ",
                   i, table->fields[i].name, table->fields[i].type,
                   (unsigned long)field_len);
            for (ulint k=0; k<field_len && k<16; k++) {
                printf("%02X ", field_ptr[k]);
            }
            if (field_len>16) printf("...(truncated)...");
            printf("\n");
            break;
        } // switch
    }

    printf("End of row\n\n");
}

/**
 * discover_target_index_id():
 *   Scans all pages to find the *first* root page
 *   (FIL_PAGE_INDEX + btr_root_fseg_validate() checks).
 *   Once found, read PAGE_INDEX_ID from that page
 *   and store in the parser context.
 *
 *   Returns 0 if success, non-0 if error.
 */

int discover_target_index_id(int fd, parser_context_t* ctx)
{
  if (ctx == nullptr) {
    fprintf(stderr, "discover_target_index_id: parser context is null.\n");
    return 1;
  }

  // Determine page size via MySQL helper
  File mfd = (File)fd;
  page_size_t pg_sz(0,0,false);
  if (!determine_page_size(mfd, pg_sz)) { fprintf(stderr, "Cannot read page size\n"); return 1; }
  const size_t physical_size = pg_sz.physical();
  const size_t logical_size = pg_sz.logical();
  const bool tablespace_compressed = (physical_size < logical_size);
  // file size
  struct stat stat_buf;
  if (fstat(fd, &stat_buf) == -1) { perror("fstat"); return 1; }
  const off_t total = stat_buf.st_size;
  const off_t block_num = total / (off_t)physical_size;
  if (block_num <= 0) { fprintf(stderr, "Empty file?\n"); return 1; }
  std::vector<unsigned char> page_buf(physical_size);
  std::vector<unsigned char> logical_buf;
  if (tablespace_compressed) {
    logical_buf.resize(logical_size);
  }
  if (pread(fd, page_buf.data(), physical_size, 0) != (ssize_t)physical_size) {
    perror("pread page0");
    return 1;
  }
  uint32_t space_id = mach_read_from_4(page_buf.data() + FSP_HEADER_OFFSET + FSP_SPACE_ID);

  // 4) loop over each page
  for (int i = 0; i < block_num; i++) {
    off_t offset = (off_t) i * physical_size;
    if (pread(fd, page_buf.data(), physical_size, offset) != (ssize_t)physical_size) {
      // partial read => break or return error
      break;
    }

    // check if FIL_PAGE_INDEX
    if (fil_page_get_type(page_buf.data()) == FIL_PAGE_INDEX) {
      const unsigned char* page_data = page_buf.data();
      if (tablespace_compressed) {
        size_t actual_size = 0;
        if (!decompress_page_inplace(page_buf.data(),
                                     physical_size,
                                     logical_size,
                                     logical_buf.data(),
                                     logical_size,
                                     &actual_size)) {
          continue;
        }
        if (actual_size != logical_size) {
          continue;
        }
        page_data = logical_buf.data();
      }
      if (!page_is_comp(page_data)) {
        continue;
      }

      // Check if this is a *root* page (like ShowIndexSummary does)
      // by verifying the fseg headers for leaf and top
      bool is_root = btr_root_fseg_validate(page_data + FIL_PAGE_DATA + PAGE_BTR_SEG_LEAF, space_id)
                  && btr_root_fseg_validate(page_data + FIL_PAGE_DATA + PAGE_BTR_SEG_TOP, space_id);

      if (is_root) {
        // We consider the *first* root we find as the "Primary index"
        uint64_t idx_id_64 = read_uint64_from_page(page_data + PAGE_HEADER + PAGE_INDEX_ID);
        set_target_index_id(ctx, idx_id_64);
        const uint32_t high = static_cast<uint32_t>(ctx->target_index_id >> 32);
        const uint32_t low = static_cast<uint32_t>(ctx->target_index_id & 0xffffffff);

        fprintf(stderr, "discover_target_index_id: Found root at page=%d  index_id=%u:%u\n",
                i, high, low);

        // done
        return 0;
      }
    }
  }

  // if we got here, we never found a root => maybe the table is empty or corrupted
  fprintf(stderr, "discover_target_index_id: No root page found.\n");
  return 1;
}

bool is_target_index(const unsigned char* page, const parser_context_t* ctx)
{
  if (ctx == nullptr || !ctx->target_index_set) {
    // We never discovered the target index => default to false
    return false;
  }

  uint64_t page_index_id = read_uint64_from_page(page + PAGE_HEADER + PAGE_INDEX_ID);
  return page_index_id == ctx->target_index_id;
}

static std::string to_lower_copy(const std::string& in) {
  std::string out = in;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
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
  if (s.empty() || out == nullptr) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  unsigned long long v = std::strtoull(s.c_str(), &end, 10);
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
  if (out != nullptr) {
    *out = static_cast<uint32_t>(tmp);
  }
  return true;
}

// read_be_int_signed is defined earlier (near line 322)

static uint64_t read_be_uint(const unsigned char* ptr, size_t len) {
  uint64_t val = 0;
  for (size_t i = 0; i < len; i++) {
    val = (val << 8) | ptr[i];
  }
  return val;
}

static bool parse_first_paren_number(const std::string& s, unsigned int* out) {
  size_t l = s.find('(');
  size_t r = s.find(')', l == std::string::npos ? 0 : l + 1);
  if (l == std::string::npos || r == std::string::npos || r <= l + 1) {
    return false;
  }
  std::string inner = s.substr(l + 1, r - l - 1);
  char* end = nullptr;
  unsigned long val = std::strtoul(inner.c_str(), &end, 10);
  if (end == inner.c_str()) {
    return false;
  }
  *out = static_cast<unsigned int>(val);
  return true;
}

static bool parse_two_paren_numbers(const std::string& s, int* a, int* b) {
  size_t l = s.find('(');
  size_t r = s.find(')', l == std::string::npos ? 0 : l + 1);
  if (l == std::string::npos || r == std::string::npos || r <= l + 1) {
    return false;
  }
  std::string inner = s.substr(l + 1, r - l - 1);
  size_t comma = inner.find(',');
  if (comma == std::string::npos) {
    char* end = nullptr;
    long val = std::strtol(inner.c_str(), &end, 10);
    if (end == inner.c_str()) {
      return false;
    }
    *a = static_cast<int>(val);
    *b = 0;
    return true;
  }
  std::string left = inner.substr(0, comma);
  std::string right = inner.substr(comma + 1);
  char* end_left = nullptr;
  char* end_right = nullptr;
  long val_left = std::strtol(left.c_str(), &end_left, 10);
  long val_right = std::strtol(right.c_str(), &end_right, 10);
  if (end_left == left.c_str() || end_right == right.c_str()) {
    return false;
  }
  *a = static_cast<int>(val_left);
  *b = static_cast<int>(val_right);
  return true;
}

static unsigned int decimal_storage_bytes(int precision, int scale) {
  static const unsigned char dig2bytes[10] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
  if (precision <= 0) {
    return 0;
  }
  if (scale < 0) {
    scale = 0;
  }
  int intg = precision - scale;
  if (intg < 0) {
    intg = 0;
  }
  int intg0 = intg / 9;
  int intg0x = intg - intg0 * 9;
  int frac0 = scale / 9;
  int frac0x = scale - frac0 * 9;
  unsigned int bytes = 0;
  bytes += static_cast<unsigned int>(intg0 * 4);
  bytes += dig2bytes[intg0x];
  bytes += static_cast<unsigned int>(frac0 * 4);
  bytes += dig2bytes[frac0x];
  return bytes;
}

static unsigned int temporal_storage_bytes(const std::string& type, int precision) {
  if (precision < 0) {
    precision = 0;
  }
  if (precision > 6) {
    precision = 6;
  }
  unsigned int frac = static_cast<unsigned int>((precision + 1) / 2);
  if (type == "datetime") {
    return 5 + frac;
  }
  if (type == "timestamp") {
    return 4 + frac;
  }
  if (type == "time") {
    return 3 + frac;
  }
  return 0;
}

static void set_fixed(field_def_t* fld, unsigned int len) {
  fld->fixed_length = static_cast<int>(len);
  fld->min_length = len;
  fld->max_length = len;
}

static void set_var(field_def_t* fld, unsigned int max_len) {
  fld->fixed_length = 0;
  fld->min_length = 0;
  fld->max_length = max_len;
}

static unsigned int clamp_var_max(unsigned int default_len, uint32_t col_len) {
  if (col_len > 0 && col_len < default_len) {
    return static_cast<unsigned int>(col_len);
  }
  return default_len;
}

static bool char_is_variable_length(uint32_t collation_id) {
  if (collation_id == 0) {
    return false;
  }
  const CHARSET_INFO* cs = get_charset(collation_id, MYF(0));
  if (cs == nullptr || cs == &my_charset_bin) {
    return false;
  }
  return cs->mbmaxlen > 1;
}

static bool is_internal_column_name(const std::string& name) {
  return (name == "DB_TRX_ID" || name == "DB_ROLL_PTR" || name == "DB_ROW_ID");
}

static unsigned int internal_column_length(const std::string& name,
                                            unsigned int fallback) {
  if (name == "DB_TRX_ID") return 6;
  if (name == "DB_ROLL_PTR") return 7;
  if (name == "DB_ROW_ID") return 6;
  return fallback;
}

/**
 * build_table_def_from_json():
 *   Creates a table_def_t from the columns in g_columns.
 *   This way, you can pass table_def_t into ibrec_init_offsets_new().
 *
 *   @param[out] table    The table_def_t to fill
 *   @param[in]  tbl_name The table name (e.g. "HISTORICO")
 *   @return 0 on success
 */
int build_table_def_from_json(table_def_t* table, const char* tbl_name)
{
    // 1) Zero out "table_def_t"
    std::memset(table, 0, sizeof(table_def_t));

    // 2) Copy the table name
    table->name = strdup(tbl_name);

    // 3) Loop over columns
    unsigned colcount = 0;
    for (size_t i = 0; i < g_columns.size(); i++) {
        if (colcount >= MAX_TABLE_FIELDS) {
            fprintf(stderr, "[Error] Too many columns (>MAX_TABLE_FIELDS)\n");
            return 1;
        }

        const MyColumnDef& col = g_columns[i];
        field_def_t* fld = &table->fields[colcount];
        std::memset(fld, 0, sizeof(*fld));

        // (A) Name
        fld->name = strdup(col.name.c_str());
        fld->collation_id = col.collation_id;

        // (B) is_nullable => can_be_null
        // If the JSON had is_nullable => g_columns[i].nullable, adapt.
        // Let's assume we store is_nullable in g_columns[i].is_nullable:
        fld->can_be_null = col.is_nullable;

        // (C) If the JSON had "is_unsigned" => store or adapt type
        // For example, if we see "int" + is_unsigned => FT_UINT
        bool is_unsigned = col.is_unsigned;
        std::string type = to_lower_copy(col.type_utf8);

        if (is_internal_column_name(col.name) || (type.empty() && col.hidden > 1)) {
            fld->type = FT_INTERNAL;
            unsigned int len = internal_column_length(col.name, col.char_length);
            set_fixed(fld, len);
            colcount++;
            continue;
        }

        if (type.empty()) {
            if (col.char_length == 0) {
                std::cerr << "[Warn] Column '" << col.name
                          << "' has no type and no length, skipping.\n";
                continue;
            }
            fld->type = FT_INTERNAL;
            set_fixed(fld, col.char_length);
            colcount++;
            continue;
        }

        if (type.find("tinyint") != std::string::npos ||
            type == "bool" || type == "boolean") {
            fld->type = is_unsigned ? FT_UINT : FT_INT;
            set_fixed(fld, 1);

        } else if (type.find("smallint") != std::string::npos) {
            fld->type = is_unsigned ? FT_UINT : FT_INT;
            set_fixed(fld, 2);

        } else if (type.find("mediumint") != std::string::npos) {
            fld->type = is_unsigned ? FT_UINT : FT_INT;
            set_fixed(fld, 3);

        } else if (type.find("bigint") != std::string::npos) {
            fld->type = is_unsigned ? FT_UINT : FT_INT;
            set_fixed(fld, 8);

        } else if (type.find("int") != std::string::npos ||
                   type.find("integer") != std::string::npos) {
            fld->type = is_unsigned ? FT_UINT : FT_INT;
            set_fixed(fld, 4);

        } else if (type.find("float") != std::string::npos) {
            fld->type = FT_FLOAT;
            set_fixed(fld, 4);

        } else if (type.find("double") != std::string::npos) {
            fld->type = FT_DOUBLE;
            set_fixed(fld, 8);

        } else if (type.find("decimal") != std::string::npos ||
                   type.find("numeric") != std::string::npos) {
            fld->type = FT_DECIMAL;
            int precision = col.numeric_precision;
            int scale = col.numeric_scale;
            if (precision == 0 && scale == 0) {
                parse_two_paren_numbers(type, &precision, &scale);
            }
            fld->decimal_precision = precision;
            fld->decimal_digits = scale;
            unsigned int len = decimal_storage_bytes(precision, scale);
            if (len == 0 && col.char_length > 0) {
                len = col.char_length;
            }
            set_fixed(fld, len);

        } else if (type.find("datetime") != std::string::npos) {
            fld->type = FT_DATETIME;
            fld->time_precision = col.datetime_precision;
            set_fixed(fld, temporal_storage_bytes("datetime", col.datetime_precision));

        } else if (type.find("timestamp") != std::string::npos) {
            fld->type = FT_TIMESTAMP;
            fld->time_precision = col.datetime_precision;
            set_fixed(fld, temporal_storage_bytes("timestamp", col.datetime_precision));

        } else if (type.find("time") != std::string::npos) {
            fld->type = FT_TIME;
            fld->time_precision = col.datetime_precision;
            set_fixed(fld, temporal_storage_bytes("time", col.datetime_precision));

        } else if (type.find("date") != std::string::npos) {
            fld->type = FT_DATE;
            set_fixed(fld, 3);

        } else if (type.find("year") != std::string::npos) {
            fld->type = FT_YEAR;
            set_fixed(fld, 1);

        } else if (type.find("bit") != std::string::npos) {
            fld->type = FT_BIT;
            unsigned int bits = col.char_length;
            parse_first_paren_number(type, &bits);
            unsigned int bytes = (bits + 7) / 8;
            set_fixed(fld, bytes);

        } else if (type.find("varbinary") != std::string::npos) {
            fld->type = FT_BIN;
            unsigned int max_len = col.char_length;
            unsigned int parsed = 0;
            if (parse_first_paren_number(type, &parsed) && max_len == 0) {
                max_len = parsed;
            }
            set_var(fld, max_len);

        } else if (type.find("binary") != std::string::npos) {
            fld->type = FT_BIN;
            unsigned int len = col.char_length;
            unsigned int parsed = 0;
            if (parse_first_paren_number(type, &parsed) && len == 0) {
                len = parsed;
            }
            set_fixed(fld, len);

        } else if (type.find("varchar") != std::string::npos) {
            fld->type = FT_CHAR;
            unsigned int max_len = col.char_length;
            unsigned int parsed = 0;
            if (parse_first_paren_number(type, &parsed) && max_len == 0) {
                max_len = parsed;
            }
            set_var(fld, max_len);

        } else if (type.find("char") != std::string::npos) {
            fld->type = FT_CHAR;
            unsigned int len = col.char_length;
            unsigned int parsed = 0;
            if (parse_first_paren_number(type, &parsed) && len == 0) {
                len = parsed;
            }
            if (char_is_variable_length(col.collation_id)) {
                set_var(fld, len);
            } else {
                set_fixed(fld, len);
            }

        } else if (type.find("tinytext") != std::string::npos) {
            fld->type = FT_TEXT;
            set_var(fld, clamp_var_max(255, col.char_length));

        } else if (type.find("mediumtext") != std::string::npos) {
            fld->type = FT_TEXT;
            set_var(fld, clamp_var_max(16777215, col.char_length));

        } else if (type.find("longtext") != std::string::npos) {
            fld->type = FT_TEXT;
            set_var(fld, clamp_var_max(0xFFFFFFFFu, col.char_length));

        } else if (type.find("text") != std::string::npos) {
            fld->type = FT_TEXT;
            set_var(fld, clamp_var_max(65535, col.char_length));

        } else if (type.find("tinyblob") != std::string::npos) {
            fld->type = FT_BLOB;
            set_var(fld, clamp_var_max(255, col.char_length));

        } else if (type.find("mediumblob") != std::string::npos) {
            fld->type = FT_BLOB;
            set_var(fld, clamp_var_max(16777215, col.char_length));

        } else if (type.find("longblob") != std::string::npos) {
            fld->type = FT_BLOB;
            set_var(fld, clamp_var_max(0xFFFFFFFFu, col.char_length));

        } else if (type.find("blob") != std::string::npos) {
            fld->type = FT_BLOB;
            set_var(fld, clamp_var_max(65535, col.char_length));

        } else if (type.find("enum") != std::string::npos) {
            fld->type = FT_ENUM;
            unsigned int len = (col.elements_count > 255) ? 2 : 1;
            set_fixed(fld, len);

        } else if (type.find("set") != std::string::npos) {
            fld->type = FT_SET;
            unsigned int len = static_cast<unsigned int>((col.elements_count + 7) / 8);
            if (len == 0) {
                len = 1;
            }
            set_fixed(fld, len);

        } else if (type.find("json") != std::string::npos) {
            fld->type = FT_JSON;
            set_var(fld, clamp_var_max(0xFFFFFFFFu, col.char_length));

        } else if (type.find("geometry") != std::string::npos) {
            fld->type = FT_BLOB;
            set_var(fld, clamp_var_max(0xFFFFFFFFu, col.char_length));

        } else {
            fld->type = FT_TEXT;
            unsigned int max_len = col.char_length > 0 ? col.char_length : 255;
            set_var(fld, max_len);
        }

        if ((fld->type == FT_ENUM || fld->type == FT_SET) &&
            col.elements_complete && !col.elements.empty()) {
            fld->has_limits = true;
            if (fld->type == FT_ENUM) {
                size_t count = col.elements.size();
                if (count > MAX_ENUM_VALUES) {
                    count = MAX_ENUM_VALUES;
                }
                fld->limits.enum_values_count = static_cast<int>(count);
                for (size_t j = 0; j < count; j++) {
                    fld->limits.enum_values[j] = strdup(col.elements[j].c_str());
                }
            } else {
                size_t count = col.elements.size();
                if (count > MAX_SET_VALUES) {
                    count = MAX_SET_VALUES;
                }
                fld->limits.set_values_count = static_cast<int>(count);
                for (size_t j = 0; j < count; j++) {
                    fld->limits.set_values[j] = strdup(col.elements[j].c_str());
                }
            }
        }

        // (E) Possibly parse numeric precision, scale => decimal_digits, etc.
        // (F) Possibly parse "char_length" => do above or below.

        // done
        colcount++;
    }

    // 5) fields_count
    table->fields_count = colcount;

    // 6) Optionally compute table->n_nullable
    table->n_nullable = 0;
    for (unsigned i = 0; i < colcount; i++) {
        if (table->fields[i].can_be_null) {
            table->n_nullable++;
        }
    }

    // optionally set data_max_size, data_min_size
    // or do so in your calling code if you want consistent row checks.

    return 0;
}

/**
 * load_ib2sdi_table_columns():
 *   Parses an ib2sdi-generated JSON file (like the one you pasted),
 *   searches for the array element that has "dd_object_type" == "Table",
 *   then extracts its "columns" array from "dd_object" and the table name.
 *
 * Returns 0 on success, non-0 on error.
 */
int load_ib2sdi_table_columns(const char* json_path,
                              std::string& table_name,
                              parser_context_t* ctx)
{
    // 1) Open the file
    FILE* fp = std::fopen(json_path, "rb");
    if (!fp) {
        std::cerr << "[Error] Could not open JSON file: " << json_path << std::endl;
        return 1;
    }

    // 2) Parse the top-level JSON
    char read_buffer[1 << 16];
    rapidjson::FileReadStream isw(fp, read_buffer, sizeof(read_buffer));
    rapidjson::Document d;
    d.ParseStream(isw);
    if (d.HasParseError()) {
        std::cerr << "[Error] JSON parse error: " 
                  << rapidjson::GetParseError_En(d.GetParseError())
                  << " at offset " << d.GetErrorOffset() << std::endl;
        std::fclose(fp);
        return 1;
    }

    if (!d.IsArray()) {
        std::cerr << "[Error] Top-level JSON is not an array.\n";
        std::fclose(fp);
        return 1;
    }

    // 3) Find the array element whose "dd_object_type" == "Table".
    //    In your example, you had something like:
    //    [
    //       "ibd2sdi",
    //       { "type":1, "object": { "dd_object_type":"Table", ... } },
    //       { "type":2, "object": { "dd_object_type":"Tablespace", ... } }
    //    ]
    // We'll loop the array to find the "Table" entry.

    const rapidjson::Value* table_obj = nullptr;

    for (auto& elem : d.GetArray()) {
        // Each elem might be "ibd2sdi" (string) or an object with { "type":..., "object":... }
        if (elem.IsObject() && elem.HasMember("object")) {
            const rapidjson::Value& obj = elem["object"];
            if (obj.HasMember("dd_object_type") && obj["dd_object_type"].IsString()) {
                std::string ddtype = obj["dd_object_type"].GetString();
                if (ddtype == "Table") {
                    // Found the table element
                    table_obj = &obj;
                    break; 
                }
            }
        }
    }

    if (!table_obj) {
        std::cerr << "[Error] Could not find any array element with dd_object_type=='Table'.\n";
        std::fclose(fp);
        return 1;
    }

    // 4) Inside that "object", we want "dd_object" => "columns"
    //    i.e. table_obj->HasMember("dd_object") => columns in table_obj["dd_object"]["columns"]
    if (!table_obj->HasMember("dd_object")) {
        std::cerr << "[Error] Table object is missing 'dd_object' member.\n";
        std::fclose(fp);
        return 1;
    }
    const rapidjson::Value& dd_obj = (*table_obj)["dd_object"];

    // Extract table name from dd_object
    if (dd_obj.HasMember("name") && dd_obj["name"].IsString()) {
        table_name = dd_obj["name"].GetString();
        std::cout << "[Debug] Extracted table name: " << table_name << "\n";
    } else {
        std::cerr << "[Warning] 'dd_object' is missing 'name'. Using default 'UNKNOWN_TABLE'.\n";
        table_name = "UNKNOWN_TABLE";
    }

    if (!dd_obj.HasMember("columns") || !dd_obj["columns"].IsArray()) {
        std::cerr << "[Error] 'dd_object' is missing 'columns' array.\n";
        std::fclose(fp);
        return 1;
    }

    const rapidjson::Value& columns = dd_obj["columns"];
    g_columns.clear();
    g_columns_by_opx.clear();
    g_columns_by_opx.resize(columns.Size());

    // 5) Iterate the columns array
    for (rapidjson::SizeType i = 0; i < columns.Size(); ++i) {
        const rapidjson::Value& c = columns[i];

        MyColumnDef def{};
        def.column_opx = static_cast<int>(i);
        if (c.HasMember("name") && c["name"].IsString()) {
            def.name = c["name"].GetString();
        } else {
            std::cerr << "[Warn] Column is missing 'name'.\n";
            def.is_virtual = true;
        }

        if (c.HasMember("column_type_utf8") && c["column_type_utf8"].IsString()) {
            def.type_utf8 = c["column_type_utf8"].GetString();
        }

        if (c.HasMember("char_length") && c["char_length"].IsUint()) {
            def.char_length = c["char_length"].GetUint();
        }
        if (c.HasMember("collation_id")) {
            if (c["collation_id"].IsUint()) {
                def.collation_id = c["collation_id"].GetUint();
            } else if (c["collation_id"].IsInt()) {
                def.collation_id = static_cast<uint32_t>(c["collation_id"].GetInt());
            }
        }

        if (c.HasMember("is_nullable") && c["is_nullable"].IsBool()) {
            def.is_nullable = c["is_nullable"].GetBool();
        }
        if (c.HasMember("is_unsigned") && c["is_unsigned"].IsBool()) {
            def.is_unsigned = c["is_unsigned"].GetBool();
        }
        if (c.HasMember("is_virtual") && c["is_virtual"].IsBool()) {
            def.is_virtual = c["is_virtual"].GetBool();
        }
        if (c.HasMember("hidden") && c["hidden"].IsInt()) {
            def.hidden = c["hidden"].GetInt();
        }
        if (c.HasMember("ordinal_position") && c["ordinal_position"].IsInt()) {
            def.ordinal_position = c["ordinal_position"].GetInt();
        }
        if (c.HasMember("numeric_precision") && c["numeric_precision"].IsInt()) {
            def.numeric_precision = c["numeric_precision"].GetInt();
        }
        if (c.HasMember("numeric_scale") && c["numeric_scale"].IsInt()) {
            def.numeric_scale = c["numeric_scale"].GetInt();
        }
        if (c.HasMember("datetime_precision") && c["datetime_precision"].IsInt()) {
            def.datetime_precision = c["datetime_precision"].GetInt();
        }
        if (c.HasMember("elements") && c["elements"].IsArray()) {
            const auto& elems = c["elements"].GetArray();
            def.elements_count = elems.Size();
            def.elements.resize(elems.Size());
            def.elements_complete = true;
            for (rapidjson::SizeType ei = 0; ei < elems.Size(); ++ei) {
                const auto& el = elems[ei];
                if (el.IsString()) {
                    def.elements[ei] = decode_sdi_string(el.GetString());
                } else if (el.IsObject() && el.HasMember("name") && el["name"].IsString()) {
                    def.elements[ei] = decode_sdi_string(el["name"].GetString());
                } else {
                    def.elements_complete = false;
                }
            }
        }

        g_columns_by_opx[i] = def;

        std::cout << "[Debug] Added column: name='" << def.name
                  << "', type='" << def.type_utf8
                  << "', char_length=" << def.char_length
                  << ", ordinal=" << def.ordinal_position
                  << ", opx=" << def.column_opx
                  << (def.is_virtual ? " (virtual)" : "")
                  << "\n";
    }

    bool have_indexes = parse_index_defs(dd_obj);
    if (have_indexes && ctx != nullptr) {
        std::string err;
        if (select_index_for_parsing(ctx, "PRIMARY", &err) && !g_columns.empty()) {
            std::cout << "[Debug] Using PRIMARY index order for record parsing ("
                      << g_columns.size() << " columns).\n";
        } else {
            std::cerr << "[Warn] PRIMARY index order not found: " << err << "\n";
        }
    } else if (have_indexes) {
        std::cerr << "[Warn] PRIMARY index order not set (missing parser context).\n";
    }

    if (g_columns.empty()) {
        std::vector<MyColumnDef> ordered;
        ordered.reserve(g_columns_by_opx.size());
        for (const auto& col : g_columns_by_opx) {
            if (!col.is_virtual) {
                ordered.push_back(col);
            }
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const MyColumnDef& a, const MyColumnDef& b) {
                             int a_pos = a.ordinal_position == 0
                                             ? std::numeric_limits<int>::max()
                                             : a.ordinal_position;
                             int b_pos = b.ordinal_position == 0
                                             ? std::numeric_limits<int>::max()
                                             : b.ordinal_position;
                             return a_pos < b_pos;
                         });
        g_columns.swap(ordered);
        std::cout << "[Warn] PRIMARY index order not found; using ordinal_position order.\n";
    }

    std::fclose(fp);
    return 0;
}

/**
 * Example: parse and print records from a leaf page.
 * Very minimal, ignoring many corner cases.
 *
 * If you want the "table-based" field parsing from undrop-for-innodb,
 * you’d bring in structures like table_def_t, rec_offs_* helpers, etc.
 */
static bool next_compact_rec_offset(const page_t* page,
                                    ulint rec_offset,
                                    size_t page_size,
                                    ulint* next_offset)
{
  if (rec_offset < REC_NEXT || rec_offset >= page_size) {
    return false;
  }

  const rec_t* rec = reinterpret_cast<const rec_t*>(
      reinterpret_cast<const unsigned char*>(page) + rec_offset);
  const unsigned char* rec_bytes =
      reinterpret_cast<const unsigned char*>(rec);
  const int16_t delta = static_cast<int16_t>(
      mach_read_from_2(rec_bytes - REC_NEXT));

  if (delta == 0) {
    return false;
  }

  int32_t raw = static_cast<int32_t>(rec_offset) + delta;
  int32_t mod = raw % static_cast<int32_t>(page_size);
  if (mod < 0) {
    mod += static_cast<int32_t>(page_size);
  }

  if (mod < static_cast<int32_t>(PAGE_NEW_INFIMUM) ||
      mod >= static_cast<int32_t>(page_size)) {
    return false;
  }

  *next_offset = static_cast<ulint>(mod);
  return true;
}

void parse_records_on_page(const unsigned char* page,
                           size_t page_size,
                           uint64_t page_no,
                           const parser_context_t* ctx)
{
  // 1) Check if this page belongs to the selected index
  if (!is_target_index(page, ctx)) {
    return; // Not selected index => skip
  }

  // 2) Check if it’s a LEAF page
  ulint page_level = mach_read_from_2(page + PAGE_HEADER + PAGE_LEVEL);
  if (page_level != 0) {
    // Non-leaf => skip
    return;
  }

  const std::string index_name = ctx ? ctx->target_index_name : std::string();
  std::cout << "Page " << page_no
            << " is index '" << index_name
            << "' leaf. Parsing records.\n";

  // 3) Check if COMPACT or REDUNDANT
  bool is_compact = page_is_comp(page);
  if (!is_compact) {
    // Skip old REDUNDANT format (not supported).
    return;
  }

  const bool include_deleted = false;

  // 4) Loop from infimum -> supremum using COMPACT offsets
  const ulint inf_offset = PAGE_NEW_INFIMUM;
  ulint n_records  = 0;
  ulint n_deleted  = 0;
  ulint n_invalid  = 0;
  ulint rec_offset = inf_offset;
  ulint steps = 0;
  ulint max_steps = static_cast<ulint>(
      page_size / (REC_N_NEW_EXTRA_BYTES + 1));
  const ulint n_recs = mach_read_from_2(page + PAGE_HEADER + PAGE_N_RECS);
  if (max_steps < n_recs + 2) {
    max_steps = n_recs + 2;
  }

  while (steps < max_steps) {
    const rec_t* rec = reinterpret_cast<const rec_t*>(page + rec_offset);
    const ulint status = rec_get_status(rec);
    if (status == REC_STATUS_SUPREMUM) {
      break;
    }

    if (status == REC_STATUS_ORDINARY) {
      const bool deleted = rec_get_deleted_flag(rec, true);
      if (!deleted || include_deleted) {
        n_records++;
        std::cout << "  - Found record at offset "
                  << rec_offset << " (page " << page_no << ")\n";

        // (A) We'll do the undrop approach: check_for_a_record() => if valid => process_ibrec()
        ulint offsets[MAX_TABLE_FIELDS + 2];
        table_def_t* table = &table_definitions[0];

        bool valid = check_for_a_record(
            (page_t*)page,
            (rec_t*)rec,
            table,
            offsets);

        if (valid) {
          if (parser_debug_enabled()) {
            debug_print_compact_row(page, rec, table, offsets);
          }
          bool hex_output = false;
          RowMeta meta;
          meta.page_no = page_no;
          meta.rec_offset = rec_offset;
          meta.deleted = deleted;
          process_ibrec((page_t*)page, (rec_t*)rec, table, offsets, hex_output, &meta);
        } else {
          n_invalid++;
        }
      } else {
        n_deleted++;
      }
    }

    ulint next_off = 0;
    if (!next_compact_rec_offset((const page_t*)page,
                                 rec_offset,
                                 page_size,
                                 &next_off)) {
      n_invalid++;
      break;
    }

    if (next_off == rec_offset || next_off >= page_size) {
      n_invalid++;
      break;
    }

    rec_offset = next_off;
    steps++;
  }

  std::cout << "Leaf Page " << page_no
            << " had " << n_records << " user records";
  if (n_deleted > 0 || n_invalid > 0) {
    std::cout << " (" << n_deleted << " deleted, "
              << n_invalid << " invalid)";
  }
  std::cout << ".\n";
}

// ============================================================================
// Callback-based record extraction for API use
// ============================================================================

// Helper to read signed big-endian integer for extraction
// InnoDB stores signed integers with the sign bit flipped for B-tree ordering
static int64_t extract_be_int_signed(const unsigned char* ptr, size_t len) {
  if (len == 0 || len > 8) return 0;

  // Read as big-endian unsigned
  uint64_t raw = 0;
  for (size_t i = 0; i < len; i++) {
    raw = (raw << 8) | ptr[i];
  }

  // Flip the sign bit (highest bit)
  uint64_t sign_bit = 1ULL << (len * 8 - 1);
  raw ^= sign_bit;

  // Now interpret as signed: if high bit is set, sign-extend
  if (raw & sign_bit) {
    // Negative: extend sign bits
    uint64_t mask = ~((1ULL << (len * 8)) - 1);
    return static_cast<int64_t>(raw | mask);
  }
  return static_cast<int64_t>(raw);
}

// Helper to read unsigned big-endian integer for extraction
static uint64_t extract_be_uint(const unsigned char* ptr, size_t len) {
  if (len == 0 || len > 8) return 0;
  uint64_t val = 0;
  for (size_t i = 0; i < len; i++) {
    val = (val << 8) | ptr[i];
  }
  return val;
}

bool extract_record_data(const page_t* page,
                         const rec_t* rec,
                         table_def_t* table,
                         const ulint* offsets,
                         uint64_t page_no,
                         ulint rec_offset,
                         bool deleted,
                         parsed_row_t* out_row) {
  if (!out_row || !table || !rec || !offsets) {
    return false;
  }

  memset(out_row, 0, sizeof(*out_row));
  out_row->page_no = page_no;
  out_row->rec_offset = rec_offset;
  out_row->deleted = deleted;
  out_row->column_count = 0;

  for (ulint i = 0; i < (ulint)table->fields_count && i < MAX_TABLE_FIELDS; i++) {
    parsed_column_t& col = out_row->columns[out_row->column_count];
    const field_def_t& field = table->fields[i];

    col.name = field.name;
    col.field_type = field.type;
    col.is_internal = (field.type == FT_INTERNAL);
    col.is_null = false;
    col.data = nullptr;
    col.data_len = 0;
    col.int_val = 0;
    col.formatted[0] = '\0';

    ulint field_len = 0;
    const unsigned char* field_ptr = my_rec_get_nth_field(rec, offsets, i, &field_len);

    if (field_len == UNIV_SQL_NULL) {
      col.is_null = true;
      snprintf(col.formatted, sizeof(col.formatted), "NULL");
      out_row->column_count++;
      continue;
    }

    col.data = field_ptr;
    col.data_len = field_len;

    // Format based on field type
    switch (field.type) {
      case FT_INT: {
        col.int_val = extract_be_int_signed(field_ptr, field_len);
        snprintf(col.formatted, sizeof(col.formatted), "%lld",
                 (long long)col.int_val);
        break;
      }
      case FT_UINT: {
        col.uint_val = extract_be_uint(field_ptr, field_len);
        snprintf(col.formatted, sizeof(col.formatted), "%llu",
                 (unsigned long long)col.uint_val);
        break;
      }
      case FT_FLOAT: {
        if (field_len == 4) {
          uint32_t raw = static_cast<uint32_t>(extract_be_uint(field_ptr, 4));
          float f;
          memcpy(&f, &raw, sizeof(f));
          col.double_val = f;
          snprintf(col.formatted, sizeof(col.formatted), "%f", f);
        } else {
          snprintf(col.formatted, sizeof(col.formatted), "(binary float)");
        }
        break;
      }
      case FT_DOUBLE: {
        if (field_len == 8) {
          uint64_t raw = extract_be_uint(field_ptr, 8);
          double d;
          memcpy(&d, &raw, sizeof(d));
          col.double_val = d;
          snprintf(col.formatted, sizeof(col.formatted), "%f", d);
        } else {
          snprintf(col.formatted, sizeof(col.formatted), "(binary double)");
        }
        break;
      }
      case FT_CHAR:
      case FT_TEXT: {
        // Copy as string, handle length limit
        size_t copy_len = (field_len < sizeof(col.formatted) - 1)
                              ? field_len
                              : sizeof(col.formatted) - 1;
        memcpy(col.formatted, field_ptr, copy_len);
        col.formatted[copy_len] = '\0';
        // Trim trailing spaces for CHAR
        if (field.type == FT_CHAR) {
          while (copy_len > 0 && col.formatted[copy_len - 1] == ' ') {
            col.formatted[--copy_len] = '\0';
          }
        }
        break;
      }
      case FT_DATE: {
        // InnoDB DATE: 3 bytes, sign-bit flipped
        std::string date_str;
        if (format_innodb_date(field_ptr, field_len, date_str)) {
          strncpy(col.formatted, date_str.c_str(), sizeof(col.formatted) - 1);
          col.formatted[sizeof(col.formatted) - 1] = '\0';
        } else {
          snprintf(col.formatted, sizeof(col.formatted), "(invalid date)");
        }
        break;
      }
      case FT_TIME: {
        // InnoDB TIME: 3+ bytes
        std::string time_str;
        if (format_innodb_time(field_ptr, field_len, field.time_precision, time_str)) {
          strncpy(col.formatted, time_str.c_str(), sizeof(col.formatted) - 1);
          col.formatted[sizeof(col.formatted) - 1] = '\0';
        } else {
          snprintf(col.formatted, sizeof(col.formatted), "(invalid time)");
        }
        break;
      }
      case FT_DATETIME: {
        // InnoDB DATETIME: 5-8 bytes
        std::string dt_str;
        if (format_innodb_datetime(field_ptr, field_len, field.time_precision, dt_str)) {
          strncpy(col.formatted, dt_str.c_str(), sizeof(col.formatted) - 1);
          col.formatted[sizeof(col.formatted) - 1] = '\0';
        } else {
          snprintf(col.formatted, sizeof(col.formatted), "(invalid datetime)");
        }
        break;
      }
      case FT_TIMESTAMP: {
        // InnoDB TIMESTAMP: 4-7 bytes
        std::string ts_str;
        if (format_innodb_timestamp(field_ptr, field_len, field.time_precision, ts_str)) {
          strncpy(col.formatted, ts_str.c_str(), sizeof(col.formatted) - 1);
          col.formatted[sizeof(col.formatted) - 1] = '\0';
        } else {
          snprintf(col.formatted, sizeof(col.formatted), "(invalid timestamp)");
        }
        break;
      }
      case FT_YEAR: {
        // InnoDB YEAR: 1 byte, offset from 1900
        if (field_len == 1) {
          unsigned int year = (field_ptr[0] == 0) ? 0 : 1900 + field_ptr[0];
          snprintf(col.formatted, sizeof(col.formatted), "%04u", year);
          col.uint_val = year;
        } else {
          snprintf(col.formatted, sizeof(col.formatted), "(invalid year)");
        }
        break;
      }
      case FT_BIT: {
        // InnoDB BIT: 1-8 bytes as unsigned integer
        if (field_len <= 8) {
          col.uint_val = extract_be_uint(field_ptr, field_len);
          snprintf(col.formatted, sizeof(col.formatted), "%llu",
                   (unsigned long long)col.uint_val);
        } else {
          // Too long - show as hex
          size_t hex_len = 0;
          for (size_t j = 0; j < field_len && hex_len < sizeof(col.formatted) - 3; j++) {
            hex_len += snprintf(col.formatted + hex_len,
                                sizeof(col.formatted) - hex_len,
                                "%02X", field_ptr[j]);
          }
        }
        break;
      }
      case FT_ENUM: {
        // InnoDB ENUM: 1-2 bytes as unsigned integer (index into enum values)
        uint64_t idx = extract_be_uint(field_ptr, field_len);
        col.uint_val = idx;
        // Try to get the string value from enum_values
        if (field.has_limits && field.limits.enum_values_count > 0 &&
            idx > 0 && idx <= (uint64_t)field.limits.enum_values_count) {
          const char* val = field.limits.enum_values[idx - 1];
          if (val) {
            strncpy(col.formatted, val, sizeof(col.formatted) - 1);
            col.formatted[sizeof(col.formatted) - 1] = '\0';
          } else {
            snprintf(col.formatted, sizeof(col.formatted), "%llu",
                     (unsigned long long)idx);
          }
        } else if (idx == 0) {
          col.formatted[0] = '\0';  // Empty string for index 0
        } else {
          snprintf(col.formatted, sizeof(col.formatted), "%llu",
                   (unsigned long long)idx);
        }
        break;
      }
      case FT_SET: {
        // InnoDB SET: 1-8 bytes as bitmask
        if (field_len > 8) {
          size_t hex_len = 0;
          for (size_t j = 0; j < field_len && hex_len < sizeof(col.formatted) - 3; j++) {
            hex_len += snprintf(col.formatted + hex_len,
                                sizeof(col.formatted) - hex_len,
                                "%02X", field_ptr[j]);
          }
          break;
        }
        uint64_t mask = extract_be_uint(field_ptr, field_len);
        col.uint_val = mask;
        // Try to build comma-separated list from set_values
        if (field.has_limits && field.limits.set_values_count > 0) {
          std::string set_str;
          for (int i = 0; i < field.limits.set_values_count && i < 64; i++) {
            if (mask & (1ULL << i)) {
              const char* val = field.limits.set_values[i];
              if (val) {
                if (!set_str.empty()) {
                  set_str += ",";
                }
                set_str += val;
              }
            }
          }
          strncpy(col.formatted, set_str.c_str(), sizeof(col.formatted) - 1);
          col.formatted[sizeof(col.formatted) - 1] = '\0';
        } else {
          snprintf(col.formatted, sizeof(col.formatted), "%llu",
                   (unsigned long long)mask);
        }
        break;
      }
      case FT_DECIMAL: {
        // InnoDB DECIMAL: binary format using bin2decimal
        // Constants: DIG_PER_DEC1=9, max precision=81, max str len=83
        int precision = field.decimal_precision;
        int scale = field.decimal_digits;
        if (precision <= 0 || scale < 0 || scale > precision) {
          // Invalid precision/scale - show as hex
          size_t hex_len = 0;
          for (size_t j = 0; j < field_len && hex_len < sizeof(col.formatted) - 3; j++) {
            hex_len += snprintf(col.formatted + hex_len,
                                sizeof(col.formatted) - hex_len,
                                "%02X", field_ptr[j]);
          }
          break;
        }
        int bin_size = decimal_bin_size(precision, scale);
        if (bin_size <= 0 || field_len < static_cast<ulint>(bin_size)) {
          // Size mismatch - show as hex
          size_t hex_len = 0;
          for (size_t j = 0; j < field_len && hex_len < sizeof(col.formatted) - 3; j++) {
            hex_len += snprintf(col.formatted + hex_len,
                                sizeof(col.formatted) - hex_len,
                                "%02X", field_ptr[j]);
          }
          break;
        }
        // Decode decimal (81/9 + 2 = 11 buffer entries should be enough)
        decimal_t dec;
        decimal_digit_t dec_buf[12];
        dec.buf = dec_buf;
        dec.len = sizeof(dec_buf) / sizeof(dec_buf[0]);
        dec.intg = precision - scale;
        dec.frac = scale;
        int err = bin2decimal(field_ptr, &dec, precision, scale, false);
        if (err & E_DEC_FATAL_ERROR) {
          snprintf(col.formatted, sizeof(col.formatted), "(invalid decimal)");
          break;
        }
        // Convert to string (max 83 chars + null)
        char dec_str[100];
        int out_len = sizeof(dec_str) - 1;
        err = decimal2string(&dec, dec_str, &out_len);
        if (err & E_DEC_FATAL_ERROR || out_len <= 0) {
          snprintf(col.formatted, sizeof(col.formatted), "(invalid decimal)");
          break;
        }
        dec_str[out_len] = '\0';
        strncpy(col.formatted, dec_str, sizeof(col.formatted) - 1);
        col.formatted[sizeof(col.formatted) - 1] = '\0';
        break;
      }
      case FT_INTERNAL: {
        // Internal columns (DB_TRX_ID, DB_ROLL_PTR)
        col.uint_val = extract_be_uint(field_ptr, field_len);
        snprintf(col.formatted, sizeof(col.formatted), "%llu",
                 (unsigned long long)col.uint_val);
        break;
      }
      default: {
        // Binary/blob/other: hex representation
        size_t hex_len = 0;
        for (size_t j = 0; j < field_len && hex_len < sizeof(col.formatted) - 3; j++) {
          hex_len += snprintf(col.formatted + hex_len,
                              sizeof(col.formatted) - hex_len,
                              "%02X", field_ptr[j]);
        }
        break;
      }
    }

    out_row->column_count++;
  }

  return true;
}

int parse_records_with_callback(const unsigned char* page,
                                size_t page_size,
                                uint64_t page_no,
                                table_def_t* table,
                                const parser_context_t* ctx,
                                record_callback_t callback,
                                void* user_data) {
  if (!page || !table || !callback) {
    return 0;
  }

  // 1) Check if this page belongs to the selected index
  if (!is_target_index(page, ctx)) {
    return 0;
  }

  // 2) Check if it's a LEAF page
  ulint page_level = mach_read_from_2(page + PAGE_HEADER + PAGE_LEVEL);
  if (page_level != 0) {
    return 0;  // Non-leaf
  }

  // 3) Check if COMPACT format
  bool is_compact = page_is_comp(page);
  if (!is_compact) {
    return 0;  // REDUNDANT not supported
  }

  // 4) Loop through records
  const ulint inf_offset = PAGE_NEW_INFIMUM;
  int valid_records = 0;
  ulint rec_offset = inf_offset;
  ulint steps = 0;
  ulint max_steps = static_cast<ulint>(page_size / (REC_N_NEW_EXTRA_BYTES + 1));
  const ulint n_recs = mach_read_from_2(page + PAGE_HEADER + PAGE_N_RECS);
  if (max_steps < n_recs + 2) {
    max_steps = n_recs + 2;
  }

  while (steps < max_steps) {
    const rec_t* rec = reinterpret_cast<const rec_t*>(page + rec_offset);
    const ulint status = rec_get_status(rec);
    if (status == REC_STATUS_SUPREMUM) {
      break;
    }

    if (status == REC_STATUS_ORDINARY) {
      const bool deleted = rec_get_deleted_flag(rec, true);

      // Validate record
      ulint offsets[MAX_TABLE_FIELDS + 2];
      bool valid = check_for_a_record((page_t*)page, (rec_t*)rec, table, offsets);

      if (valid) {
        parsed_row_t row;
        if (extract_record_data((page_t*)page, rec, table, offsets,
                                page_no, rec_offset, deleted, &row)) {
          // Call the callback
          if (!callback(&row, user_data)) {
            // Callback returned false - stop iteration
            return valid_records;
          }
          valid_records++;
        }
      }
    }

    ulint next_off = 0;
    if (!next_compact_rec_offset((const page_t*)page, rec_offset, page_size, &next_off)) {
      break;
    }
    if (next_off == rec_offset || next_off >= page_size) {
      break;
    }
    rec_offset = next_off;
    steps++;
  }

  return valid_records;
}
