#ifndef UNDROP_FOR_INNODB_H
#define UNDROP_FOR_INNODB_H

#include <cstdio>
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
};

struct RowMeta {
  uint64_t page_no = 0;
  ulint rec_offset = 0;
  bool deleted = false;
};

void set_row_output_options(const RowOutputOptions& opts);

bool check_for_a_record(page_t *page, rec_t *rec, table_def_t *table, ulint *offsets);
ulint process_ibrec(page_t *page, rec_t *rec, table_def_t *table, ulint *offsets,
                    bool hex, const RowMeta* meta);

#endif // UNDROP_FOR_INNODB_H
