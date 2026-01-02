#include <stdio.h>
#include <stdlib.h>
#include "lib/ibd_reader_api.h"

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s <ibd_file> <sdi_json>\n", argv[0]);
        return 1;
    }

    printf("Initializing library...\n");
    ibd_result_t res = ibd_init();
    if (res != IBD_SUCCESS) {
        printf("Failed to init library: %d\n", res);
        return 1;
    }

    printf("Creating reader...\n");
    ibd_reader_t reader = ibd_reader_create();
    if (!reader) {
        printf("Failed to create reader\n");
        return 1;
    }

    printf("Opening table %s with SDI %s...\n", argv[1], argv[2]);
    ibd_table_t table = NULL;
    res = ibd_open_table(reader, argv[1], argv[2], &table);
    if (res != IBD_SUCCESS) {
        printf("Failed to open table: %d - %s\n", res, ibd_reader_get_error(reader));
        ibd_reader_destroy(reader);
        ibd_cleanup();
        return 1;
    }

    // Get table info
    char table_name[256];
    uint32_t column_count = 0;
    res = ibd_get_table_info(table, table_name, sizeof(table_name), &column_count);
    if (res != IBD_SUCCESS) {
        printf("Failed to get table info: %d\n", res);
    } else {
        printf("Table: %s, Columns: %u\n", table_name, column_count);

        // Print column info
        for (uint32_t i = 0; i < column_count; i++) {
            char col_name[128];
            ibd_column_type_t col_type;
            if (ibd_get_column_info(table, i, col_name, sizeof(col_name), &col_type) == IBD_SUCCESS) {
                printf("  Column %u: %s (type=%d)\n", i, col_name, col_type);
            }
        }
    }

    // Read rows
    printf("\nReading rows...\n");
    ibd_row_t row = NULL;
    int row_count = 0;
    while ((res = ibd_read_row(table, &row)) == IBD_SUCCESS && row_count < 10) {
        char buffer[1024];
        ibd_row_to_string(row, buffer, sizeof(buffer));
        printf("Row %d: %s\n", row_count, buffer);
        ibd_free_row(row);
        row_count++;
    }
    if (res != IBD_SUCCESS && res != IBD_END_OF_STREAM) {
        printf("Row read failed: %d\n", res);
    }

    printf("\nTotal rows read (limited to 10): %d\n", row_count);
    printf("Actual row count: %lu\n", (unsigned long)ibd_get_row_count(table));

    ibd_close_table(table);
    ibd_reader_destroy(reader);
    ibd_cleanup();

    printf("Success!\n");
    return 0;
}
