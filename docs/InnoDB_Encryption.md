## 1) Key hierarchy and where the keys live

**a) Master key (in the keyring)**

- Your code loads the master key from the MySQL keyring file using the official keyring interfaces (`Keys_container`, `Buffered_file_io`). It constructs the InnoDB key name (`"INNODBKey-<server_uuid>-<master_key_id>"`) and fetches the raw bytes.
- The keyring file stores key bytes **obfuscated**; you “de‑obfuscate” by XORing with the fixed ASCII string `*305=Ljt0*!@$Hnm(*-9-w;:` (your `keyring_deobfuscate`). 

**b) Tablespace (per‑.ibd) key + IV (in the .ibd header)**

- Near the beginning of each encrypted tablespace, InnoDB stores a small *encryption info* record. Your reader recognizes versions by the 3‑byte magic: `"lCA"`/`"lCB"`/`"lCC"` ⇒ V1/V2/V3. It then reads a 64‑byte blob and a 4‑byte checksum.
- That 64‑byte blob is **(tablespace key || tablespace IV)** encrypted with the **master key using AES‑256‑ECB (no padding)**. Your code decrypts it with `my_aes_decrypt(..., my_aes_256_ecb, nullptr, false)` and verifies the 4‑byte **CRC‑32C** using `mysql_crc32c`. If the checksum matches, it splits the plaintext into **32‑byte key** and **32‑byte IV**.

> Summary so far: **Master key (keyring) → decrypts 64‑byte header → yields tablespace key + IV**, which are the inputs to page decryption.

------

## 2) How your code decides “this page is encrypted”

You check the on‑page `FIL_PAGE_TYPE` field. If it equals one of:

- `FIL_PAGE_ENCRYPTED`
- `FIL_PAGE_ENCRYPTED_RTREE`
- `FIL_PAGE_COMPRESSED_AND_ENCRYPTED`
   then you treat the page as encrypted. (InnoDB overwrites the visible page type when it encrypts and saves the “original type” in a side field so it can be restored after decryption.) 

------

## 3) The page decryption algorithm (what bytes, what mode, and why that odd “last two blocks” trick)

Your workhorse is `decrypt_page_inplace(...)`. Its behavior depends on the page type: 

1. **Which region of the page is encrypted?**
   - You skip the InnoDB file header (`FIL_PAGE_DATA` offset) and decrypt the payload that follows. 
   - If the page is **compressed + encrypted** (`FIL_PAGE_COMPRESSED_AND_ENCRYPTED`), you first parse the page’s compression mini‑header to get the **compressed size** (`z_len`). The encrypted length becomes `FIL_PAGE_DATA + z_len`, optionally **aligned to the OS block size** for V1, with a **minimum encrypt length of 64 bytes**. 
2. **Cipher and parameters**
   - You decrypt using **AES‑256‑CBC with no padding**, via `my_aes_decrypt(..., my_aes_256_cbc, iv, false)`, where the **key** and **IV** are the per‑tablespace values recovered from the .ibd header. 
3. **Handling non‑block‑aligned tails (“partial‑block” logic)**
    AES‑CBC requires block‑multiple lengths, but pages may encrypt lengths that aren’t multiples of 16. To handle this **without padding**, your code reproduces InnoDB’s trick:
   - Split the payload into `main_len = floor(payload/16)*16` and a remainder.
   - If there is a remainder, **force‑decrypt the last \*two\* blocks (32 bytes) first** into a temp buffer, then decrypt the aligned prefix. Finally, splice the decrypted tail back into place.
      This matches the “decrypt last 2 blocks as a unit” approach InnoDB uses to make a pad‑less CBC layout work offline. 
4. **Restoring the page type**
   - After decryption:
     - `FIL_PAGE_ENCRYPTED` → restore the original page type from `FIL_PAGE_ORIGINAL_TYPE_V1`.
     - `FIL_PAGE_ENCRYPTED_RTREE` → set type to `FIL_PAGE_RTREE`.
     - `FIL_PAGE_COMPRESSED_AND_ENCRYPTED` → set type to `FIL_PAGE_COMPRESSED` (still compressed — see §4). 

------

## 4) Relationship to compression (what comes first, and what you do after decryption)

There are **two distinct compression features** you’ll encounter on disk:

1. **Table compression (page‑zip)** — only B‑tree index pages are stored in a compact “page‑zip” layout; they must be decompressed to the logical page size before you can read records.
2. **Transparent page compression** — the whole page (header+body) is deflated/LZ4’ed and framed.

When a page is **both compressed and encrypted**, the **write‑time order** is:

> **compress → encrypt**

Your read‑time code does the reverse:

> **decrypt → decompress**

Evidence in your implementation:

