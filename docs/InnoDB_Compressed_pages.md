## 1) Big picture: what kinds of “compressed” InnoDB pages exist?

InnoDB has **two different features** that produce compact/on‑disk pages:

1. **Table compression** (`ROW_FORMAT=COMPRESSED`, with `KEY_BLOCK_SIZE` 1K/2K/4K/8K).
   - **Only B‑tree index pages** are stored in a special compressed representation (“page zip”) and must be decompressed to the full logical page size when you want to read them. InnoDB implements this with **zlib (LZ77)** and a custom page layout around it. [MySQL](https://dev.mysql.com/doc/refman/en/innodb-compression-internals.html)
2. **Transparent page compression** (`COMPRESSION="zlib"` or `"lz4"`).
   - Here, **any page type** can be compressed “as a whole” (header + body) and written to disk shorter, with page type set to **`FIL_PAGE_COMPRESSED` (14)** (or “compressed+encrypted”). Supported algorithms include **Zlib** and **LZ4**. [MySQL+1](https://dev.mysql.com/doc/en/innodb-page-compression.html?utm_source=chatgpt.com)

Your file focuses on **table compression** (the page‑zip format used for **index pages**). That’s why it calls the InnoDB routine `page_zip_decompress_low()`, which is the official, externalizable entry point Oracle exposes for tools. (More on that below.)

------

## 2) The flow in your code (function by function)

### 2.1 Determining the page geometry

```
bool determine_page_size(File file_in, page_size_t& page_sz) { … }
```

- Reads a small prefix of page 0 (the fsp header) to discover the tablespace’s **logical page size** and **physical size** (for compressed tables: physical < logical).
- Sets globals like `srv_page_size`, `srv_page_size_shift`, and `univ_page_size`, then copies the result into the `page_size_t` you pass around.
- This gives you:
  - `page_sz.logical()` → the **uncompressed** page size (typically 16KiB unless `innodb_page_size` is smaller).
  - `page_sz.physical()` → the **on‑disk** size per page for this tablespace (for compressed b‑tree pages this is 1/2/4/8 KiB; for uncompressed pages equals `logical()`).

> Conceptually: every compressed index page on disk corresponds to one uncompressed, logical page in memory (e.g., 8KiB on disk ↔ 16KiB in memory). [MySQL](https://dev.mysql.com/doc/refman/en/innodb-compression-internals.html)

------

### 2.2 Deciding if a page is “compressed”

```
bool is_page_compressed(const unsigned char* page_data,
                        size_t physical_size, size_t logical_size) {
  if (physical_size < logical_size) return true;
  uint16_t t = mach_read_from_2(page_data + FIL_PAGE_TYPE);
  return t == FIL_PAGE_COMPRESSED || t == FIL_PAGE_COMPRESSED_AND_ENCRYPTED;
}
```

- **Primary test**: if `physical_size < logical_size` then it’s a **table‑compressed** b‑tree page (page‑zip).
- **Secondary test**: if the **page type** is `FIL_PAGE_COMPRESSED` (14) or `FIL_PAGE_COMPRESSED_AND_ENCRYPTED` (16), then it’s a **transparent page compression** page. (Your current decompressor does **not** expand these; see §4.) [MySQL](https://dev.mysql.com/doc/dev/mysql-server/9.2.0/fil0fil_8h_source.html?utm_source=chatgpt.com)

------

### 2.3 The workhorse: `decompress_page_inplace(...)`

```
bool decompress_page_inplace(const unsigned char* src_buf,
                             size_t physical_size,
                             bool is_compressed,
                             unsigned char* out_buf,
                             size_t out_buf_len,
                             size_t logical_size) {
  if (!is_compressed) { memcpy(out_buf, src_buf, physical_size); return true; }

  /* read the page type */
  uint16_t page_type = mach_read_from_2(src_buf + FIL_PAGE_TYPE);

  /* temp aligned buffer for the uncompressed 16KiB (or logical) page */
  unsigned char* temp = (unsigned char*) ut::malloc(2 * logical_size);
  unsigned char* aligned = (unsigned char*) ut_align(temp, logical_size);
  memset(aligned, 0, logical_size);

  /* build the page_zip descriptor pointing at the compressed bytes */
  page_zip_des_t page_zip;
  page_zip_des_init(&page_zip);
  page_zip.data  = reinterpret_cast<page_zip_t*>(const_cast<unsigned char*>(src_buf));
  page_zip.ssize = page_size_to_ssize(physical_size);

  bool ok = false;
  if (page_type == FIL_PAGE_INDEX) {
    ok = page_zip_decompress_low(&page_zip, aligned, /*all=*/true);
    if (ok) memcpy(out_buf, aligned, logical_size);
  } else {
    /* not an index page → just copy (no decode implemented here) */
    memcpy(out_buf, src_buf, physical_size);
    ok = true;
  }
  ut::free(temp);
  return ok;
}
```

What’s happening here:

- If the page is **uncompressed** → just copy it out.
- If **compressed**:
  1. It checks `FIL_PAGE_TYPE`. For **`FIL_PAGE_INDEX`** (the value for B‑tree index pages), it uses **InnoDB’s own decompressor**:
     - Prepare a `page_zip_des_t` descriptor:
       - `page_zip.data` points at your on‑disk compressed page.
       - `page_zip.ssize` is the compressed page size encoded in the InnoDB‑expected “shift size” form (helper `page_size_to_ssize()` maps 1024/2048/4096/8192 to the internal code).
     - Call **`page_zip_decompress_low(&page_zip, aligned, true)`**, which:
       - Inflates the page‑zip payload with **zlib** and reconstructs a **full logical page image** (16KiB or whatever the tablespace’s logical size is), including page header, records, page directory, etc.
       - Returns `true`/`false` instead of asserting on malformed pages (specifically designed for external tools). [Fossies](https://fossies.org/dox/mysql-cluster-gpl-8.4.6/zipdecompress_8h.html)
     - Copy the decompressed 16KiB (or logical) into `out_buf`.
  2. If the page type is **not** `FIL_PAGE_INDEX`, the function **does not attempt** to decode it (it just copies the raw page). This is fine for many non‑index page types (which are written uncompressed in table compression), **but** it also means **transparent page compression pages** aren’t expanded here. See §4 for how to handle those if you need to.

> Key point: the heavy lifting is **not** your code; it’s `page_zip_decompress_low()` from InnoDB’s decompression library. Oracle exposes it exactly so tools like yours can safely decode compressed index pages without dragging in the full server. [Oracle Docs](https://docs.oracle.com/cd/E17952_01/mysql-8.0-relnotes-en/news-8-0-0.html)

------

### 2.4 Reading and writing pages

- `fetch_page(...)` seeks to page *N*, reads **`page_sz.physical()`** bytes, calls `is_page_compressed(...)` and then `decompress_page_inplace(...)`, filling a **`page_sz.logical()`**‑sized output buffer with the uncompressed page image (or a copy for uncompressed pages).
- `decompress_ibd(...)` iterates over all pages in the input tablespace and writes each **uncompressed logical page** to the output.

------

## 3) About `page_zip_decompress_low()`: what it expects & guarantees

- **Purpose**: “Decompress a page” for external tools; tolerate bad/corrupt input by returning `false` rather than firing assertions.
- **Signature**:
   `bool page_zip_decompress_low(page_zip_des_t* page_zip, ib_page_t* page, bool all);`
   where:
  - `page_zip->data` points at the compressed page bytes,
  - `page_zip->ssize` identifies the compressed page size class,
  - `page` points at a buffer of **logical page size**,
  - `all=true` means “decompress the whole page”.
- **Availability**: As of MySQL 8.0, Oracle ships `libinnodb_zipdecompress.a`, explicitly so external tools can link this without the full server. [Fossies](https://fossies.org/dox/mysql-cluster-gpl-8.4.6/zipdecompress_8h.html)[Oracle Docs](https://docs.oracle.com/cd/E17952_01/mysql-8.0-relnotes-en/news-8-0-0.html)

------

## 4) Table compression vs. Transparent page compression (why it matters to your tool)

Your current code handles **table compression** (the classic page‑zip representation of **index** pages). It does **not** actually expand **transparent page compression** pages (`FIL_PAGE_COMPRESSED` / `…_AND_ENCRYPTED`) — it merely detects them as “compressed” and then **falls back to a raw copy** unless the page type also happens to be `FIL_PAGE_INDEX`.

If you want your output file to contain **fully expanded 16KiB pages for \*both\* features**, you must add **another branch**:

- For **page‑compressed** pages (page type = `FIL_PAGE_COMPRESSED`), read the small header that records **algorithm** and **compressed length**, then:
  - If algorithm = **zlib**, inflate the **entire page payload** to the logical size using zlib.
  - If algorithm = **lz4**, use LZ4’s decoder likewise.
     These pages are different from table‑compressed pages: they’re essentially “whole‑page deflate/LZ4” with a small framing header, not the specialized page‑zip layout used for index pages. [MySQL](https://dev.mysql.com/doc/en/innodb-page-compression.html?utm_source=chatgpt.com)

> In short: keep using `page_zip_decompress_low()` **only** for `FIL_PAGE_INDEX` table‑compressed pages; implement a **separate** inflate/LZ4 path for `FIL_PAGE_COMPRESSED` pages.

------

## 5) Moving this **outside** the MySQL build (recommended approaches)

You mentioned it “works inside MySQL’s make system” and you now want it **out‑of‑tree**.

You have two practical options:

### Option A — Link the official library

- Build or ship **`libinnodb_zipdecompress.a`** (provided by Oracle since 8.0) and include the headers that declare:
  - `page_zip_des_t`, `page_zip_des_init()`, `page_zip_decompress_low()`, and helpers like `page_size_to_ssize()`.
- Your code already matches the **intended usage** pattern. Keep your `page_zip` setup and call the function exactly as you do now.
   *Documentation proof that this is intended for external tools:* the Doxygen for `zipdecompress.h` literally says the code “is used to make a library for external tools,” and points to `page_zip_decompress_low()` as the entry point; the 8.0 release notes explicitly mention the new library for this purpose. [Fossies](https://fossies.org/dox/mysql-cluster-gpl-8.4.6/zipdecompress_8h.html)[Oracle Docs](https://docs.oracle.com/cd/E17952_01/mysql-8.0-relnotes-en/news-8-0-0.html)

### Option B — Vendor the minimal sources

- If you can’t (or won’t) link a prebuilt static library, **vendor** the tiny subset of `storage/innobase/page/zipdecompress.*` and its few header dependencies (Oracle designed it to minimize server dependencies).
- You still need **zlib** at link time.
- This is more work to keep in sync across versions; Option A is cleaner.

> Either way, keep your own `mach_read_from_2()` / endian helpers or reuse InnoDB’s; you already call `mach_read_from_2(page + FIL_PAGE_TYPE)` to read the page type. (That macro reads big‑endian fields on the page.)

------

## 6) Subtleties and gotchas

- **Only index pages are page‑zip compressed** (table compression). Your `if (page_type == FIL_PAGE_INDEX)` guard is **correct**. Other page types (FSP_HDR, IBUF_BITMAP, etc.) remain uncompressed under table compression and can be copied as‑is. (Overflow/BLOB pages under table compression are handled differently; they store their payload zlib‑compressed “as is”, not in the page‑zip structure. Your tool can safely copy those pages; InnoDB inflates their contents when it needs the BLOB.) [MySQL](https://dev.mysql.com/doc/refman/en/innodb-compression-internals.html)
- **Logical vs physical size**: The buffer you pass to `page_zip_decompress_low()` must be **logical page size** long (e.g., 16KiB). Your code allocates and aligns such a buffer correctly before calling the function.
- **Return‑on‑error**: `page_zip_decompress_low()` returns `false` for inconsistencies rather than asserting, exactly to help offline tools deal with corruption gracefully. [Fossies](https://fossies.org/dox/mysql-cluster-gpl-8.4.6/zipdecompress_8h.html)
- **Transparent page compression**: if you decide to support it, you need to parse the page header to discover the algorithm and compressed length, then run **zlib/LZ4** to produce the logical‑size page image. That logic is separate from page‑zip. [MySQL](https://dev.mysql.com/doc/en/innodb-page-compression.html?utm_source=chatgpt.com)

------

## 7) Minimal pseudo‑code for an **out‑of‑tree** tool

```
open(in), open(out);
detect_page_sizes(in) -> logical, physical;

for (page_no = 0; page_no < filesize/physical; ++page_no) {
  read physical bytes -> disk_buf;

  bool compressed = (physical < logical)
                 || (page_type(disk_buf) == FIL_PAGE_COMPRESSED)
                 || (page_type(disk_buf) == FIL_PAGE_COMPRESSED_AND_ENCRYPTED);

  if (!compressed) {
    // normal page
    memcpy(out_buf, disk_buf, logical); // physical==logical
  } else if (page_type(disk_buf) == FIL_PAGE_INDEX) {
    // table-compressed index page
    page_zip_des_t z; page_zip_des_init(&z);
    z.data = (page_zip_t*)disk_buf;
    z.ssize = page_size_to_ssize(physical);
    if (!page_zip_decompress_low(&z, out_buf, true)) { /*handle error*/ }
  } else if (page_type is FIL_PAGE_COMPRESSED or ...ENCRYPTED) {
    // transparent page compression: inspect header -> algo, len
    // use zlib or LZ4 to inflate whole page into out_buf (logical bytes)
  } else {
    // other compressed type you don't decode -> just copy for now
    memcpy(out_buf, disk_buf, physical);
  }

  write(out, out_buf, logical);
}
```

------

## 8) Quick references for the claims above

- **InnoDB uses zlib (LZ77) for table compression**, B‑tree pages decompress to the logical page size in memory; buffer pool can hold both forms. [MySQL](https://dev.mysql.com/doc/refman/en/innodb-compression-internals.html)
- **Transparent page compression** supports **Zlib and LZ4**, page type `FIL_PAGE_COMPRESSED`. [MySQL+1](https://dev.mysql.com/doc/en/innodb-page-compression.html?utm_source=chatgpt.com)
- **External decompression entry point** `page_zip_decompress_low()` and the dedicated **library for tools** (`libinnodb_zipdecompress.a`). [Fossies](https://fossies.org/dox/mysql-cluster-gpl-8.4.6/zipdecompress_8h.html)[Oracle Docs](https://docs.oracle.com/cd/E17952_01/mysql-8.0-relnotes-en/news-8-0-0.html)

------

## 9) What I would change or verify in your program

1. **Be explicit** in logs about *which* compressed feature you handled: log `FIL_PAGE_INDEX (table compression)` vs `FIL_PAGE_COMPRESSED` (transparent page compression).
2. **Add decode for page compression** if you need fully expanded output for those pages (zlib/LZ4).
3. **Fail semantics**: on `page_zip_decompress_low()==false`, decide whether to:
   - emit an all‑zero page,
   - copy the original bytes,
   - or abort (depending on your recovery goal).

------

If you want, paste a specific run (page types you see and sizes) and I’ll sketch the exact decode path for each. But structurally, this is the essence: **your code hands compressed index pages to InnoDB’s own `page_zip_decompress_low()`**, which inflates them into a full logical page using **zlib**, and writes the result. Everything else is bookkeeping around page sizes and page types.