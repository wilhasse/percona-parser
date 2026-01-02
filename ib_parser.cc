/*****************************************************************************
  ib_parser.cc

  Demonstrates how to:
    - parse a "mode" (1=decrypt, 2=decompress, 3=both),
    - run the appropriate logic for either decrypt, decompress, or both.

  This merges the old "main" from decrypt.cc and decompress.cc into
  one single program.
*****************************************************************************/

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <limits>
#include <unistd.h>

// MySQL/Percona, OpenSSL, etc.
#include <my_sys.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <mysql/plugin.h>
#include <my_sys.h>
#include <my_thread.h>
#include <m_string.h>
#include <mysys_err.h>
#include <mysqld_error.h>
#include <fcntl.h>  // For O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC

// InnoDB
#include "page0size.h"  // Add this for page_size_t
#include "fil0fil.h"      // for fsp_header_get_flags()
#include "fsp0fsp.h"      // for fsp_flags_is_valid(), etc.
#include "fsp0types.h"
#include "mach0data.h"


// Your headers that declare "decrypt_page_inplace()", "decompress_page_inplace()",
// "get_master_key()", "read_tablespace_key_iv()", etc.
#include "decrypt.h"     // Contains e.g. decrypt_page_inplace(), get_master_key() ...
#include "decompress.h"  // Contains e.g. decompress_page_inplace(), etc.
#include "parser.h"      // Contains parser logic
#include "undrop_for_innodb.h"

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

static bool read_index_id_from_root(int fd, page_no_t root, uint64_t* out) {
  if (root == FIL_NULL || out == nullptr) {
    return false;
  }

  File mfd = (File)fd;
  page_size_t pg_sz(0, 0, false);
  if (!determine_page_size(mfd, pg_sz)) {
    return false;
  }

  const size_t physical_size = pg_sz.physical();
  const size_t logical_size = pg_sz.logical();
  const bool tablespace_compressed = (physical_size < logical_size);

  std::vector<unsigned char> page_buf(physical_size);
  std::vector<unsigned char> logical_buf;
  if (tablespace_compressed) {
    logical_buf.resize(logical_size);
  }

  const off_t offset = static_cast<off_t>(root) *
                       static_cast<off_t>(physical_size);
  if (pread(fd, page_buf.data(), physical_size, offset) !=
      static_cast<ssize_t>(physical_size)) {
    return false;
  }

  const unsigned char* page_data = page_buf.data();
  if (tablespace_compressed) {
    size_t actual_size = 0;
    if (!decompress_page_inplace(page_buf.data(), physical_size, logical_size,
                                 logical_buf.data(), logical_size, &actual_size)) {
      return false;
    }
    if (actual_size != logical_size) {
      return false;
    }
    page_data = logical_buf.data();
  }

  if (fil_page_get_type(page_data) != FIL_PAGE_INDEX) {
    return false;
  }

  *out = mach_read_from_8(page_data + PAGE_HEADER + PAGE_INDEX_ID);
  return true;
}

