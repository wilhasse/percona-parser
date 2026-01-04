#ifndef PARSER_H
#define PARSER_H

// ---------------
// Declarations from parser.cc
// ---------------
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>
#include "page0page.h"
#include "tables_dict.h"

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

struct parser_context_t {
  uint64_t target_index_id;
  bool target_index_set;
  std::string target_index_name;
  page_no_t target_index_root;
  std::vector<MyColumnDef> columns;
  std::vector<MyColumnDef> columns_by_opx;
  std::vector<IndexDef> index_defs;

  parser_context_t();
};

void parse_records_on_page(const unsigned char* page,
                           size_t page_size,
                           uint64_t page_no,
                           const parser_context_t* ctx);

int discover_target_index_id(int fd, parser_context_t* ctx);

bool is_target_index(const unsigned char* page, const parser_context_t* ctx);

int load_ib2sdi_table_columns(const char* json_path,
                              std::string& table_name,
                              parser_context_t* ctx);

int build_table_def_from_json(table_def_t* table,
                              const char* tbl_name,
                              const parser_context_t* ctx);

bool has_sdi_index_definitions(const parser_context_t* ctx);
void print_sdi_indexes(const parser_context_t* ctx, FILE* out);
bool select_index_for_parsing(parser_context_t* ctx, const std::string& selector, std::string* error);
page_no_t selected_index_root(const parser_context_t* ctx);
const std::string& selected_index_name(const parser_context_t* ctx);
bool target_index_is_set(const parser_context_t* ctx);
void set_target_index_id_from_value(parser_context_t* ctx, uint64_t id);

bool parser_debug_enabled();

bool format_innodb_datetime(const unsigned char* ptr, ulint len,
                            unsigned int dec, std::string& out);
bool format_innodb_timestamp(const unsigned char* ptr, ulint len,
                             unsigned int dec, std::string& out);
bool format_innodb_date(const unsigned char* ptr, ulint len, std::string& out);
bool format_innodb_time(const unsigned char* ptr, ulint len,
                        unsigned int dec, std::string& out);

void debug_print_table_def(const table_def_t *table);

void debug_print_compact_row(const page_t* page,
                             const rec_t* rec,
                             const table_def_t* table,
                             const ulint* offsets);

#endif  // PARSER_H

// ---------------
// Callback-based record extraction for API use
// ---------------

// Parsed column value
struct parsed_column_t {
    const char* name;           // Column name (points to table_def)
    int field_type;             // field_type_t enum value
    bool is_null;               // NULL value
    bool is_internal;           // Internal column (DB_TRX_ID, DB_ROLL_PTR)

    // Value data (depends on type)
    union {
        int64_t int_val;
        uint64_t uint_val;
        double double_val;
    };

    // String/binary data
    const unsigned char* data;  // Points into page buffer
    size_t data_len;

    // Formatted string representation
    char formatted[512];
};

// Parsed row
struct parsed_row_t {
    uint64_t page_no;
    ulint rec_offset;
    bool deleted;
    int column_count;
    parsed_column_t columns[MAX_TABLE_FIELDS];
};

// Record callback function type
typedef bool (*record_callback_t)(const parsed_row_t* row, void* user_data);

// Parse records on a page with callback
// Returns number of valid records parsed
int parse_records_with_callback(const unsigned char* page,
                                size_t page_size,
                                uint64_t page_no,
                                table_def_t* table,
                                const parser_context_t* ctx,
                                record_callback_t callback,
                                void* user_data);

// Extract a single record's data
bool extract_record_data(const page_t* page,
                         const rec_t* rec,
                         table_def_t* table,
                         const ulint* offsets,
                         uint64_t page_no,
                         ulint rec_offset,
                         bool deleted,
                         parsed_row_t* out_row);
