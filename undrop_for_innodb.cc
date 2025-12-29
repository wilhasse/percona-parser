/**
 * undrop_for_innodb.cc
 *
 * Code extracted from undrop_for_innodb.cc.
 *
 * A minimal "undrop" style code that:
 *   - avoids MySQL rec_get_nth_field(),
 *   - uses a local "my_rec_get_nth_field()" for retrieving column data,
 *   - prints a simple pipe-separated row of data,
 *   - prints column headers only once.
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include <zlib.h>

// MySQL includes
#include "fil0fil.h"
#include "fil0types.h"
#include "btr0types.h"
#include "lob0lob.h"
#include "page/zipdecompress.h"
#include "tables_dict.h"
#include "univ.i"
#include "page0page.h"
#include "rem0rec.h"
#include "parser.h"
#include "undrop_for_innodb.h"
#include "my_time.h"
#include "my_sys.h"
#include "my_byteorder.h"
#include "m_ctype.h"
#include "decimal.h"

// Undefine MySQL rec_offs_* if they exist
#ifdef rec_offs_nth_size
#undef rec_offs_nth_size
#endif
#ifdef rec_offs_nth_extern
#undef rec_offs_nth_extern
#endif
#ifdef rec_offs_n_fields
#undef rec_offs_n_fields
#endif
#ifdef rec_offs_data_size
#undef rec_offs_data_size
#endif
#ifdef rec_offs_base
#undef rec_offs_base
#endif
#ifdef rec_offs_set_n_fields
#undef rec_offs_set_n_fields
#endif

#ifndef UNIV_SQL_NULL
#define UNIV_SQL_NULL 0xFFFFFFFF
#endif
#ifndef REC_OFFS_SQL_NULL
#define REC_OFFS_SQL_NULL     0x80000000UL
#endif
#ifndef REC_OFFS_EXTERNAL
#define REC_OFFS_EXTERNAL     0x40000000UL
#endif

/** Our local "undrop" offset array format:
  offsets[0] = #fields
  offsets[i+1] = offset bits for field i
*/

/** my_rec_offs_n_fields() => number of fields. */
inline ulint my_rec_offs_n_fields(const ulint* offsets) {
  return offsets[0];
}

/** my_rec_offs_set_n_fields() => sets #fields. */
inline void my_rec_offs_set_n_fields(ulint* offsets, ulint n) {
  offsets[0] = n;
}

/** my_rec_offs_nth_size() => returns length for i-th field. */
inline ulint my_rec_offs_nth_size(const ulint* offsets, ulint i) {
  ulint end   = (offsets[i+1] & ~(REC_OFFS_SQL_NULL | REC_OFFS_EXTERNAL));
  ulint start = 0;
  if (i > 0) {
    start = (offsets[i] & ~(REC_OFFS_SQL_NULL | REC_OFFS_EXTERNAL));
  }
  if (end < start) {
    return UNIV_SQL_NULL;
  }
  return (end - start);
}

/** my_rec_offs_nth_extern() => checks EXTERNAL bit. */
inline bool my_rec_offs_nth_extern(const ulint* offsets, ulint i) {
  return (offsets[i+1] & REC_OFFS_EXTERNAL) != 0;
}

/** my_rec_offs_data_size() => basic row data size. */
inline ulint my_rec_offs_data_size(const ulint* offsets) {
  ulint n = offsets[0];
  // For example, end=offsets[n], start=offsets[1]
  if (n == 0) {
    return 0;
  }
  ulint end   = (offsets[n] & ~(REC_OFFS_SQL_NULL | REC_OFFS_EXTERNAL));
  return end;
}

/** my_rec_get_nth_field() => returns pointer and length for i-th field. */
inline const unsigned char*
my_rec_get_nth_field(const rec_t* rec, const ulint* offsets,
                     ulint i, ulint* len)
{
  ulint end_bits   = offsets[i+1];
  ulint start_bits = (i == 0) ? 0 : offsets[i];

  bool is_null    = (end_bits   & REC_OFFS_SQL_NULL) != 0;
  bool is_extern  = (end_bits   & REC_OFFS_EXTERNAL) != 0;
  (void)is_extern; // if we don't handle external data

  ulint end   = end_bits   & ~(REC_OFFS_SQL_NULL | REC_OFFS_EXTERNAL);
  ulint start = start_bits & ~(REC_OFFS_SQL_NULL | REC_OFFS_EXTERNAL);

  if (is_null) {
    *len = UNIV_SQL_NULL;
  } else {
    *len = (end >= start) ? (end - start) : 0;
  }

  return (const unsigned char*)rec + start;
}

/** check_fields_sizes() => minimal check for each field. */
inline bool check_fields_sizes(const rec_t* rec, table_def_t* table, ulint* offsets)
{
  // remove "ulint n = offsets[0]" since we don't use it
  // remove "field_ptr" or cast to (void) if not used

  for (ulint i = 0; i < (ulint)table->fields_count; i++) {
    ulint field_len;
    (void)my_rec_get_nth_field(rec, offsets, i, &field_len); // if we don't actually need 'field_ptr'

    // Check range
    if (field_len != UNIV_SQL_NULL) {
      ulint minL = (ulint)table->fields[i].min_length;
      ulint maxL = (ulint)table->fields[i].max_length;
      if (field_len < minL || field_len > maxL) {
        printf("ERROR: field #%lu => length %lu out of [%u..%u]\n",
               (unsigned long)i, (unsigned long)field_len,
               table->fields[i].min_length, table->fields[i].max_length);
        return false;
      }
    }
  }
  return true;
}

/** ibrec_init_offsets_new() => fill offsets array for a COMPACT record. */
inline bool ibrec_init_offsets_new(const page_t* page,
                                   const rec_t* rec,
                                   table_def_t* table,
                                   ulint* offsets)
{
  ulint status = rec_get_status((rec_t*)rec);
  if (status != REC_STATUS_ORDINARY) {
    return false;
  }
  // set #fields
  my_rec_offs_set_n_fields(offsets, (ulint)table->fields_count);

  const unsigned char* nulls = (const unsigned char*)rec - (REC_N_NEW_EXTRA_BYTES + 1);
  ulint info_bits = rec_get_info_bits(rec, true);
  if (info_bits & REC_INFO_VERSION_FLAG) {
    nulls -= 1;
  } else if (info_bits & REC_INFO_INSTANT_FLAG) {
    uint16_t len = 1;
    if ((*nulls & REC_N_FIELDS_TWO_BYTES_FLAG) != 0) {
      len = 2;
    }
    nulls -= len;
  }
  const unsigned char* lens  = nulls - ((table->n_nullable + 7) / 8);

  ulint offs = 0;
  ulint null_mask = 1;

  for (ulint i = 0; i < (ulint)table->fields_count; i++) {
    field_def_t* fld = &table->fields[i];
    bool is_null = false;

    if (fld->can_be_null) {
      if (null_mask == 0) {
        nulls--;
        null_mask = 1;
      }
      if ((*nulls & null_mask) != 0) {
        is_null = true;
      }
      null_mask <<= 1;
    }

    ulint len_val;
    if (is_null) {
      len_val = offs | REC_OFFS_SQL_NULL;
    } else {
      if (fld->fixed_length == 0) {
        ulint lenbyte = *lens--;
        if (fld->max_length > 255
            || fld->type == FT_BLOB
            || fld->type == FT_TEXT) {
          if (lenbyte & 0x80) {
            lenbyte <<= 8;
            lenbyte |= *lens--;
            offs += (lenbyte & 0x3fff);
            if (lenbyte & 0x4000) {
              len_val = offs | REC_OFFS_EXTERNAL;
              goto store_len;
            } else {
              len_val = offs;
              goto store_len;
            }
          }
        }
        offs += lenbyte;
        len_val = offs;
      } else {
        offs += (ulint)fld->fixed_length;
        len_val = offs;
      }
    }
store_len:
    offs &= 0xffff;
    ulint diff = (ulint)((const unsigned char*)rec + offs - (const unsigned char*)page);
    if (diff > (ulint)UNIV_PAGE_SIZE) {
      printf("Invalid offset => field %lu => %lu\n",
             (unsigned long)i, (unsigned long)offs);
      return false;
    }
    offsets[i+1] = len_val;
  }
  return true;
}