// We assume a fixed 16K buffer for reading page 0, 
// since the maximum InnoDB page size is 16K in many setups.
// (If you allow 32K or 64K pages, you’ll need a bigger buffer.)
static bool is_table_compressed(File in_fd)
{
    // Step 1: Seek to 0
    if (my_seek(in_fd, 0, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
        fprintf(stderr, "Error: cannot seek to start.\n");
        return false; // default
    }

    // Step 2: Read up to 16 KB for page 0
    unsigned char page0[UNIV_PAGE_SIZE_ORIG]; 
    memset(page0, 0, sizeof(page0));

    size_t r = my_read(in_fd, page0, sizeof(page0), MYF(0));
    if (r < FIL_PAGE_DATA) {
        fprintf(stderr, "Error: cannot read page 0.\n");
        // For safety, return false => 'not compressed' as fallback
        return false;
    }

    // Step 3: Extract the fsp flags from page 0
    uint32_t flags = fsp_header_get_flags(page0);
    if (!fsp_flags_is_valid(flags)) {
        fprintf(stderr, "Error: fsp flags not valid on page 0.\n");
        return false;
    }

    // Depending on MySQL version, the "space is compressed" bit
    // is often tested by something like "FSP_FLAGS_GET_ZIP_SSIZE(flags) != 0"
    // or "fsp_flags_is_compressed(flags)" if you have that helper.
    // We'll demonstrate one possible check:
    ulint zip_ssize = FSP_FLAGS_GET_ZIP_SSIZE(flags);
    bool compressed = (zip_ssize != 0);

    return compressed;
}

/** 
 * Minimal usage print 
 */
static void usage() {
  std::cerr << "Usage:\n"
            << "  ib_parser <mode> [decrypt/decompress args...]\n\n"
            << "Where <mode> is:\n"
            << "  1 = Decrypt only\n"
            << "  2 = Decompress only\n"
            << "  3 = Parse only\n"
            << "  4 = Decrypt then Decompress in a single pass\n"
            << "  5 = Rebuild to uncompressed (experimental)\n\n"
            << "Examples:\n"
            << "  ib_parser 1 <master_key_id> <server_uuid> <keyring_file> <ibd_path> <dest_path>\n"
            << "  ib_parser 2 <in_file.ibd> <out_file>\n"
            << "  ib_parser 3 <in_file.ibd> <table_def.json> [--index=NAME|ID] [--list-indexes]\n"
            << "    [--format=pipe|csv|jsonl] [--output=PATH] [--with-meta] [--lob-max-bytes=N]\n"
            << "  ib_parser 4 <master_key_id> <server_uuid> <keyring_file> <ibd_path> <dest_path>\n"
            << "  ib_parser 5 <in_file.ibd> <out_file> [--sdi-json=PATH]\n"
            << "    [--target-sdi-json=PATH] [--index-id-map=PATH] [--cfg-out=PATH]\n"
            << std::endl;
}

/**
 * (A) The "decrypt only" routine, adapted from your old decrypt main().
 *     We assume you have a function: 
 *         bool decrypt_ibd_file(const char* src, const char* dst, 
 *                               const Tablespace_key_iv &ts_key_iv);
 */
static int do_decrypt_main(int argc, char** argv)
{
  if (argc < 6) {
    std::cerr << "Usage for mode=1 (decrypt):\n"
              << "  ib_parser 1 <master_key_id> <server_uuid> <keyring_file> <ibd_path> <dest_path>\n";
    return 1;
  }

  uint32_t master_id       = static_cast<uint32_t>(std::atoi(argv[1]));
  std::string srv_uuid     = argv[2];
  const char* keyring_path = argv[3];
  const char* ibd_path     = argv[4];
  const char* dest_path    = argv[5];

  // 1) Global MySQL init
  my_init();
  my_thread_init();
  OpenSSL_add_all_algorithms();

  // 2) get the master key
  std::vector<unsigned char> master_key;
  if (!get_master_key(master_id, srv_uuid, keyring_path, master_key)) {
    std::cerr << "Could not get master key\n";
    return 1;
  }

  // 3) Open .ibd for reading so we can see if it's compressed
  File in_fd = my_open(ibd_path, O_RDONLY, MYF(0));
  if (in_fd < 0) {
    std::cerr << "Cannot open file " << ibd_path << std::endl;
    return 1;
  }
  bool compressed = is_table_compressed(in_fd);
  my_close(in_fd, MYF(0));

  // -----------------------------
  // 3a) read the tablespace key/IV
  // Compressed Page: Seek to offset 5270 (0x1496)
  // Uncompressed Page: Offset 10390 (0x2896)
  // -----------------------------
  long offset = compressed ? 5270 : 10390;
  Tablespace_key_iv ts_key_iv;
  if (!read_tablespace_key_iv(ibd_path, offset, master_key, ts_key_iv)) {
    std::cerr << "Could not read tablespace key\n";
    return 1;
  }

  // 4) Decrypt the entire .ibd
  if (!decrypt_ibd_file(ibd_path, dest_path, ts_key_iv, compressed)) {
    std::cerr << "Decrypt failed.\n";
    return 1;
  }

  my_thread_end();
  my_end(0);
  return 0;  // success
}

/**
 * (B) The "decompress only" routine, adapted from your old decompress main().
 *     We assume you have: bool decompress_ibd(File in_fd, File out_fd);
 */
static int do_decompress_main(int argc, char** argv)
{
  if (argc < 3) {
    std::cerr << "Usage for mode=2 (decompress):\n"
              << "  ib_parser 2 <in_file> <out_file>\n";
    return 1;
  }

  MY_INIT(argv[0]);
  DBUG_TRACE;
  DBUG_PROCESS(argv[0]);

  const char* in_file  = argv[1];
  const char* out_file = argv[2];

  // open input
  File in_fd = my_open(in_file, O_RDONLY, MYF(0));
  if (in_fd < 0) {
    fprintf(stderr, "Cannot open input '%s'.\n", in_file);
    return 1;
  }
  // open output
  File out_fd = my_open(out_file, O_CREAT | O_WRONLY | O_TRUNC, MYF(0));
  if (out_fd < 0) {
    fprintf(stderr, "Cannot open/create output '%s'.\n", out_file);
    my_close(in_fd, MYF(0));
    return 1;
  }

  bool ok = decompress_ibd(in_fd, out_fd); 
  my_close(in_fd, MYF(0));
  my_close(out_fd, MYF(0));
  return ok ? 0 : 1;
}

/**
 * (E) The "rebuild to uncompressed" routine (experimental).
 *     Produces 16KB pages with CRC32 checksums.
 */
static int do_rebuild_uncompressed_main(int argc, char** argv)
{
  if (argc < 3) {
    std::cerr << "Usage for mode=5 (rebuild-uncompressed):\n"
              << "  ib_parser 5 <in_file> <out_file> [--sdi-json=PATH]\n"
              << "    [--target-sdi-json=PATH] [--index-id-map=PATH]\n"
              << "    [--target-sdi-root=N] [--use-target-sdi-root|--use-source-sdi-root]\n"
              << "    [--target-space-id=N] [--use-target-space-id|--use-source-space-id]\n"
              << "    [--target-ibd=PATH] [--cfg-out=PATH] [--validate-remap]\n";
    return 1;
  }

  MY_INIT(argv[0]);
  DBUG_TRACE;
  DBUG_PROCESS(argv[0]);

  const char* in_file  = argv[1];
  const char* out_file = argv[2];
  const char* source_sdi_json = nullptr;
  const char* target_sdi_json = nullptr;
  const char* index_id_map = nullptr;
  const char* cfg_out = nullptr;
  const char* target_ibd = nullptr;
  bool use_target_sdi_root = false;
  bool use_source_sdi_root = false;
  bool target_sdi_root_override_set = false;
  uint32_t target_sdi_root_override = 0;
  bool use_target_space_id = false;
  bool use_source_space_id = false;
  bool target_space_id_override_set = false;
  uint32_t target_space_id_override = 0;
  bool validate_remap = false;

  for (int i = 3; i < argc; ++i) {
    const char* arg = argv[i];
    if (strncmp(arg, "--sdi-json=", 11) == 0) {
      source_sdi_json = arg + 11;
      continue;
    }
    if (strcmp(arg, "--sdi-json") == 0 && i + 1 < argc) {
      source_sdi_json = argv[++i];
      continue;
    }
    if (strncmp(arg, "--target-sdi-json=", 18) == 0) {
      target_sdi_json = arg + 18;
      continue;
    }
    if (strcmp(arg, "--target-sdi-json") == 0 && i + 1 < argc) {
      target_sdi_json = argv[++i];
      continue;
    }
    if (strncmp(arg, "--target-ibd=", 13) == 0) {
      target_ibd = arg + 13;
      continue;
    }
    if (strcmp(arg, "--target-ibd") == 0 && i + 1 < argc) {
      target_ibd = argv[++i];
      continue;
    }
    if (strncmp(arg, "--target-sdi-root=", 18) == 0) {
      const char* value = arg + 18;
      unsigned long long root = std::strtoull(value, nullptr, 10);
      if (root > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "Invalid --target-sdi-root value\n";
        return 1;
      }
      target_sdi_root_override = static_cast<uint32_t>(root);
      target_sdi_root_override_set = true;
      continue;
    }
    if (strcmp(arg, "--target-sdi-root") == 0 && i + 1 < argc) {
      unsigned long long root = std::strtoull(argv[++i], nullptr, 10);
      if (root > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "Invalid --target-sdi-root value\n";
        return 1;
      }
      target_sdi_root_override = static_cast<uint32_t>(root);
      target_sdi_root_override_set = true;
      continue;
    }
    if (strncmp(arg, "--target-space-id=", 18) == 0) {
      const char* value = arg + 18;
      unsigned long long space_id = std::strtoull(value, nullptr, 10);
      if (space_id == 0 || space_id > std::numeric_limits<uint32_t>::max() ||
          space_id == SPACE_UNKNOWN) {
        std::cerr << "Invalid --target-space-id value\n";
        return 1;
      }
      target_space_id_override = static_cast<uint32_t>(space_id);
      target_space_id_override_set = true;
      continue;
    }
    if (strcmp(arg, "--target-space-id") == 0 && i + 1 < argc) {
      unsigned long long space_id = std::strtoull(argv[++i], nullptr, 10);
      if (space_id == 0 || space_id > std::numeric_limits<uint32_t>::max() ||
          space_id == SPACE_UNKNOWN) {
        std::cerr << "Invalid --target-space-id value\n";
        return 1;
      }
      target_space_id_override = static_cast<uint32_t>(space_id);
      target_space_id_override_set = true;
      continue;
    }
    if (strcmp(arg, "--use-target-sdi-root") == 0) {
      use_target_sdi_root = true;
      continue;
    }
    if (strcmp(arg, "--use-source-sdi-root") == 0) {
      use_source_sdi_root = true;
      continue;
    }
    if (strcmp(arg, "--use-target-space-id") == 0) {
      use_target_space_id = true;
      continue;
    }
    if (strcmp(arg, "--use-source-space-id") == 0) {
      use_source_space_id = true;
      continue;
    }
    if (strcmp(arg, "--validate-remap") == 0) {
      validate_remap = true;
      continue;
    }
    if (strncmp(arg, "--index-id-map=", 15) == 0) {
      index_id_map = arg + 15;
      continue;
    }
    if (strcmp(arg, "--index-id-map") == 0 && i + 1 < argc) {
      index_id_map = argv[++i];
      continue;
    }
    if (strncmp(arg, "--cfg-out=", 10) == 0) {
      cfg_out = arg + 10;
      continue;
    }
    if (strcmp(arg, "--cfg-out") == 0 && i + 1 < argc) {
      cfg_out = argv[++i];
      continue;
    }
    std::cerr << "Unknown option: " << arg << "\n";
    return 1;
  }

  if (target_sdi_json != nullptr && source_sdi_json == nullptr) {
    std::cerr << "Error: --target-sdi-json requires --sdi-json (source).\n";
    return 1;
  }

  if (use_target_sdi_root && use_source_sdi_root) {
    std::cerr << "Error: --use-target-sdi-root and --use-source-sdi-root are mutually exclusive.\n";
    return 1;
  }
  if (use_target_space_id && use_source_space_id) {
    std::cerr << "Error: --use-target-space-id and --use-source-space-id are mutually exclusive.\n";
    return 1;
  }

  if (cfg_out != nullptr &&
      (target_sdi_json == nullptr && source_sdi_json == nullptr)) {
    std::cerr << "Error: --cfg-out requires --sdi-json or --target-sdi-json.\n";
    return 1;
  }

  if (validate_remap) {
    if (source_sdi_json == nullptr || target_sdi_json == nullptr) {
      std::cerr << "Error: --validate-remap requires --sdi-json and --target-sdi-json.\n";
      return 1;
    }
    const bool ok = validate_index_id_remap(source_sdi_json,
                                            target_sdi_json,
                                            index_id_map);
    return ok ? 0 : 1;
  }

  File in_fd = my_open(in_file, O_RDONLY, MYF(0));
  if (in_fd < 0) {
    fprintf(stderr, "Cannot open input '%s'.\n", in_file);
    return 1;
  }
  File out_fd = my_open(out_file, O_CREAT | O_WRONLY | O_TRUNC, MYF(0));
  if (out_fd < 0) {
    fprintf(stderr, "Cannot open/create output '%s'.\n", out_file);
    my_close(in_fd, MYF(0));
    return 1;
  }

  bool ok = rebuild_uncompressed_ibd(in_fd, out_fd, source_sdi_json,
                                     target_sdi_json, index_id_map, cfg_out,
                                     use_target_sdi_root, use_source_sdi_root,
                                     target_sdi_root_override_set,
                                     target_sdi_root_override, target_ibd,
                                     use_target_space_id, use_source_space_id,
                                     target_space_id_override_set,
                                     target_space_id_override);
  my_close(in_fd, MYF(0));
  my_close(out_fd, MYF(0));
  return ok ? 0 : 1;
}

/**
 * (C) The "parse only" routine (unencrypted + uncompressed).
 *     Illustrative minimal example based on undrop-for-innodb code.
 */
static int do_parse_main(int argc, char** argv)
{
  if (argc < 3) {
    std::cerr << "Usage for mode=3 (parse-only):\n"
              << "  ib_parser 3 <in_file.ibd> <table_def.json> [--index=NAME|ID] [--list-indexes]\n"
              << "    [--format=pipe|csv|jsonl] [--output=PATH] [--with-meta] [--lob-max-bytes=N]\n";
    return 1;
  }

  const char* in_file = argv[1];
  const char* json_file = argv[2];
  const char* out_path = nullptr;
  std::string index_selector;
  bool index_selector_explicit = false;
  bool list_indexes = false;
  RowOutputOptions output_opts;
  output_opts.format = ROW_OUTPUT_PIPE;
  output_opts.include_meta = false;
  output_opts.out = nullptr;

  for (int i = 3; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--with-meta") {
      output_opts.include_meta = true;
      continue;
    }
    if (arg == "--list-indexes") {
      list_indexes = true;
      continue;
    }
    if (arg.rfind("--index=", 0) == 0) {
      index_selector = arg.substr(std::strlen("--index="));
      index_selector_explicit = true;
      continue;
    }
    if (arg == "--index") {
      if (i + 1 >= argc) {
        std::cerr << "--index requires a value\n";
        return 1;
      }
      index_selector = argv[++i];
      index_selector_explicit = true;
      continue;
    }
    if (arg.rfind("--format=", 0) == 0) {
      std::string fmt = arg.substr(std::strlen("--format="));
      if (fmt == "pipe") {
        output_opts.format = ROW_OUTPUT_PIPE;
      } else if (fmt == "csv") {
        output_opts.format = ROW_OUTPUT_CSV;
      } else if (fmt == "jsonl") {
        output_opts.format = ROW_OUTPUT_JSONL;
      } else {
        std::cerr << "Unknown format: " << fmt << "\n";
        return 1;
      }
      continue;
    }
    if (arg == "--format") {
      if (i + 1 >= argc) {
        std::cerr << "--format requires a value\n";
        return 1;
      }
      std::string fmt = argv[++i];
      if (fmt == "pipe") {
        output_opts.format = ROW_OUTPUT_PIPE;
      } else if (fmt == "csv") {
        output_opts.format = ROW_OUTPUT_CSV;
      } else if (fmt == "jsonl") {
        output_opts.format = ROW_OUTPUT_JSONL;
      } else {
        std::cerr << "Unknown format: " << fmt << "\n";
        return 1;
      }
      continue;
    }
    if (arg.rfind("--output=", 0) == 0) {
      out_path = argv[i] + std::strlen("--output=");
      continue;
    }
    if (arg == "--output") {
      if (i + 1 >= argc) {
        std::cerr << "--output requires a path\n";
        return 1;
      }
      out_path = argv[++i];
      continue;
    }
    if (arg.rfind("--lob-max-bytes=", 0) == 0) {
      const char* value = argv[i] + std::strlen("--lob-max-bytes=");
      output_opts.lob_max_bytes = static_cast<size_t>(std::strtoull(value, nullptr, 10));
      continue;
    }
    if (arg == "--lob-max-bytes") {
      if (i + 1 >= argc) {
        std::cerr << "--lob-max-bytes requires a value\n";
        return 1;
      }
      output_opts.lob_max_bytes =
          static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
      continue;
    }
    std::cerr << "Unknown argument: " << arg << "\n";
    return 1;
  }

  parser_context_t parser_ctx;

  // 0) Load table definition and extract table name
  std::string table_name;
  if (load_ib2sdi_table_columns(json_file, table_name, &parser_ctx) != 0) {
    std::cerr << "Failed to load table columns from JSON.\n";
    return 1;
  }

  if (list_indexes) {
    print_sdi_indexes(stdout);
    return 0;
  }

  if (has_sdi_index_definitions()) {
    std::string err;
    if (!select_index_for_parsing(&parser_ctx, index_selector, &err)) {
      std::cerr << "Index selection failed: " << err << "\n";
      return 1;
    }
  } else if (index_selector_explicit) {
    std::cerr << "Index selection requires SDI index metadata.\n";
    return 1;
  }

  // Build a table_def_t from g_columns
  static table_def_t my_table;
  if (build_table_def_from_json(&my_table, table_name.c_str()) != 0) {
    std::cerr << "Failed to build table_def_t from JSON.\n";
    return 1;
  }

  // If your code uses the n_nullable field in ibrec_init_offsets_new():
  my_table.n_nullable = 0;
  for (int i = 0; i < my_table.fields_count; i++) {
    if (my_table.fields[i].can_be_null) my_table.n_nullable++;
  }
    // 1) MySQL init
  my_init();
  my_thread_init();

  // 2) Resolve selected index ID using system "open + pread" approach
  int sys_fd = ::open(in_file, O_RDONLY);
  if (sys_fd < 0) {
    perror("open");
    return 1;
  }

  if (!target_index_is_set(&parser_ctx)) {
    page_no_t root = selected_index_root(&parser_ctx);
    uint64_t idx_id = 0;
    if (root != FIL_NULL && read_index_id_from_root(sys_fd, root, &idx_id)) {
      set_target_index_id_from_value(&parser_ctx, idx_id);
    }
  }

  if (!target_index_is_set(&parser_ctx)) {
    if (index_selector_explicit && has_sdi_index_definitions()) {
      std::cerr << "Could not resolve index id for selected index '"
                << selected_index_name(&parser_ctx) << "'.\n";
      ::close(sys_fd);
      my_thread_end();
      my_end(0);
      return 1;
    }
    if (discover_target_index_id(sys_fd, &parser_ctx) != 0) {
      std::cerr << "Could not discover index from " << in_file << std::endl;
      ::close(sys_fd);
      my_thread_end();
      my_end(0);
      return 1;
    }
  }

  // 3) Now open the file with MySQL's my_open
  File in_fd = my_open(in_file, O_RDONLY, MYF(0));
  if (in_fd < 0) {
    std::cerr << "Cannot open file " << in_file << std::endl;
    ::close(sys_fd);
    my_thread_end();
    my_end(0);
    return 1;
  }

  // 4) Determine page size (for 8KB,16KB, etc.)
  page_size_t pg_sz(0, 0, false);
  if (!determine_page_size(in_fd, pg_sz)) {
    std::cerr << "Could not determine page size from " << in_file << "\n";
    my_close(in_fd, MYF(0));
    ::close(sys_fd);
    my_thread_end();
    my_end(0);
    return 1;
  }
  const size_t physical_page_size = pg_sz.physical();
  const size_t logical_page_size = pg_sz.logical();
  const bool tablespace_compressed = (physical_page_size < logical_page_size);

  LobReadContext lob_ctx;
  lob_ctx.fd = sys_fd;
  lob_ctx.physical_page_size = physical_page_size;
  lob_ctx.logical_page_size = logical_page_size;
  lob_ctx.tablespace_compressed = tablespace_compressed;
  set_lob_read_context(lob_ctx);

  // 5) Rewind
  if (my_seek(in_fd, 0, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
    std::cerr << "Cannot seek to start of " << in_file << "\n";
    my_close(in_fd, MYF(0));
    ::close(sys_fd);
    my_thread_end();
    my_end(0);
    return 1;
  }

  // 6) Overwrite table_definitions[] with your new table and init table defs
  table_definitions[0] = my_table;
  table_definitions_cnt = 1;
  init_table_defs(1);
  if (parser_debug_enabled()) {
    debug_print_table_def(&my_table);
  }

  FILE* out_file = nullptr;
  if (out_path && *out_path) {
    out_file = std::fopen(out_path, "wb");
    if (!out_file) {
      std::cerr << "Cannot open output file " << out_path << "\n";
      my_close(in_fd, MYF(0));
      ::close(sys_fd);
      my_thread_end();
      my_end(0);
      return 1;
    }
    output_opts.out = out_file;
  }
  set_row_output_options(output_opts);

  // 7) Allocate buffers
  std::unique_ptr<unsigned char[]> page_buf(new unsigned char[physical_page_size]);
  std::unique_ptr<unsigned char[]> logical_buf;
  if (tablespace_compressed) {
    logical_buf.reset(new unsigned char[logical_page_size]);
  }
  std::unique_ptr<unsigned char[]> xdes_scratch(new unsigned char[physical_page_size]);
  XdesCache xdes_cache;

  auto load_xdes_page = [&](page_no_t target) -> void {
    const page_no_t xdes_page = xdes_calc_descriptor_page(pg_sz, target);
    if (xdes_page == FIL_NULL || xdes_cache.page_no == xdes_page) {
      return;
    }
    const off_t offset = static_cast<off_t>(xdes_page) *
                         static_cast<off_t>(physical_page_size);
    const ssize_t read_bytes = pread(sys_fd, xdes_scratch.get(),
                                     physical_page_size, offset);
    if (read_bytes == static_cast<ssize_t>(physical_page_size)) {
      xdes_cache.update(xdes_page, xdes_scratch.get(), physical_page_size);
    }
  };

  // 8) Page-by-page loop
  uint64_t page_no = 0;
  while (true) {
    size_t rd = my_read(in_fd, page_buf.get(), physical_page_size, MYF(0));
    if (rd == 0) {
      // EOF
      break;
    }
    if (rd < physical_page_size) {
      std::cerr << "Warning: partial page read at page " << page_no << "\n";
      break;
    }

    const uint32_t on_disk_page_no =
        mach_read_from_4(page_buf.get() + FIL_PAGE_OFFSET);
    if (on_disk_page_no != page_no) {
      page_no++;
      continue;
    }

    const uint16_t page_type = mach_read_from_2(page_buf.get() + FIL_PAGE_TYPE);
    if (page_type == FIL_PAGE_TYPE_XDES ||
        page_type == FIL_PAGE_TYPE_FSP_HDR) {
      xdes_cache.update(page_no, page_buf.get(), physical_page_size);
    }

    load_xdes_page(page_no);
    if (xdes_cache.is_free(page_no, pg_sz)) {
      page_no++;
      continue;
    }

    if (page_type != FIL_PAGE_INDEX) {
      page_no++;
      continue;
    }

    const unsigned char* parse_buf = page_buf.get();
    size_t parse_size = physical_page_size;
    if (tablespace_compressed) {
      size_t actual_size = 0;
      if (!decompress_page_inplace(page_buf.get(),
                                   physical_page_size,
                                   logical_page_size,
                                   logical_buf.get(),
                                   logical_page_size,
                                   &actual_size)) {
        page_no++;
        continue;
      }
      if (actual_size != logical_page_size) {
        page_no++;
        continue;
      }
      parse_buf = logical_buf.get();
      parse_size = logical_page_size;
    }

    if (!page_is_comp(parse_buf)) {
      page_no++;
      continue;
    }

    parse_records_on_page(parse_buf, parse_size, page_no, &parser_ctx);

    page_no++;
  }

  if (out_file) {
    std::fclose(out_file);
  }

  my_close(in_fd, MYF(0));
  ::close(sys_fd);
  my_thread_end();
  my_end(0);

  std::cout << "Parse-only complete. Pages read: " << page_no << "\n";
  return 0;
}

/**
 * (D) The "decrypt + decompress" combined logic in a single pass,
 *     adapted from what we did in "combined_decrypt_decompress.cc".
 *     Page by page => decrypt_page_inplace => decompress_page_inplace => write final.
 */
static int do_decrypt_then_decompress_main(int argc, char** argv)
{
  if (argc < 6) {
    std::cerr << "Usage for mode=4 (decrypt+decompress):\n"
              << "  ib_parser 4 <master_key_id> <server_uuid> <keyring_file> "
              << "<ibd_path> <dest_path>\n";
    return 1;
  }

  uint32_t master_id       = static_cast<uint32_t>(std::atoi(argv[1]));
  std::string srv_uuid     = argv[2];
  const char* keyring_path = argv[3];
  const char* ibd_path     = argv[4];
  const char* out_file     = argv[5];

  // -----------------------------
  // (A) Global MySQL / OpenSSL init
  // -----------------------------
  my_init();
  my_thread_init();
  OpenSSL_add_all_algorithms();

  // -----------------------------
  // (B) Retrieve the master key
  // -----------------------------
  std::vector<unsigned char> master_key;
  if (!get_master_key(master_id, srv_uuid, keyring_path, master_key)) {
    std::cerr << "Could not get master key\n";
    return 1;
  }

  // (C) Open .ibd for reading so we can see if it's compressed
  File in_fd = my_open(ibd_path, O_RDONLY, MYF(0));
  if (in_fd < 0) {
    std::cerr << "Cannot open file " << ibd_path << std::endl;
    return 1;
  }

  bool compressed = is_table_compressed(in_fd);
  my_close(in_fd, MYF(0));

  // -----------------------------
  // (C.1) Read the tablespace key/IV
  //     If you already know the file is "encrypted & compressed" => offset=5270.
  //     Or if you know it's "encrypted & uncompressed" => offset=10390.
  //  5270 (0x1496) => For "encrypted & compressed"  (8 KB physical, etc.)
  // 10390 (0x2896) => For "encrypted & uncompressed" (16 KB pages).
  // -----------------------------
  long offset = compressed ? 5270 : 10390;
  Tablespace_key_iv ts_key_iv;
  if (!read_tablespace_key_iv(ibd_path, offset, master_key, ts_key_iv)) {
    std::cerr << "Could not read tablespace key\n";
    return 1;
  }

  // -----------------------------
  // (D) Open input file with MySQL I/O or system I/O
  //     so we can run "determine_page_size()"
  // -----------------------------
  in_fd = my_open(ibd_path, O_RDONLY, MYF(0));
  if (in_fd < 0) {
    std::cerr << "Cannot open input file " << ibd_path << "\n";
    return 1;
  }

  // (D.1) Figure out the actual page size by reading the FSP header from page 0
  page_size_t pg_sz(0, 0, false);
  if (!determine_page_size(in_fd, pg_sz)) {
    std::cerr << "Could not determine page size from " << ibd_path << "\n";
    my_close(in_fd, MYF(0));
    return 1;
  }

  // We now know the physical page size => e.g. 8192 or 16384, etc.
  const size_t physical_page_size = pg_sz.physical();
  const size_t logical_page_size  = pg_sz.logical();

  // -----------------------------
  // (E) Reopen with FILE* APIs, or rewind using my_seek()
  //     For clarity, let's just close the File handle and use std::fopen.
  // -----------------------------
  my_close(in_fd, MYF(0)); // close
  FILE* fin = std::fopen(ibd_path, "rb");
  if (!fin) {
    std::cerr << "Cannot reopen input " << ibd_path << "\n";
    return 1;
  }
  FILE* fout = std::fopen(out_file, "wb");
  if (!fout) {
    std::cerr << "Cannot open output " << out_file << "\n";
    std::fclose(fin);
    return 1;
  }

  // -----------------------------
  // (F) Allocate buffers based on actual page size
  // -----------------------------
  std::unique_ptr<unsigned char[]> page_buf(new unsigned char[physical_page_size]);
  std::unique_ptr<unsigned char[]> final_buf(new unsigned char[logical_page_size]);

  // -----------------------------
  // (G) Page-by-page loop
  // -----------------------------
  uint64_t page_number = 0;
  while (true) {
    // Read exactly 'physical_page_size' from the file
    size_t rd = std::fread(page_buf.get(), 1, physical_page_size, fin);
    if (rd == 0) {
      // EOF
      break;
    }
    if (rd < physical_page_size) {
      std::cerr << "Warning: partial page read at page " 
                << page_number << "\n";
      // We can either break or continue with partial data
      // But usually we break because it's incomplete
      break;
    }

    // 1) Decrypt in-place
    bool dec_ok = decrypt_page_inplace(
        page_buf.get(), 
        physical_page_size,   // or logical_page_size, depends on your encryption
        ts_key_iv.key, 
        32, 
        ts_key_iv.iv, 
        8 * 1024);
    if (!dec_ok) {
      std::cerr << "Decrypt failed on page " << page_number << "\n";
      std::fclose(fin);
      std::fclose(fout);
      return 1;
    }

    // 2) Decompress in-place (if needed)
    size_t actual_page_size = 0;
    bool cmp_ok = decompress_page_inplace(
        page_buf.get(),          /* src data        */
        physical_page_size,      /* physical_size   */
        logical_page_size,       /* logical_size    */
        final_buf.get(),         /* output buffer   */
        logical_page_size,       /* out_buf_len     */
        &actual_page_size        /* actual size used */
    );
    if (!cmp_ok) {
      std::cerr << "Decompress failed on page " << page_number << "\n";
      std::fclose(fin);
      std::fclose(fout);
      return 1;
    }

    // 3) Write out the final processed page
    size_t wr = std::fwrite(final_buf.get(), 1, actual_page_size, fout);
    if (wr < actual_page_size) {
      std::cerr << "Failed to write final page " << page_number << "\n";
      std::fclose(fin);
      std::fclose(fout);
      return 1;
    }

    page_number++;
  }

  std::fclose(fin);
  std::fclose(fout);

  std::cout << "Decrypt+Decompress done. " << page_number 
            << " pages written.\n";
  my_thread_end();
  my_end(0);
  return 0;
}

/**
 * The single main() that decides which path to use based on "mode".
 */
int main(int argc, char** argv)
{
  if (argc < 2) {
    usage();
    return 1;
  }

  int mode = std::atoi(argv[1]);
  switch (mode) {
    case 1:  // decrypt only
      return do_decrypt_main(argc - 1, &argv[1]);
    case 2:  // decompress only
      return do_decompress_main(argc - 1, &argv[1]);
    case 3:  // parse-only
      return do_parse_main(argc - 1, &argv[1]);
    case 4:  // decrypt + decompress
      return do_decrypt_then_decompress_main(argc - 1, &argv[1]);
    case 5:  // rebuild to uncompressed
      return do_rebuild_uncompressed_main(argc - 1, &argv[1]);
    default:
      std::cerr << "Error: invalid mode '" << mode << "'\n";
      usage();
      return 1;
  }
}