- For `FIL_PAGE_COMPRESSED_AND_ENCRYPTED`, you **compute the encrypted length from the compressed size**, decrypt that region, and then **flip the page type back to `FIL_PAGE_COMPRESSED`** rather than the final logical type — signaling that a **separate decompressor pass must follow**. 
- In other words, decryption reveals the **still‑compressed** bytes; then you run your decompressor (page‑zip for index pages, or the transparent‑compression inflater) to get the full logical page. (This is exactly why your decryption function does **not** inflate anything itself.) 

------

## 5) End‑to‑end pipeline (what your tool actually does)

1. **Load master key** from keyring → **de‑obfuscate** → bytes in memory.
2. **Read the .ibd encryption info** (magic + metadata + 64‑byte blob + CRC).
   - **AES‑256‑ECB decrypt** the 64‑byte blob with master key → **tablespace key + IV**.
   - **CRC‑32C verify** the 64‑byte plaintext.
3. For **each page**:
   - If not encrypted → copy (or later, decompress if `FIL_PAGE_COMPRESSED`).
   - If **encrypted**:
     - Compute encrypted region (special handling when compressed+encrypted).
     - **AES‑256‑CBC (no padding) decrypt** that region using **tablespace key + IV** (with the “last‑two‑blocks” trick when needed).
     - Restore page type (e.g., `ENCRYPTED` → original; `COMPRESSED_AND_ENCRYPTED` → `COMPRESSED`). 
   - If the resulting page is **compressed**, run the appropriate **decompressor** next (outside this decryption routine). 

Your top‑level file routine (`decrypt_ibd_file`) applies this page by page and writes out the decrypted pages; it chooses a page size (8 KiB for your “compressed” case vs. 16 KiB otherwise) purely for I/O chunking during this offline pass. 

------

## 6) Version nuances your code already handles

- **Header versions**: V1/V2/V3 magics; V2/V3 include a **server UUID** in the header (your reader consumes 36 bytes for UUID in V2/V3). 
- **Master key ID = 0 (old 5.7 corner)**: you adjust how many bytes to skip before the 64‑byte block. 
- **V1 alignment**: for compressed+encrypted pages with header version 1, you **align the encrypted length to the OS block size** (your call passes `8*1024`). Also enforce a **minimum 64 bytes** encrypted length. 

------

## 7) Practical tips & pitfalls

- **Don’t decompress before decrypting.** If the page is `COMPRESSED_AND_ENCRYPTED`, what you see on disk (after the InnoDB file header) is **encrypted compressed data**. You must **decrypt first**, then pass the resulting `COMPRESSED` page to your decompressor. Your implementation already does exactly this. 
- **Sanity checks help**: You already verify the **CRC‑32C** of the 64‑byte key/IV block; after page‑level decrypt+decompress, you can also validate page checksums using standard InnoDB page checksum routines (not shown in these files).
- **Key/IV usage**: Your current path uses the **tablespace key** and **tablespace IV** directly with AES‑CBC for the data region. That matches the pages produced by the same scheme your tool targets. If you ever hit decryption mismatches on other servers/builds, check whether the IV is further derived (e.g., mixed with the page number) in that specific build; your current code expects the IV from the header to be sufficient. 

------

## 8) A compact “decision table” for pages (read path)

| On‑disk page type you see                     | Step 1                                  | Step 2                                                       | Final state after step 2              |
| --------------------------------------------- | --------------------------------------- | ------------------------------------------------------------ | ------------------------------------- |
| `FIL_PAGE_ENCRYPTED`                          | Decrypt payload with tablespace key+IV  | Restore original type from `FIL_PAGE_ORIGINAL_TYPE_V1`       | Normal (uncompressed) page            |
| `FIL_PAGE_COMPRESSED_AND_ENCRYPTED`           | Decrypt **compressed length** (aligned) | Change type → `FIL_PAGE_COMPRESSED`                          | Still compressed → **now decompress** |
| `FIL_PAGE_COMPRESSED`                         | (No decryption)                         | Decompress                                                   | Normal page                           |
| Other (`FIL_PAGE_INDEX`, etc.), not encrypted | (No decryption)                         | (If table‑compressed, you wouldn’t see these directly; otherwise nothing) | Normal page                           |

------

### Bottom line

- **Encryption and compression are orthogonal layers.**
- When both apply, **InnoDB writes `compress → encrypt`**; your offline reader must **`decrypt → decompress`**.
- Your implementation gets the keys right (keyring → tablespace key/IV), chooses the correct **cipher modes** (ECB for the 64‑byte header wrap; CBC for page data), handles **non‑aligned payloads**, and correctly **restores page types** so that the decompressor can run next.

If you want, I can sketch a clean “post‑decrypt” decompression pass for both **page‑zip** (index pages) and **transparent page compression** (whole‑page deflate/LZ4) so you can chain it directly after `decrypt_page_inplace(...)`.