/** check_for_a_record() => basic validity check. */
bool check_for_a_record(page_t *page, rec_t *rec, table_def_t *table, ulint *offsets)
{
  ulint offset_in_page = (ulint)((const unsigned char*)rec - (const unsigned char*)page);
  ulint min_hdr = (ulint)(table->min_rec_header_len + record_extra_bytes);
  if (offset_in_page < min_hdr) {
    return false;
  }

  if (!ibrec_init_offsets_new(page, rec, table, offsets)) {
    return false;
  }

  ulint data_sz = my_rec_offs_data_size(offsets);
  if (data_sz > (ulint)table->data_max_size) {
    printf("DATA_SIZE=FAIL(%lu > %ld)\n",
           (unsigned long)data_sz, (long)table->data_max_size);
    return false;
  }
  if (data_sz < (ulint)table->data_min_size) {
    printf("DATA_SIZE=FAIL(%lu < %d)\n",
           (unsigned long)data_sz, table->data_min_size);
    return false;
  }

  if (!check_fields_sizes(rec, table, offsets)) {
    return false;
  }

  return true;
}

// global so we can print header once
static bool g_printed_header = false;
static RowOutputOptions g_row_output;
static LobReadContext g_lob_ctx;

void set_row_output_options(const RowOutputOptions& opts) {
  g_row_output = opts;
  g_printed_header = false;
}

void set_lob_read_context(const LobReadContext& ctx) {
  g_lob_ctx = ctx;
}

static FILE* output_stream() {
  return g_row_output.out ? g_row_output.out : stdout;
}

static uint64_t read_be_uint(const unsigned char* ptr, size_t len) {
  uint64_t val = 0;
  for (size_t i = 0; i < len; i++) {
    val = (val << 8) | ptr[i];
  }
  return val;
}

static int64_t read_be_int_signed(const unsigned char* ptr, size_t len) {
  if (len == 0 || len > 8) {
    return 0;
  }
  uint64_t val = read_be_uint(ptr, len);
  uint64_t sign_mask = 1ULL << (len * 8 - 1);
  val ^= sign_mask;
  if (val & sign_mask && len < 8) {
    uint64_t mask = ~0ULL << (len * 8);
    val |= mask;
  }
  return static_cast<int64_t>(val);
}

constexpr ulint LOB_FLST_BASE_NODE_SIZE = 4 + 2 * FIL_ADDR_SIZE;

constexpr ulint LOB_FIRST_OFFSET_VERSION = FIL_PAGE_DATA;
constexpr ulint LOB_FIRST_OFFSET_FLAGS = LOB_FIRST_OFFSET_VERSION + 1;
constexpr ulint LOB_FIRST_OFFSET_LOB_VERSION = LOB_FIRST_OFFSET_FLAGS + 1;
constexpr ulint LOB_FIRST_OFFSET_LAST_TRX_ID = LOB_FIRST_OFFSET_LOB_VERSION + 4;
constexpr ulint LOB_FIRST_OFFSET_LAST_UNDO_NO = LOB_FIRST_OFFSET_LAST_TRX_ID + 6;
constexpr ulint LOB_FIRST_OFFSET_DATA_LEN = LOB_FIRST_OFFSET_LAST_UNDO_NO + 4;
constexpr ulint LOB_FIRST_OFFSET_TRX_ID = LOB_FIRST_OFFSET_DATA_LEN + 4;
constexpr ulint LOB_FIRST_OFFSET_INDEX_LIST = LOB_FIRST_OFFSET_TRX_ID + 6;
constexpr ulint LOB_FIRST_OFFSET_INDEX_FREE_NODES =
    LOB_FIRST_OFFSET_INDEX_LIST + LOB_FLST_BASE_NODE_SIZE;
constexpr ulint LOB_FIRST_DATA = LOB_FIRST_OFFSET_INDEX_FREE_NODES + LOB_FLST_BASE_NODE_SIZE;

constexpr ulint LOB_DATA_OFFSET_VERSION = FIL_PAGE_DATA;
constexpr ulint LOB_DATA_OFFSET_DATA_LEN = LOB_DATA_OFFSET_VERSION + 1;
constexpr ulint LOB_DATA_OFFSET_TRX_ID = LOB_DATA_OFFSET_DATA_LEN + 4;
constexpr ulint LOB_DATA_DATA = LOB_DATA_OFFSET_TRX_ID + 6;

constexpr ulint LOB_INDEX_ENTRY_OFFSET_PREV = 0;
constexpr ulint LOB_INDEX_ENTRY_OFFSET_NEXT = FIL_ADDR_SIZE;
constexpr ulint LOB_INDEX_ENTRY_OFFSET_VERSIONS =
    LOB_INDEX_ENTRY_OFFSET_NEXT + FIL_ADDR_SIZE;
constexpr ulint LOB_INDEX_ENTRY_OFFSET_TRXID =
    LOB_INDEX_ENTRY_OFFSET_VERSIONS + LOB_FLST_BASE_NODE_SIZE;
constexpr ulint LOB_INDEX_ENTRY_OFFSET_TRXID_MODIFIER =
    LOB_INDEX_ENTRY_OFFSET_TRXID + 6;
constexpr ulint LOB_INDEX_ENTRY_OFFSET_TRX_UNDO_NO =
    LOB_INDEX_ENTRY_OFFSET_TRXID_MODIFIER + 6;
constexpr ulint LOB_INDEX_ENTRY_OFFSET_TRX_UNDO_NO_MODIFIER =
    LOB_INDEX_ENTRY_OFFSET_TRX_UNDO_NO + 4;
constexpr ulint LOB_INDEX_ENTRY_OFFSET_PAGE_NO =
    LOB_INDEX_ENTRY_OFFSET_TRX_UNDO_NO_MODIFIER + 4;
constexpr ulint LOB_INDEX_ENTRY_OFFSET_DATA_LEN =
    LOB_INDEX_ENTRY_OFFSET_PAGE_NO + 4;
constexpr ulint LOB_INDEX_ENTRY_OFFSET_LOB_VERSION =
    LOB_INDEX_ENTRY_OFFSET_DATA_LEN + 4;
constexpr ulint LOB_INDEX_ENTRY_SIZE =
    LOB_INDEX_ENTRY_OFFSET_LOB_VERSION + 4;
constexpr ulint LOB_FIRST_INDEX_COUNT = 10;
constexpr ulint LOB_FIRST_INDEX_ARRAY_SIZE =
    LOB_FIRST_INDEX_COUNT * LOB_INDEX_ENTRY_SIZE;
constexpr ulint LOB_FIRST_DATA_BEGIN = LOB_FIRST_DATA + LOB_FIRST_INDEX_ARRAY_SIZE;

constexpr ulint ZLOB_FIRST_OFFSET_VERSION = FIL_PAGE_DATA;
constexpr ulint ZLOB_FIRST_OFFSET_FLAGS = ZLOB_FIRST_OFFSET_VERSION + 1;
constexpr ulint ZLOB_FIRST_OFFSET_LOB_VERSION = ZLOB_FIRST_OFFSET_FLAGS + 1;
constexpr ulint ZLOB_FIRST_OFFSET_LAST_TRX_ID = ZLOB_FIRST_OFFSET_LOB_VERSION + 4;
constexpr ulint ZLOB_FIRST_OFFSET_LAST_UNDO_NO = ZLOB_FIRST_OFFSET_LAST_TRX_ID + 6;
constexpr ulint ZLOB_FIRST_OFFSET_DATA_LEN = ZLOB_FIRST_OFFSET_LAST_UNDO_NO + 4;
constexpr ulint ZLOB_FIRST_OFFSET_TRX_ID = ZLOB_FIRST_OFFSET_DATA_LEN + 4;
constexpr ulint ZLOB_FIRST_OFFSET_INDEX_PAGE_NO = ZLOB_FIRST_OFFSET_TRX_ID + 6;
constexpr ulint ZLOB_FIRST_OFFSET_FRAG_NODES_PAGE_NO =
    ZLOB_FIRST_OFFSET_INDEX_PAGE_NO + 4;
constexpr ulint ZLOB_FIRST_OFFSET_FREE_LIST =
    ZLOB_FIRST_OFFSET_FRAG_NODES_PAGE_NO + 4;
constexpr ulint ZLOB_FIRST_OFFSET_INDEX_LIST =
    ZLOB_FIRST_OFFSET_FREE_LIST + LOB_FLST_BASE_NODE_SIZE;
constexpr ulint ZLOB_FIRST_OFFSET_FREE_FRAG_LIST =
    ZLOB_FIRST_OFFSET_INDEX_LIST + LOB_FLST_BASE_NODE_SIZE;
constexpr ulint ZLOB_FIRST_OFFSET_FRAG_LIST =
    ZLOB_FIRST_OFFSET_FREE_FRAG_LIST + LOB_FLST_BASE_NODE_SIZE;
