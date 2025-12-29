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

// MySQL includes
#include "tables_dict.h"
#include "univ.i"
#include "page0page.h"
#include "rem0rec.h"
#include "parser.h"
#include "undrop_for_innodb.h"

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

void set_row_output_options(const RowOutputOptions& opts) {
  g_row_output = opts;
  g_printed_header = false;
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
      out.value = format_text(field_ptr, field_len);
      break;
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
