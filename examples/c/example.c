/**
 * Example usage of the InnoDB Reader Library in C
 * 
 * Compile with:
 * gcc -o example example.c -L../../build -libd_reader -Wl,-rpath,../../build
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../lib/ibd_reader_api.h"

void print_separator() {
    printf("========================================\n");
}

void example_decompress_file(const char* input_file, const char* output_file) {
    printf("\n");
    print_separator();
    printf("Example: Decompress IBD File\n");
    print_separator();
    
    // Create reader context
    ibd_reader_t reader = ibd_reader_create();
    if (!reader) {
        fprintf(stderr, "Failed to create reader\n");
        return;
    }
    
    // Enable debug mode for verbose output
    ibd_reader_set_debug(reader, 1);
    
    printf("Input file: %s\n", input_file);
    printf("Output file: %s\n", output_file);
    printf("\nDecompressing...\n");
    
    // Decompress the file
    ibd_result_t result = ibd_decompress_file(reader, input_file, output_file);
    
    if (result == IBD_SUCCESS) {
        printf("\nDecompression successful!\n");
    } else {
        fprintf(stderr, "\nDecompression failed: %s (error code: %d)\n", 
                ibd_reader_get_error(reader), result);
    }
    
    // Cleanup
    ibd_reader_destroy(reader);
}

void example_decompress_page() {
    printf("\n");
    print_separator();
    printf("Example: Decompress Single Page\n");
    print_separator();
    
    // Simulate a compressed page (in reality, this would be read from file)
    // This is just example data - real compressed pages have specific format
    uint8_t compressed_page[8192];
    memset(compressed_page, 0, sizeof(compressed_page));
    
    // Output buffer for decompressed page
    uint8_t decompressed_page[16384];
    size_t decompressed_size = sizeof(decompressed_page);
    
    // Page information structure
    ibd_page_info_t page_info;
    
    // Decompress the page
    ibd_result_t result = ibd_decompress_page(
        NULL,  // No reader context needed for stateless operation
        compressed_page,
        sizeof(compressed_page),
        decompressed_page,
        &decompressed_size,
        &page_info
    );
    
    if (result == IBD_SUCCESS) {
        printf("Page decompression successful!\n");
        printf("  Page number: %u\n", page_info.page_number);
        printf("  Page type: %u (%s)\n", page_info.page_type, 
               ibd_get_page_type_name(page_info.page_type));
        printf("  Physical size: %zu\n", page_info.physical_size);
        printf("  Logical size: %zu\n", page_info.logical_size);
        printf("  Is compressed: %s\n", page_info.is_compressed ? "Yes" : "No");
    } else {
        fprintf(stderr, "Page decompression failed (error code: %d)\n", result);
    }
}

void example_get_page_info() {
    printf("\n");
    print_separator();
    printf("Example: Get Page Information\n");
    print_separator();
    
    // Simulate a page header
    uint8_t page_header[38];
    memset(page_header, 0, sizeof(page_header));
    
    // Set page type at offset 24 (FIL_PAGE_TYPE)
    page_header[24] = 0x45;  // 17855 >> 8
    page_header[25] = 0xBF;  // 17855 & 0xFF (FIL_PAGE_INDEX)
    
    // Set page number at offset 4 (FIL_PAGE_OFFSET)
    page_header[4] = 0;
    page_header[5] = 0;
    page_header[6] = 0;
    page_header[7] = 42;  // Page number 42
    
    ibd_page_info_t info;
    ibd_result_t result = ibd_get_page_info(page_header, sizeof(page_header), &info);
    
    if (result == IBD_SUCCESS) {
        printf("Page information:\n");
        printf("  Page number: %u\n", info.page_number);
        printf("  Page type: %u (%s)\n", info.page_type, 
               ibd_get_page_type_name(info.page_type));
        printf("  Is compressed: %s\n", info.is_compressed ? "Yes" : "No");
        printf("  Is encrypted: %s\n", info.is_encrypted ? "Yes" : "No");
    }
}

void example_library_info() {
    printf("\n");
    print_separator();
    printf("InnoDB Reader Library Information\n");
    print_separator();
    
    printf("Library version: %s\n", ibd_get_version());
    printf("\nSupported page types:\n");
    
    uint16_t page_types[] = {
        IBD_PAGE_TYPE_ALLOCATED,
        IBD_PAGE_UNDO_LOG,
        IBD_PAGE_INODE,
        IBD_PAGE_TYPE_FSP_HDR,
        IBD_PAGE_INDEX,
        IBD_PAGE_COMPRESSED,
        IBD_PAGE_ENCRYPTED
    };
    
    for (size_t i = 0; i < sizeof(page_types)/sizeof(page_types[0]); i++) {
        printf("  %5u: %s\n", page_types[i], 
               ibd_get_page_type_name(page_types[i]));
    }
}

int main(int argc, char* argv[]) {
    printf("InnoDB Reader Library - C Example\n");
    printf("==================================\n");
    
    // Initialize the library
    ibd_result_t init_result = ibd_init();
    if (init_result != IBD_SUCCESS) {
        fprintf(stderr, "Failed to initialize library\n");
        return 1;
    }
    
    // Show library information
    example_library_info();
    
    // Example: Get page information
    example_get_page_info();
    
    // Example: Decompress a page
    example_decompress_page();
    
    // Example: Decompress a file (if arguments provided)
    if (argc == 3) {
        example_decompress_file(argv[1], argv[2]);
    } else {
        printf("\n");
        print_separator();
        printf("File Decompression Example\n");
        print_separator();
        printf("To decompress a file, run:\n");
        printf("  %s <input.ibd> <output.ibd>\n", argv[0]);
    }
    
    // Cleanup the library
    ibd_cleanup();
    
    return 0;
}