constexpr ulint ZLOB_FIRST_OFFSET_INDEX_BEGIN =
    ZLOB_FIRST_OFFSET_FRAG_LIST + LOB_FLST_BASE_NODE_SIZE;

constexpr ulint ZLOB_DATA_OFFSET_VERSION = FIL_PAGE_DATA;
constexpr ulint ZLOB_DATA_OFFSET_DATA_LEN = ZLOB_DATA_OFFSET_VERSION + 1;
constexpr ulint ZLOB_DATA_OFFSET_TRX_ID = ZLOB_DATA_OFFSET_DATA_LEN + 4;
constexpr ulint ZLOB_DATA_OFFSET_DATA_BEGIN = ZLOB_DATA_OFFSET_TRX_ID + 6;

constexpr ulint ZLOB_INDEX_ENTRY_OFFSET_PREV = 0;
constexpr ulint ZLOB_INDEX_ENTRY_OFFSET_NEXT = FIL_ADDR_SIZE;
constexpr ulint ZLOB_INDEX_ENTRY_OFFSET_VERSIONS =
    ZLOB_INDEX_ENTRY_OFFSET_NEXT + FIL_ADDR_SIZE;
constexpr ulint ZLOB_INDEX_ENTRY_OFFSET_TRXID =
    ZLOB_INDEX_ENTRY_OFFSET_VERSIONS + LOB_FLST_BASE_NODE_SIZE;
constexpr ulint ZLOB_INDEX_ENTRY_OFFSET_TRXID_MODIFIER =
    ZLOB_INDEX_ENTRY_OFFSET_TRXID + 6;
constexpr ulint ZLOB_INDEX_ENTRY_OFFSET_TRX_UNDO_NO =
    ZLOB_INDEX_ENTRY_OFFSET_TRXID_MODIFIER + 6;
constexpr ulint ZLOB_INDEX_ENTRY_OFFSET_TRX_UNDO_NO_MODIFIER =
    ZLOB_INDEX_ENTRY_OFFSET_TRX_UNDO_NO + 4;
constexpr ulint ZLOB_INDEX_ENTRY_OFFSET_Z_PAGE_NO =
    ZLOB_INDEX_ENTRY_OFFSET_TRX_UNDO_NO_MODIFIER + 4;
constexpr ulint ZLOB_INDEX_ENTRY_OFFSET_Z_FRAG_ID =
    ZLOB_INDEX_ENTRY_OFFSET_Z_PAGE_NO + 4;
constexpr ulint ZLOB_INDEX_ENTRY_OFFSET_DATA_LEN =
    ZLOB_INDEX_ENTRY_OFFSET_Z_FRAG_ID + 2;
constexpr ulint ZLOB_INDEX_ENTRY_OFFSET_ZDATA_LEN =
    ZLOB_INDEX_ENTRY_OFFSET_DATA_LEN + 4;
constexpr ulint ZLOB_INDEX_ENTRY_OFFSET_LOB_VERSION =
    ZLOB_INDEX_ENTRY_OFFSET_ZDATA_LEN + 4;
constexpr ulint ZLOB_INDEX_ENTRY_SIZE =
    ZLOB_INDEX_ENTRY_OFFSET_LOB_VERSION + 4;

constexpr ulint ZLOB_FRAG_ENTRY_OFFSET_PREV = 0;
constexpr ulint ZLOB_FRAG_ENTRY_OFFSET_NEXT = ZLOB_FRAG_ENTRY_OFFSET_PREV + FIL_ADDR_SIZE;
constexpr ulint ZLOB_FRAG_ENTRY_OFFSET_PAGE_NO = ZLOB_FRAG_ENTRY_OFFSET_NEXT + FIL_ADDR_SIZE;
constexpr ulint ZLOB_FRAG_ENTRY_OFFSET_N_FRAGS = ZLOB_FRAG_ENTRY_OFFSET_PAGE_NO + 4;
constexpr ulint ZLOB_FRAG_ENTRY_OFFSET_USED_LEN = ZLOB_FRAG_ENTRY_OFFSET_N_FRAGS + 2;
constexpr ulint ZLOB_FRAG_ENTRY_OFFSET_TOTAL_FREE_LEN =
    ZLOB_FRAG_ENTRY_OFFSET_USED_LEN + 2;
constexpr ulint ZLOB_FRAG_ENTRY_OFFSET_BIG_FREE_LEN =
    ZLOB_FRAG_ENTRY_OFFSET_TOTAL_FREE_LEN + 2;
constexpr ulint ZLOB_FRAG_ENTRY_SIZE = ZLOB_FRAG_ENTRY_OFFSET_BIG_FREE_LEN + 2;

constexpr ulint ZLOB_FRAG_PAGE_OFFSET_PAGE_DIR_ENTRY_COUNT = FIL_PAGE_DATA_END + 2;
constexpr ulint ZLOB_FRAG_PAGE_OFFSET_PAGE_DIR_ENTRY_FIRST =
    ZLOB_FRAG_PAGE_OFFSET_PAGE_DIR_ENTRY_COUNT + 2;
constexpr ulint ZLOB_FRAG_PAGE_DIR_ENTRY_SIZE = 2;

constexpr ulint ZLOB_PLIST_NODE_SIZE = 4;
constexpr ulint ZLOB_FRAG_NODE_OFFSET_LEN = ZLOB_PLIST_NODE_SIZE;
constexpr ulint ZLOB_FRAG_NODE_OFFSET_FRAG_ID = ZLOB_FRAG_NODE_OFFSET_LEN + 2;
constexpr ulint ZLOB_FRAG_NODE_OFFSET_DATA = ZLOB_FRAG_NODE_OFFSET_FRAG_ID + 2;
constexpr ulint ZLOB_FRAG_NODE_HEADER_SIZE = ZLOB_FRAG_NODE_OFFSET_DATA;
constexpr uint16_t ZLOB_FRAG_ID_NULL = 0xFFFF;

struct LobRef {
  space_id_t space_id = 0;
  page_no_t page_no = FIL_NULL;
  uint32_t offset = 0;
  uint32_t version = 0;
  uint32_t length = 0;
  bool being_modified = false;
};

struct ZlobIndexEntry {
  fil_addr_t next;
  fil_addr_t versions_first;
  page_no_t z_page_no = FIL_NULL;
  uint16_t z_frag_id = ZLOB_FRAG_ID_NULL;
  uint32_t data_len = 0;
  uint32_t zdata_len = 0;
  uint32_t lob_version = 0;
};

static fil_addr_t read_fil_addr(const unsigned char* ptr) {
  fil_addr_t addr;
  addr.page = mach_read_from_4(ptr + FIL_ADDR_PAGE);
  addr.boffset = mach_read_from_2(ptr + FIL_ADDR_BYTE);
  return addr;
}

static uint32_t page_size_to_ssize_local(size_t page_size) {
  uint32_t ssize;
  for (ssize = UNIV_ZIP_SIZE_SHIFT_MIN;
       static_cast<uint32_t>(1U << ssize) < page_size; ssize++) {
  }
  return (ssize - UNIV_ZIP_SIZE_SHIFT_MIN + 1);
}

static unsigned char* align_ptr(unsigned char* ptr, size_t align) {
  const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
  const uintptr_t aligned = (p + align - 1) & ~(align - 1);
  return reinterpret_cast<unsigned char*>(aligned);
}

static bool should_decompress_lob_page(uint16_t page_type) {
  return page_type == FIL_PAGE_TYPE_ZLOB_FIRST ||
         page_type == FIL_PAGE_TYPE_ZLOB_DATA ||
         page_type == FIL_PAGE_TYPE_ZLOB_INDEX ||
         page_type == FIL_PAGE_TYPE_ZLOB_FRAG ||
         page_type == FIL_PAGE_TYPE_ZLOB_FRAG_ENTRY;
}

static bool decompress_zip_page(const unsigned char* src,
                                std::vector<unsigned char>& buf) {
  const size_t logical = g_lob_ctx.logical_page_size;
  if (logical == 0 || buf.size() < logical) {
    buf.resize(logical);
  }

  std::vector<unsigned char> temp(logical * 2);
  unsigned char* aligned = align_ptr(temp.data(), logical);
  std::memset(aligned, 0, logical);

  page_zip_des_t page_zip;
  std::memset(&page_zip, 0, sizeof(page_zip));
  page_zip.data = reinterpret_cast<page_zip_t*>(const_cast<unsigned char*>(src));
  page_zip.ssize = page_size_to_ssize_local(g_lob_ctx.physical_page_size);

  if (!page_zip_decompress_low(&page_zip, aligned, true)) {
    return false;
  }
  std::memcpy(buf.data(), aligned, logical);
  return true;
}

