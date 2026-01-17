#ifndef UNDROP_FOR_INNODB_H
#define UNDROP_FOR_INNODB_H

#include <cstdio>
#include <cstddef>
#include "page0page.h"
#include "tables_dict.h"

enum RowOutputFormat {
  ROW_OUTPUT_PIPE = 0,
  ROW_OUTPUT_CSV,
  ROW_OUTPUT_JSONL
};

struct RowOutputOptions {
  RowOutputFormat format = ROW_OUTPUT_PIPE;
  bool include_meta = false;
  FILE* out = nullptr;
  size_t lob_max_bytes = 4 * 1024 * 1024;
  bool raw_integers = false;  // Skip InnoDB sign-bit decoding for test files
};

struct RowMeta {
  uint64_t page_no = 0;
  ulint rec_offset = 0;
  bool deleted = false;
};

struct LobReadContext {
  int fd = -1;
  size_t physical_page_size = 0;
  size_t logical_page_size = 0;
  bool tablespace_compressed = false;
};

void set_row_output_options(const RowOutputOptions& opts);
void set_lob_read_context(const LobReadContext& ctx);

bool check_for_a_record(page_t *page, rec_t *rec, table_def_t *table, ulint *offsets);
ulint process_ibrec(page_t *page, rec_t *rec, table_def_t *table, ulint *offsets,
                    bool hex, const RowMeta* meta);

#endif // UNDROP_FOR_INNODB_H
