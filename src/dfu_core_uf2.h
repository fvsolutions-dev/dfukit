#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// UF2 parser layered on top of dfu_core. Accepts a byte stream (which may
// arrive in arbitrary chunk sizes), reassembles 512-byte UF2 blocks,
// validates the start/end magic words, and **address-dispatches** each
// block's payload into the inactive OTA partition using the block's
// `target_addr` field.
//
// The parser is built for the hostile case: USB-MSC drag-drop, where the
// host filesystem (notably Linux FAT writeback) delivers UF2 data blocks
// out of order, duplicated, and interleaved with filesystem metadata,
// with no clean stream-start or EOF signal. To survive that:
//   - block_num is the identity, not the position. A per-block-number
//     bitmap tracks which blocks have been WRITTEN; completion fires when
//     every distinct block in [0, numBlocks) has been seen exactly once.
//   - duplicates are dropped (a re-written block_num isn't re-flashed and
//     doesn't double-count) — both idempotent and correct.
//   - a sector that fails validation is skipped, never fatal: the 4-byte
//     magic sniff upstream is weak, so a coincidental / torn match must
//     not tear down an in-flight transfer.

#define DFU_CORE_UF2_BLOCK_SIZE     512
#define DFU_CORE_UF2_MAGIC_START0   0x0A324655u   // "UF2\n"
#define DFU_CORE_UF2_MAGIC_START1   0x9E5D5157u
#define DFU_CORE_UF2_MAGIC_END      0x0AB16F30u

// Bitmap cap on numBlocks. 16384 blocks covers a 1.5 MB OTA partition (plus
// the bootloader-region blocks of a combined uf2.bin) even at small UF2
// payload sizes, for 2 KB of parser state. Streams declaring more fall back
// to counter-based completion (best effort; logged).
#define DFU_CORE_UF2_MAX_BLOCKS     16384

typedef struct {
    uint8_t  block[DFU_CORE_UF2_BLOCK_SIZE];
    size_t   in_block;          // bytes filled in the current block
    uint32_t total_blocks;      // from numBlocks field; 0 until first block
    uint32_t seen_blocks;       // distinct blocks written (popcount of seen)
    uint32_t family_id;         // captured for diagnostics
    bool     started;           // partition handles resolved
    bool     complete;          // all blocks seen + set_boot_partition ok
    bool     no_bitmap;         // numBlocks > MAX_BLOCKS: counter fallback
    uint8_t  seen[DFU_CORE_UF2_MAX_BLOCKS / 8];  // block_num -> written bit
} dfu_core_uf2_parser_t;

// Reset parser state. Call this before each new download.
esp_err_t dfu_core_uf2_init(dfu_core_uf2_parser_t *p);

// Feed an arbitrary chunk. Calls dfu_core_begin lazily on the first valid
// block, dfu_core_write per block, and dfu_core_finish on the last.
esp_err_t dfu_core_uf2_feed(dfu_core_uf2_parser_t *p, const void *data, size_t len);

bool dfu_core_uf2_complete(const dfu_core_uf2_parser_t *p);

// Helper: returns true if buf[0..3] starts with the UF2 magic so callers
// can sniff the format from the head of the first chunk.
bool dfu_core_uf2_looks_like_uf2(const void *buf, size_t len);

#ifdef __cplusplus
}
#endif