static bool read_tablespace_page_raw(page_no_t page_no,
                                     std::vector<unsigned char>& buf) {
  if (g_lob_ctx.fd < 0 || g_lob_ctx.physical_page_size == 0) {
    return false;
  }
  const size_t physical = g_lob_ctx.physical_page_size;
  if (buf.size() < physical) {
    buf.resize(physical);
  }
  const off_t offset =
      static_cast<off_t>(page_no) * static_cast<off_t>(physical);
  const ssize_t rd = pread(g_lob_ctx.fd, buf.data(), physical, offset);
  return rd == static_cast<ssize_t>(physical);
}

static ulint zlob_first_index_entries(size_t physical_size) {
  switch (physical_size) {
    case 16384:
      return 100;
    case 8192:
      return 80;
    case 4096:
      return 40;
    case 2048:
      return 20;
    case 1024:
      return 5;
    default:
      return 0;
  }
}

static ulint zlob_first_frag_entries(size_t physical_size) {
  switch (physical_size) {
    case 16384:
      return 200;
    case 8192:
      return 100;
    case 4096:
      return 40;
    case 2048:
      return 20;
    case 1024:
      return 5;
    default:
      return 0;
  }
}

static ulint zlob_first_data_begin(size_t physical_size) {
  const ulint index_entries = zlob_first_index_entries(physical_size);
  const ulint frag_entries = zlob_first_frag_entries(physical_size);
  return ZLOB_FIRST_OFFSET_INDEX_BEGIN +
         index_entries * ZLOB_INDEX_ENTRY_SIZE +
         frag_entries * ZLOB_FRAG_ENTRY_SIZE;
}

static bool read_tablespace_page(page_no_t page_no,
                                 std::vector<unsigned char>& buf) {
  if (g_lob_ctx.fd < 0 || g_lob_ctx.physical_page_size == 0) {
    return false;
  }
  const size_t physical = g_lob_ctx.physical_page_size;
  const size_t logical = g_lob_ctx.tablespace_compressed
                             ? g_lob_ctx.logical_page_size
                             : g_lob_ctx.physical_page_size;

  if (buf.size() < logical) {
    buf.resize(logical);
  }

  const off_t offset =
      static_cast<off_t>(page_no) * static_cast<off_t>(physical);

  if (!g_lob_ctx.tablespace_compressed) {
    const ssize_t rd = pread(g_lob_ctx.fd, buf.data(), physical, offset);
    return rd == static_cast<ssize_t>(physical);
  }

  std::vector<unsigned char> phys_buf(physical);
  if (!read_tablespace_page_raw(page_no, phys_buf)) {
    return false;
  }

  const uint16_t page_type = mach_read_from_2(phys_buf.data() + FIL_PAGE_TYPE);
  if (!should_decompress_lob_page(page_type)) {
    std::memcpy(buf.data(), phys_buf.data(), physical);
    return true;
  }

  return decompress_zip_page(phys_buf.data(), buf);
}

static size_t clamp_page_copy(size_t page_size, size_t start, size_t want) {
  if (start >= page_size) {
    return 0;
  }
  size_t avail = page_size - start;
  return want < avail ? want : avail;
}

static size_t read_lob_old_chain(const LobRef& ref,
                                 size_t want,
                                 std::string& out) {
  if (want == 0 || ref.page_no == FIL_NULL) {
    return 0;
  }
  std::vector<unsigned char> page_buf(g_lob_ctx.physical_page_size);
  page_no_t page_no = ref.page_no;
  ulint offset = ref.offset;
  size_t remaining = want;
  size_t total = 0;
  size_t steps = 0;
  const size_t max_steps = 100000;

  if (offset < FIL_PAGE_DATA || offset >= g_lob_ctx.physical_page_size) {
    offset = FIL_PAGE_DATA;
  }

  while (page_no != FIL_NULL && remaining > 0 && steps++ < max_steps) {
    if (!read_tablespace_page(page_no, page_buf)) {
      break;
    }
    const uint16_t page_type = mach_read_from_2(page_buf.data() + FIL_PAGE_TYPE);
    if (page_type != FIL_PAGE_TYPE_BLOB &&
        page_type != FIL_PAGE_SDI_BLOB) {
      break;
    }
    if (offset + lob::LOB_HDR_SIZE > g_lob_ctx.physical_page_size) {
      break;
    }
    const unsigned char* header = page_buf.data() + offset;
    ulint part_len = mach_read_from_4(header + lob::LOB_HDR_PART_LEN);
    page_no_t next_page = mach_read_from_4(header + lob::LOB_HDR_NEXT_PAGE_NO);

    size_t copy_len = part_len;
    if (copy_len > remaining) {
      copy_len = remaining;
    }
    copy_len = clamp_page_copy(g_lob_ctx.physical_page_size,
                               offset + lob::LOB_HDR_SIZE, copy_len);
    if (copy_len == 0) {
      break;
    }
    out.append(reinterpret_cast<const char*>(header + lob::LOB_HDR_SIZE), copy_len);
    total += copy_len;
    remaining -= copy_len;

    if (copy_len < static_cast<size_t>(part_len)) {
      break;
    }
    page_no = next_page;
    offset = FIL_PAGE_DATA;
  }

  return total;
}

static size_t read_lob_first_page(const unsigned char* page,
                                  size_t want,
                                  std::string& out) {
  const uint32_t data_len = mach_read_from_4(page + LOB_FIRST_OFFSET_DATA_LEN);
  const size_t max_data =
      clamp_page_copy(g_lob_ctx.physical_page_size, LOB_FIRST_DATA_BEGIN, data_len);
  size_t copy_len = want < max_data ? want : max_data;
  if (copy_len == 0) {
    return 0;
  }
  out.append(reinterpret_cast<const char*>(page + LOB_FIRST_DATA_BEGIN), copy_len);
  return copy_len;
}

static size_t read_lob_data_page(const unsigned char* page,
                                 size_t want,
                                 std::string& out) {
  const uint32_t data_len = mach_read_from_4(page + LOB_DATA_OFFSET_DATA_LEN);
  const size_t max_data =
      clamp_page_copy(g_lob_ctx.physical_page_size, LOB_DATA_DATA, data_len);
  size_t copy_len = want < max_data ? want : max_data;
  if (copy_len == 0) {
    return 0;
  }
  out.append(reinterpret_cast<const char*>(page + LOB_DATA_DATA), copy_len);
  return copy_len;
}

static size_t read_lob_new_format(const LobRef& ref,
                                  size_t want,
                                  std::string& out) {
  if (want == 0 || ref.page_no == FIL_NULL) {
    return 0;
  }
  std::vector<unsigned char> first_page(g_lob_ctx.physical_page_size);
  if (!read_tablespace_page(ref.page_no, first_page)) {
    return 0;
  }
  const uint16_t page_type = mach_read_from_2(first_page.data() + FIL_PAGE_TYPE);
  if (page_type != FIL_PAGE_TYPE_LOB_FIRST) {
    return 0;
  }

  const unsigned char* base = first_page.data() + LOB_FIRST_OFFSET_INDEX_LIST;
  fil_addr_t addr = read_fil_addr(base + 4);
  size_t remaining = want;
  size_t total = 0;
  size_t steps = 0;
  const size_t max_steps = 100000;

  std::vector<unsigned char> index_buf(g_lob_ctx.physical_page_size);
  std::vector<unsigned char> data_buf(g_lob_ctx.physical_page_size);

  while (!addr.is_null() && remaining > 0 && steps++ < max_steps) {
    if (!read_tablespace_page(addr.page, index_buf)) {
      break;
    }
    if (addr.boffset + LOB_INDEX_ENTRY_SIZE > g_lob_ctx.physical_page_size) {
      break;
    }
    const unsigned char* node = index_buf.data() + addr.boffset;
    fil_addr_t next_addr = read_fil_addr(node + LOB_INDEX_ENTRY_OFFSET_NEXT);
    const uint32_t entry_version =
        mach_read_from_4(node + LOB_INDEX_ENTRY_OFFSET_LOB_VERSION);
    if (entry_version > ref.version) {
      addr = next_addr;
      continue;
    }

    const page_no_t data_page_no =
        mach_read_from_4(node + LOB_INDEX_ENTRY_OFFSET_PAGE_NO);
    if (data_page_no == FIL_NULL) {
      addr = next_addr;
      continue;
    }

    size_t copied = 0;
    if (data_page_no == ref.page_no) {
      copied = read_lob_first_page(first_page.data(), remaining, out);
    } else if (read_tablespace_page(data_page_no, data_buf)) {
      const uint16_t data_type = mach_read_from_2(data_buf.data() + FIL_PAGE_TYPE);
      if (data_type == FIL_PAGE_TYPE_LOB_DATA) {
        copied = read_lob_data_page(data_buf.data(), remaining, out);
      }
    }

    total += copied;
    remaining -= copied;
    if (copied == 0) {
      break;
    }

    addr = next_addr;
  }

  return total;
}

