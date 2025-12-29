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

void debug_print_table_def(const table_def_t *table);

void debug_print_compact_row(const page_t* page,
                             const rec_t* rec,
                             const table_def_t* table,
                             const ulint* offsets);
