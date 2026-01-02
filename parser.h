// ---------------
// Declarations from parser.cc
// ---------------
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>
#include "page0page.h"
#include "tables_dict.h"

void parse_records_on_page(const unsigned char* page,
                           size_t page_size,
                           uint64_t page_no);

int discover_target_index_id(int fd);

bool is_target_index(const unsigned char* page);

int load_ib2sdi_table_columns(const char* json_path, std::string& table_name);

int build_table_def_from_json(table_def_t* table, const char* tbl_name);

bool has_sdi_index_definitions();
void print_sdi_indexes(FILE* out);
bool select_index_for_parsing(const std::string& selector, std::string* error);
page_no_t selected_index_root();
const std::string& selected_index_name();
bool target_index_is_set();
void set_target_index_id_from_value(uint64_t id);

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