static bool read_zlob_index_entry(const unsigned char* node,
                                  ZlobIndexEntry& entry) {
  entry.next = read_fil_addr(node + ZLOB_INDEX_ENTRY_OFFSET_NEXT);
  entry.versions_first =
      read_fil_addr(node + ZLOB_INDEX_ENTRY_OFFSET_VERSIONS + 4);
  entry.z_page_no = mach_read_from_4(node + ZLOB_INDEX_ENTRY_OFFSET_Z_PAGE_NO);
  entry.z_frag_id = mach_read_from_2(node + ZLOB_INDEX_ENTRY_OFFSET_Z_FRAG_ID);
  entry.data_len = mach_read_from_4(node + ZLOB_INDEX_ENTRY_OFFSET_DATA_LEN);
  entry.zdata_len = mach_read_from_4(node + ZLOB_INDEX_ENTRY_OFFSET_ZDATA_LEN);
  entry.lob_version = mach_read_from_4(node + ZLOB_INDEX_ENTRY_OFFSET_LOB_VERSION);
  return true;
}

static bool read_zlob_frag_payload(const unsigned char* page,
                                   size_t physical_size,
                                   uint16_t frag_id,
                                   const unsigned char** data,
                                   size_t* data_len) {
  if (frag_id == ZLOB_FRAG_ID_NULL) {
    return false;
  }
  if (physical_size < ZLOB_FRAG_PAGE_OFFSET_PAGE_DIR_ENTRY_FIRST) {
    return false;
  }
  const unsigned char* count_ptr =
      page + physical_size - ZLOB_FRAG_PAGE_OFFSET_PAGE_DIR_ENTRY_COUNT;
  const uint16_t n_entries = mach_read_from_2(count_ptr);
  if (frag_id >= n_entries) {
    return false;
  }
  const unsigned char* first =
      page + physical_size - ZLOB_FRAG_PAGE_OFFSET_PAGE_DIR_ENTRY_FIRST;
  const unsigned char* entry_ptr =
      first - frag_id * ZLOB_FRAG_PAGE_DIR_ENTRY_SIZE;
  const uint16_t offset = mach_read_from_2(entry_ptr);
  if (offset + ZLOB_FRAG_NODE_HEADER_SIZE > physical_size) {
    return false;
  }
  const unsigned char* node = page + offset;
  const uint16_t total_len = mach_read_from_2(node + ZLOB_FRAG_NODE_OFFSET_LEN);
  if (total_len < ZLOB_FRAG_NODE_HEADER_SIZE) {
    return false;
  }
  size_t payload = static_cast<size_t>(total_len - ZLOB_FRAG_NODE_HEADER_SIZE);
  payload = clamp_page_copy(physical_size,
                            offset + ZLOB_FRAG_NODE_OFFSET_DATA,
                            payload);
  if (payload == 0) {
    return false;
  }
  *data = node + ZLOB_FRAG_NODE_OFFSET_DATA;
  *data_len = payload;
  return true;
}

static bool read_zlob_stream(const ZlobIndexEntry& entry,
                             unsigned char* buf,
                             size_t buf_size) {
  if (buf_size == 0 || entry.z_page_no == FIL_NULL) {
    return false;
  }

  size_t remaining = buf_size;
  unsigned char* ptr = buf;
  page_no_t page_no = entry.z_page_no;
  size_t steps = 0;
  const size_t max_steps = 100000;

  std::vector<unsigned char> page_buf(g_lob_ctx.logical_page_size);

  while (remaining > 0 && page_no != FIL_NULL && steps++ < max_steps) {
    if (!read_tablespace_page(page_no, page_buf)) {
      return false;
    }
    const uint16_t page_type =
        mach_read_from_2(page_buf.data() + FIL_PAGE_TYPE);

    const unsigned char* data = nullptr;
    size_t data_len = 0;

    if (page_type == FIL_PAGE_TYPE_ZLOB_FIRST) {
      const uint32_t len = mach_read_from_4(page_buf.data() +
                                            ZLOB_FIRST_OFFSET_DATA_LEN);
      const ulint begin = zlob_first_data_begin(g_lob_ctx.physical_page_size);
      data_len = clamp_page_copy(g_lob_ctx.physical_page_size, begin, len);
      data = page_buf.data() + begin;
    } else if (page_type == FIL_PAGE_TYPE_ZLOB_DATA) {
      const uint32_t len = mach_read_from_4(page_buf.data() +
                                            ZLOB_DATA_OFFSET_DATA_LEN);
      data_len = clamp_page_copy(g_lob_ctx.physical_page_size,
                                 ZLOB_DATA_OFFSET_DATA_BEGIN,
                                 len);
      data = page_buf.data() + ZLOB_DATA_OFFSET_DATA_BEGIN;
    } else if (page_type == FIL_PAGE_TYPE_ZLOB_FRAG) {
      if (!read_zlob_frag_payload(page_buf.data(),
                                  g_lob_ctx.physical_page_size,
                                  entry.z_frag_id,
                                  &data,
                                  &data_len)) {
        return false;
      }
    } else {
      return false;
    }

    if (data_len == 0) {
      return false;
    }

    const size_t copy_len = (remaining < data_len) ? remaining : data_len;
    std::memcpy(ptr, data, copy_len);
    ptr += copy_len;
    remaining -= copy_len;
    page_no = mach_read_from_4(page_buf.data() + FIL_PAGE_NEXT);
  }

  return remaining == 0;
}

static size_t read_zlob_chunk(const ZlobIndexEntry& entry,
                              size_t want,
                              std::string& out) {
  if (entry.z_page_no == FIL_NULL || entry.data_len == 0 || entry.zdata_len == 0) {
    return 0;
  }

  std::vector<unsigned char> zbuf(entry.zdata_len);
  if (!read_zlob_stream(entry, zbuf.data(), zbuf.size())) {
    return 0;
  }

  z_stream strm;
  std::memset(&strm, 0, sizeof(strm));
  if (inflateInit(&strm) != Z_OK) {
    return 0;
  }

  strm.avail_in = static_cast<uInt>(zbuf.size());
  strm.next_in = zbuf.data();

  size_t copied = 0;
  const size_t full_len = entry.data_len;
  const size_t target = (want < full_len) ? want : full_len;

  if (target == full_len) {
    const size_t out_pos = out.size();
    out.resize(out_pos + full_len);
    strm.avail_out = static_cast<uInt>(full_len);
    strm.next_out = reinterpret_cast<unsigned char*>(&out[out_pos]);
    const int ret = inflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
      inflateEnd(&strm);
      out.resize(out_pos);
      return 0;
    }
    copied = static_cast<size_t>(strm.total_out);
    out.resize(out_pos + copied);
  } else {
    std::vector<unsigned char> tmp(full_len);
    strm.avail_out = static_cast<uInt>(full_len);
    strm.next_out = tmp.data();
    const int ret = inflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
      inflateEnd(&strm);
      return 0;
    }
    const size_t produced = static_cast<size_t>(strm.total_out);
    copied = (target < produced) ? target : produced;
    out.append(reinterpret_cast<const char*>(tmp.data()), copied);
  }

  inflateEnd(&strm);
  return copied;
}

static bool read_zlob_visible_entry(const ZlobIndexEntry& current,
                                    const unsigned char* node,
                                    uint32_t ref_version,
                                    ZlobIndexEntry& out) {
  if (current.lob_version <= ref_version) {
    out = current;
    return true;
  }

  fil_addr_t addr =
      read_fil_addr(node + ZLOB_INDEX_ENTRY_OFFSET_VERSIONS + 4);
  size_t steps = 0;
  const size_t max_steps = 100000;
  std::vector<unsigned char> page_buf(g_lob_ctx.logical_page_size);

  while (!addr.is_null() && steps++ < max_steps) {
    if (!read_tablespace_page(addr.page, page_buf)) {
      break;
    }
    if (addr.boffset + ZLOB_INDEX_ENTRY_SIZE > g_lob_ctx.physical_page_size) {
      break;
    }
    const unsigned char* ver_node = page_buf.data() + addr.boffset;
    ZlobIndexEntry entry{};
    read_zlob_index_entry(ver_node, entry);
    if (entry.lob_version <= ref_version) {
      out = entry;
      return true;
    }
    addr = entry.next;
  }

  out = current;
  return true;
}

static size_t read_zlob_new_format(const LobRef& ref,
                                   size_t want,
                                   std::string& out) {
  if (want == 0 || ref.page_no == FIL_NULL) {
    return 0;
  }

  std::vector<unsigned char> first_page(g_lob_ctx.logical_page_size);
  if (!read_tablespace_page(ref.page_no, first_page)) {
    return 0;
  }
  const uint16_t page_type = mach_read_from_2(first_page.data() + FIL_PAGE_TYPE);
  if (page_type != FIL_PAGE_TYPE_ZLOB_FIRST) {
    return 0;
  }

  const unsigned char* base = first_page.data() + ZLOB_FIRST_OFFSET_INDEX_LIST;
  fil_addr_t addr = read_fil_addr(base + 4);
  size_t remaining = want;
  size_t total = 0;
  size_t steps = 0;
  const size_t max_steps = 100000;

  std::vector<unsigned char> index_buf(g_lob_ctx.logical_page_size);

  while (!addr.is_null() && remaining > 0 && steps++ < max_steps) {
    if (!read_tablespace_page(addr.page, index_buf)) {
      break;
    }
    if (addr.boffset + ZLOB_INDEX_ENTRY_SIZE > g_lob_ctx.physical_page_size) {
      break;
    }
    const unsigned char* node = index_buf.data() + addr.boffset;

    ZlobIndexEntry current{};
    read_zlob_index_entry(node, current);

    ZlobIndexEntry entry{};
    if (!read_zlob_visible_entry(current, node, ref.version, entry)) {
      break;
    }

    const size_t copied = read_zlob_chunk(entry, remaining, out);
    if (copied == 0) {
      break;
    }

    total += copied;
    remaining -= copied;
    addr = current.next;
  }

  return total;
}

static size_t read_zblob_external(const LobRef& ref,
                                  size_t want,
                                  std::string& out) {
  if (want == 0 || ref.page_no == FIL_NULL) {
    return 0;
  }

  std::vector<unsigned char> page_buf(g_lob_ctx.physical_page_size);
  page_no_t page_no = ref.page_no;
  ulint offset = ref.offset;
  size_t steps = 0;
  const size_t max_steps = 100000;

  const size_t out_pos = out.size();
  out.resize(out_pos + want);
  unsigned char* out_ptr = reinterpret_cast<unsigned char*>(&out[out_pos]);

  z_stream strm;
  std::memset(&strm, 0, sizeof(strm));
  if (inflateInit(&strm) != Z_OK) {
    out.resize(out_pos);
    return 0;
  }
  strm.next_out = out_ptr;
  strm.avail_out = static_cast<uInt>(want);

  while (page_no != FIL_NULL && strm.avail_out > 0 && steps++ < max_steps) {
    if (!read_tablespace_page_raw(page_no, page_buf)) {
      break;
    }
    const uint16_t page_type = mach_read_from_2(page_buf.data() + FIL_PAGE_TYPE);
    if (page_type != FIL_PAGE_TYPE_ZBLOB &&
        page_type != FIL_PAGE_TYPE_ZBLOB2 &&
        page_type != FIL_PAGE_SDI_ZBLOB) {
      break;
    }

    page_no = mach_read_from_4(page_buf.data() + FIL_PAGE_NEXT);

    ulint data_offset = offset;
    if (data_offset == FIL_PAGE_NEXT) {
      data_offset = FIL_PAGE_DATA;
    } else {
      data_offset += 4;
    }

    if (data_offset >= g_lob_ctx.physical_page_size) {
      break;
    }

    strm.next_in = page_buf.data() + data_offset;
    strm.avail_in =
        static_cast<uInt>(g_lob_ctx.physical_page_size - data_offset);

    const int ret = inflate(&strm, Z_NO_FLUSH);
    if (ret == Z_STREAM_END) {
      break;
    }
    if (ret != Z_OK && ret != Z_BUF_ERROR) {
      break;
    }

    offset = FIL_PAGE_NEXT;
  }

  const size_t produced = want - strm.avail_out;
  inflateEnd(&strm);
  out.resize(out_pos + produced);
  return produced;
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

static bool format_innodb_date(const unsigned char* ptr, ulint len, std::string& out) {
  if (!ptr || len < 3) {
    return false;
  }
  uint32_t raw = static_cast<uint32_t>(read_be_int_signed(ptr, len));
  MYSQL_TIME tm{};
  tm.time_type = MYSQL_TIMESTAMP_DATE;
  tm.day = static_cast<unsigned int>(raw & 31);
  tm.month = static_cast<unsigned int>((raw >> 5) & 15);
  tm.year = static_cast<unsigned int>(raw >> 9);
  char buf[MAX_DATE_STRING_REP_LENGTH];
  int n = my_date_to_str(tm, buf);
  out.assign(buf, static_cast<size_t>(n));
  return true;
}

static bool format_innodb_time(const unsigned char* ptr, ulint len,
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

static size_t read_lob_external(const LobRef& ref,
                                size_t want,
                                std::string& out) {
  if (g_lob_ctx.fd < 0) {
    return 0;
  }
  std::vector<unsigned char> page_buf(g_lob_ctx.logical_page_size);
  if (!read_tablespace_page(ref.page_no, page_buf)) {
    return 0;
  }
  const uint16_t page_type = mach_read_from_2(page_buf.data() + FIL_PAGE_TYPE);
  if (page_type == FIL_PAGE_TYPE_BLOB || page_type == FIL_PAGE_SDI_BLOB) {
    return read_lob_old_chain(ref, want, out);
  }
  if (page_type == FIL_PAGE_TYPE_LOB_FIRST) {
    return read_lob_new_format(ref, want, out);
  }
  if (page_type == FIL_PAGE_TYPE_ZLOB_FIRST) {
    return read_zlob_new_format(ref, want, out);
  }
  if (page_type == FIL_PAGE_TYPE_ZBLOB ||
      page_type == FIL_PAGE_TYPE_ZBLOB2 ||
      page_type == FIL_PAGE_SDI_ZBLOB) {
    return read_zblob_external(ref, want, out);
  }
  return 0;
}

static bool read_external_lob_value(const unsigned char* field_ptr,
                                    ulint field_len,
                                    std::string& out,
                                    bool& truncated) {
  truncated = false;
  if (field_len < BTR_EXTERN_FIELD_REF_SIZE || g_lob_ctx.fd < 0) {
    return false;
  }

  const ulint local_len = field_len - BTR_EXTERN_FIELD_REF_SIZE;
  const unsigned char* ref_ptr = field_ptr + local_len;

  LobRef ref{};
  ref.space_id = mach_read_from_4(ref_ptr + lob::BTR_EXTERN_SPACE_ID);
  ref.page_no = mach_read_from_4(ref_ptr + lob::BTR_EXTERN_PAGE_NO);
  ref.offset = mach_read_from_4(ref_ptr + lob::BTR_EXTERN_OFFSET);
  ref.version = ref.offset;
  ref.length = mach_read_from_4(ref_ptr + lob::BTR_EXTERN_LEN + 4);
  ref.being_modified =
      (mach_read_from_1(ref_ptr + lob::BTR_EXTERN_LEN) &
       lob::BTR_EXTERN_BEING_MODIFIED_FLAG) != 0;

  if (ref.being_modified) {
    return false;
  }

  const size_t total_len = static_cast<size_t>(local_len) + ref.length;
  size_t limit = g_row_output.lob_max_bytes;
  size_t target_total = total_len;
  if (limit > 0 && total_len > limit) {
    target_total = limit;
    truncated = true;
  }

  out.clear();
  out.reserve(target_total);
  if (local_len > 0) {
    size_t copy_len = local_len;
    if (limit > 0 && copy_len > target_total) {
      copy_len = target_total;
      truncated = true;
    }
    out.append(reinterpret_cast<const char*>(field_ptr), copy_len);
  }

  if (ref.length == 0 || out.size() >= target_total) {
    return true;
  }

  size_t want = ref.length;
  if (limit > 0 && out.size() + want > target_total) {
    want = target_total - out.size();
  }

  size_t read_bytes = read_lob_external(ref, want, out);
  if (read_bytes != want) {
    return false;
  }
  return true;
}

static bool format_decimal_value(const field_def_t& field,
                                 const unsigned char* ptr,
                                 ulint len,
                                 std::string& out) {
  int precision = field.decimal_precision;
  int scale = field.decimal_digits;
  if (precision <= 0 || scale < 0 || scale > precision) {
    return false;
  }
  int bin_size = decimal_bin_size(precision, scale);
  if (bin_size <= 0 || len < static_cast<ulint>(bin_size)) {
    return false;
  }
  int buf_len = decimal_size(precision, scale);
  if (buf_len <= 0) {
    return false;
  }
  std::vector<decimal_digit_t> digits(static_cast<size_t>(buf_len));
  decimal_t dec{};
  dec.len = buf_len;
  dec.buf = digits.data();
  dec.intg = precision - scale;
  dec.frac = scale;
  int err = bin2decimal(ptr, &dec, precision, scale, false);
  if (err & E_DEC_FATAL_ERROR) {
    return false;
  }
  int str_len = decimal_string_size(&dec);
  if (str_len <= 0) {
    return false;
  }
  std::string tmp(static_cast<size_t>(str_len), '\0');
  int out_len = str_len;
  err = decimal2string(&dec, &tmp[0], &out_len);
  if (err & E_DEC_FATAL_ERROR || out_len <= 0) {
    return false;
  }
  tmp.resize(static_cast<size_t>(out_len));
  out.swap(tmp);
  return true;
}

static bool format_enum_value(const field_def_t& field,
                              uint64_t idx,
                              std::string& out) {
  if (!field.has_limits || field.limits.enum_values_count <= 0) {
    return false;
  }
  if (idx == 0) {
    out.clear();
    return true;
  }
  if (idx > static_cast<uint64_t>(field.limits.enum_values_count)) {
    return false;
  }
  const char* value = field.limits.enum_values[idx - 1];
  if (!value) {
    return false;
  }
  out.assign(value);
  return true;
}

static bool format_set_value(const field_def_t& field,
                             uint64_t mask,
                             std::string& out) {
  if (!field.has_limits || field.limits.set_values_count <= 0) {
    return false;
  }
  if (field.limits.set_values_count > 64) {
    return false;
  }
  bool first = true;
  std::string tmp;
  for (int i = 0; i < field.limits.set_values_count; i++) {
    if (mask & (1ULL << i)) {
      const char* value = field.limits.set_values[i];
      if (!value) {
        return false;
      }
      if (!first) {
        tmp.push_back(',');
      }
      tmp.append(value);
      first = false;
    }
  }
  out.swap(tmp);
  return true;
}

static std::string format_hex(const unsigned char* ptr, ulint len, ulint max_len = 64) {
  std::string out;
  ulint to_print = (len < max_len) ? len : max_len;
  out.reserve(static_cast<size_t>(to_print) * 2 + 4);
  char buf[3];
  for (ulint i = 0; i < to_print; i++) {
    std::snprintf(buf, sizeof(buf), "%02X", ptr[i]);
    out.append(buf);
  }
  if (len > max_len) {
    out.append("...");
  }
  return out;
}

static std::string format_text(const unsigned char* ptr, ulint len, ulint max_len = 256) {
  std::string out;
  ulint to_print = (len < max_len) ? len : max_len;
  out.reserve(static_cast<size_t>(to_print) + 16);
  for (ulint i = 0; i < to_print; i++) {
    unsigned char c = ptr[i];
    if (std::isprint(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      char buf[5];
      std::snprintf(buf, sizeof(buf), "\\x%02X", c);
      out.append(buf);
    }
  }
  if (len > max_len) {
    out.append("...(truncated)");
  }
  return out;
}

static std::string escape_control_bytes(const char* ptr, ulint len) {
  std::string out;
  out.reserve(static_cast<size_t>(len) + 16);
  for (ulint i = 0; i < len; i++) {
    unsigned char c = static_cast<unsigned char>(ptr[i]);
    if (c >= 0x20 && c != 0x7F) {
      out.push_back(static_cast<char>(c));
    } else {
      char buf[5];
      std::snprintf(buf, sizeof(buf), "\\x%02X", c);
      out.append(buf);
    }
  }
  return out;
}

static std::string format_text_with_charset(const unsigned char* ptr,
                                            ulint len,
                                            unsigned int collation_id,
                                            ulint max_len = 256) {
  ulint to_convert = (len < max_len) ? len : max_len;
  if (to_convert == 0) {
    return std::string();
  }

  const CHARSET_INFO* from_cs = nullptr;
  if (collation_id != 0) {
    from_cs = get_charset(collation_id, MYF(0));
  }
  if (!from_cs) {
    std::string out = format_text(ptr, len, max_len);
    return out;
  }

  const CHARSET_INFO* to_cs = &my_charset_utf8mb4_bin;
  size_t out_cap = static_cast<size_t>(to_convert) * to_cs->mbmaxlen + 1;
  std::string converted(out_cap, '\0');
  uint errors = 0;
  size_t out_len = my_convert(converted.data(), out_cap, to_cs,
                              reinterpret_cast<const char*>(ptr),
                              static_cast<size_t>(to_convert),
                              from_cs, &errors);
  converted.resize(out_len);

  std::string out = escape_control_bytes(converted.data(),
                                         static_cast<ulint>(converted.size()));
  if (len > max_len) {
    out.append("...(truncated)");
  }
  return out;
}

static std::string format_extern(const unsigned char* ptr, ulint len, ulint max_len = 32) {
  std::string out = "<extern:";
  out.append(std::to_string(static_cast<unsigned long long>(len)));
  out.push_back(':');
  out.append(format_hex(ptr, len, max_len));
  out.push_back('>');
  return out;
}

struct FieldOutput {
  bool is_null = false;
  bool is_numeric = false;
  std::string value;
};

static FieldOutput format_field_value(const field_def_t& field,
                                      const unsigned char* field_ptr,
                                      ulint field_len,
                                      bool is_extern,
                                      bool hex) {
  FieldOutput out;
  if (field_len == UNIV_SQL_NULL) {
    out.is_null = true;
    return out;
  }

  if (is_extern) {
    if (!hex &&
        (field.type == FT_TEXT || field.type == FT_BLOB ||
         field.type == FT_CHAR || field.type == FT_BIN)) {
      std::string lob_data;
      bool truncated = false;
      if (read_external_lob_value(field_ptr, field_len, lob_data, truncated)) {
        size_t max_len = lob_data.size();
        if (g_row_output.lob_max_bytes > 0 && max_len > g_row_output.lob_max_bytes) {
          max_len = g_row_output.lob_max_bytes;
        }
        if (field.type == FT_BLOB || field.type == FT_BIN) {
          out.value = format_hex(reinterpret_cast<const unsigned char*>(lob_data.data()),
                                 lob_data.size(), max_len);
        } else {
          out.value = format_text_with_charset(
              reinterpret_cast<const unsigned char*>(lob_data.data()),
              static_cast<ulint>(lob_data.size()),
              field.collation_id, static_cast<ulint>(max_len));
        }
        if (truncated) {
          out.value.append("...(truncated)");
        }
        return out;
      }
    }
    out.value = format_extern(field_ptr, field_len);
    return out;
  }
  if (hex) {
    out.value = format_hex(field_ptr, field_len);
    return out;
  }

  switch (field.type) {
    case FT_INT: {
      int64_t val = read_be_int_signed(field_ptr, field_len);
      out.is_numeric = true;
      out.value = std::to_string(static_cast<long long>(val));
      break;
    }
    case FT_UINT: {
      uint64_t val = read_be_uint(field_ptr, field_len);
      out.is_numeric = true;
      out.value = std::to_string(static_cast<unsigned long long>(val));
      break;
    }
    case FT_FLOAT: {
      if (field_len == 4) {
        uint32_t raw = static_cast<uint32_t>(read_be_uint(field_ptr, 4));
        float f = 0.0f;
        std::memcpy(&f, &raw, sizeof(f));
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%f", f);
        out.is_numeric = true;
        out.value = buf;
      } else {
        out.value = format_hex(field_ptr, field_len);
      }
      break;
    }
    case FT_DOUBLE: {
      if (field_len == 8) {
        uint64_t raw = read_be_uint(field_ptr, 8);
        double d = 0.0;
        std::memcpy(&d, &raw, sizeof(d));
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%f", d);
        out.is_numeric = true;
        out.value = buf;
      } else {
        out.value = format_hex(field_ptr, field_len);
      }
      break;
    }
    case FT_CHAR:
    case FT_TEXT:
      out.value = format_text_with_charset(field_ptr, field_len,
                                           field.collation_id);
      break;
    case FT_BLOB:
    case FT_BIN:
      out.value = format_hex(field_ptr, field_len, field_len);
      break;
    case FT_DATE: {
      std::string formatted;
      bool ok = format_innodb_date(field_ptr, field_len, formatted);
      out.value = ok ? formatted : format_hex(field_ptr, field_len);
      break;
    }
    case FT_TIME: {
      std::string formatted;
      unsigned int dec = static_cast<unsigned int>(field.time_precision);
      bool ok = format_innodb_time(field_ptr, field_len, dec, formatted);
      out.value = ok ? formatted : format_hex(field_ptr, field_len);
      break;
    }
    case FT_DATETIME:
    case FT_TIMESTAMP: {
      std::string formatted;
      unsigned int dec = static_cast<unsigned int>(field.time_precision);
      bool ok = false;
      if (field.type == FT_DATETIME) {
        ok = format_innodb_datetime(field_ptr, field_len, dec, formatted);
      } else {
        ok = format_innodb_timestamp(field_ptr, field_len, dec, formatted);
      }
      out.value = ok ? formatted : format_hex(field_ptr, field_len);
      break;
    }
    case FT_YEAR: {
      if (field_len == 1) {
        unsigned int year = field_ptr[0] == 0 ? 0 : 1900 + field_ptr[0];
        char buf[5];
        std::snprintf(buf, sizeof(buf), "%04u", year);
        out.value = buf;
      } else {
        out.value = format_hex(field_ptr, field_len);
      }
      break;
    }
    case FT_DECIMAL: {
      std::string formatted;
      bool ok = format_decimal_value(field, field_ptr, field_len, formatted);
      if (ok) {
        out.is_numeric = true;
        out.value = formatted;
      } else {
        out.value = format_hex(field_ptr, field_len);
      }
      break;
    }
    case FT_ENUM: {
      uint64_t idx = read_be_uint(field_ptr, field_len);
      std::string formatted;
      bool ok = format_enum_value(field, idx, formatted);
      if (ok) {
        out.value = formatted;
      } else {
        out.is_numeric = true;
        out.value = std::to_string(static_cast<unsigned long long>(idx));
      }
      break;
    }
    case FT_SET: {
      if (field_len > 8) {
        out.value = format_hex(field_ptr, field_len);
        break;
      }
      uint64_t mask = read_be_uint(field_ptr, field_len);
      std::string formatted;
      bool ok = format_set_value(field, mask, formatted);
      if (ok) {
        out.value = formatted;
      } else {
        out.is_numeric = true;
        out.value = std::to_string(static_cast<unsigned long long>(mask));
      }
      break;
    }
    case FT_BIT: {
      if (field_len <= 8) {
        uint64_t mask = read_be_uint(field_ptr, field_len);
        out.is_numeric = true;
        out.value = std::to_string(static_cast<unsigned long long>(mask));
      } else {
        out.value = format_hex(field_ptr, field_len);
      }
      break;
    }
    default:
      out.value = format_hex(field_ptr, field_len);
      break;
  }

  return out;
}

static bool csv_needs_quotes(const std::string& value) {
  return value.find_first_of(",\"\n\r") != std::string::npos;
}

static void write_csv_value(FILE* out, const std::string& value) {
  if (!csv_needs_quotes(value)) {
    std::fputs(value.c_str(), out);
    return;
  }
  std::fputc('"', out);
  for (char c : value) {
    if (c == '"') {
      std::fputc('"', out);
      std::fputc('"', out);
    } else {
      std::fputc(c, out);
    }
  }
  std::fputc('"', out);
}

static void write_json_string(FILE* out, const std::string& value) {
  std::fputc('"', out);
  for (unsigned char c : value) {
    switch (c) {
      case '\\': std::fputs("\\\\", out); break;
      case '"': std::fputs("\\\"", out); break;
      case '\b': std::fputs("\\b", out); break;
      case '\f': std::fputs("\\f", out); break;
      case '\n': std::fputs("\\n", out); break;
      case '\r': std::fputs("\\r", out); break;
      case '\t': std::fputs("\\t", out); break;
      default:
        if (c < 0x20) {
          std::fprintf(out, "\\u%04X", c);
        } else {
          std::fputc(c, out);
        }
        break;
    }
  }
  std::fputc('"', out);
}

/** process_ibrec() => print columns in selected format. */
ulint process_ibrec(page_t *page, rec_t *rec, table_def_t *table, ulint *offsets,
                    bool hex, const RowMeta* meta)
{
  (void)page; // not used here
  const bool show_internal = parser_debug_enabled();
  FILE* out = output_stream();

  if (g_row_output.format != ROW_OUTPUT_JSONL) {
    if (!g_printed_header) {
      ulint printed = 0;
      if (g_row_output.include_meta && meta) {
        std::fputs("page_no", out);
        std::fputs(g_row_output.format == ROW_OUTPUT_CSV ? "," : "|", out);
        std::fputs("rec_offset", out);
        std::fputs(g_row_output.format == ROW_OUTPUT_CSV ? "," : "|", out);
        std::fputs("rec_deleted", out);
        printed += 3;
      }

      for (ulint i = 0; i < (ulint)table->fields_count; i++) {
        if (!show_internal && table->fields[i].type == FT_INTERNAL) {
          continue;
        }
        if (printed > 0) {
          std::fputs(g_row_output.format == ROW_OUTPUT_CSV ? "," : "|", out);
        }
        std::fputs(table->fields[i].name, out);
        printed++;
      }
      std::fputc('\n', out);
      g_printed_header = true;
    }
  }

  ulint data_size = my_rec_offs_data_size(offsets);

  if (g_row_output.format == ROW_OUTPUT_JSONL) {
    bool first = true;
    std::fputc('{', out);
    if (g_row_output.include_meta && meta) {
      std::fputs("\"page_no\":", out);
      std::fprintf(out, "%llu", static_cast<unsigned long long>(meta->page_no));
      std::fputs(",\"rec_offset\":", out);
      std::fprintf(out, "%lu", static_cast<unsigned long>(meta->rec_offset));
      std::fputs(",\"rec_deleted\":", out);
      std::fputs(meta->deleted ? "true" : "false", out);
      first = false;
    }

    for (ulint i = 0; i < (ulint)table->fields_count; i++) {
      if (!show_internal && table->fields[i].type == FT_INTERNAL) {
        continue;
      }
      ulint field_len;
      const unsigned char* field_ptr = my_rec_get_nth_field(rec, offsets, i, &field_len);
      bool is_extern = my_rec_offs_nth_extern(offsets, i);
      FieldOutput value = format_field_value(table->fields[i], field_ptr, field_len, is_extern, hex);

      if (!first) {
        std::fputc(',', out);
      }
      write_json_string(out, table->fields[i].name);
      std::fputc(':', out);
      if (value.is_null) {
        std::fputs("null", out);
      } else if (value.is_numeric) {
        std::fputs(value.value.c_str(), out);
      } else {
        write_json_string(out, value.value);
      }
      first = false;
    }
    std::fputs("}\n", out);
    return data_size;
  }

  ulint printed = 0;
  if (g_row_output.include_meta && meta) {
    if (g_row_output.format == ROW_OUTPUT_CSV) {
      write_csv_value(out, std::to_string(static_cast<unsigned long long>(meta->page_no)));
      std::fputc(',', out);
      write_csv_value(out, std::to_string(static_cast<unsigned long>(meta->rec_offset)));
      std::fputc(',', out);
      write_csv_value(out, meta->deleted ? "true" : "false");
    } else {
      std::fprintf(out, "%llu|%lu|%s",
                   static_cast<unsigned long long>(meta->page_no),
                   static_cast<unsigned long>(meta->rec_offset),
                   meta->deleted ? "true" : "false");
    }
    printed += 3;
  }

  for (ulint i = 0; i < (ulint)table->fields_count; i++) {
    if (!show_internal && table->fields[i].type == FT_INTERNAL) {
      continue;
    }
    ulint field_len;
    const unsigned char* field_ptr = my_rec_get_nth_field(rec, offsets, i, &field_len);
    bool is_extern = my_rec_offs_nth_extern(offsets, i);
    FieldOutput value = format_field_value(table->fields[i], field_ptr, field_len, is_extern, hex);

    if (printed > 0) {
      std::fputc(g_row_output.format == ROW_OUTPUT_CSV ? ',' : '|', out);
    }

    if (value.is_null) {
      if (g_row_output.format == ROW_OUTPUT_CSV) {
        write_csv_value(out, "NULL");
      } else {
        std::fputs("NULL", out);
      }
    } else {
      if (g_row_output.format == ROW_OUTPUT_CSV) {
        write_csv_value(out, value.value);
      } else {
        std::fputs(value.value.c_str(), out);
      }
    }
    printed++;
  }
  std::fputc('\n', out);

  return data_size;
}
