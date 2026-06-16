#include "dfu_core_uf2.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

#include "dfu_core.h"

// These helpers live in dfu_core.c and are intended to be called only from
// the OTA worker task. They translate a UF2 block's target_addr into an
// offset inside the inactive OTA partition (lazily erasing whichever sector
// gets touched first) and finalize via esp_ota_set_boot_partition. The
// parser runs on the worker too, so calling them is safe.
esp_err_t dfu_core_internal_uf2_start(void);
esp_err_t dfu_core_internal_uf2_write(uint32_t target_addr,
                                      const void *payload, size_t len,
                                      bool *out_written);
esp_err_t dfu_core_internal_uf2_finish(void);
void      dfu_core_internal_uf2_abort(void);

static const char *TAG = "dfu_core_uf2";

// UF2 block layout (Microsoft):
//   0   u32  magicStart0  (0x0A324655 "UF2\n")
//   4   u32  magicStart1  (0x9E5D5157)
//   8   u32  flags
//   12  u32  targetAddr
//   16  u32  payloadSize
//   20  u32  blockNo
//   24  u32  numBlocks
//   28  u32  fileSize / familyID
//   32  476  data
//   508 u32  magicEnd     (0x0AB16F30)

#define UF2_FLAG_NOT_MAIN_FLASH      0x00000001u
#define UF2_FLAG_FILE_CONTAINER      0x00001000u
#define UF2_FLAG_FAMILY_ID_PRESENT   0x00002000u
#define UF2_FLAG_MD5_CHECKSUM_PRESENT 0x00004000u

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t dfu_core_uf2_init(dfu_core_uf2_parser_t *p)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    memset(p, 0, sizeof(*p));
    return ESP_OK;
}

bool dfu_core_uf2_complete(const dfu_core_uf2_parser_t *p)
{
    return p && p->complete;
}

bool dfu_core_uf2_looks_like_uf2(const void *buf, size_t len)
{
    if (!buf || len < 4) return false;
    return read_u32_le(buf) == DFU_CORE_UF2_MAGIC_START0;
}

// True once block_no has been written this session (bitmap mode only).
static bool block_seen(const dfu_core_uf2_parser_t *p, uint32_t block_no)
{
    if (p->no_bitmap || block_no >= DFU_CORE_UF2_MAX_BLOCKS) return false;
    return (p->seen[block_no >> 3] >> (block_no & 7)) & 1u;
}

static void mark_block_seen(dfu_core_uf2_parser_t *p, uint32_t block_no)
{
    if (p->no_bitmap || block_no >= DFU_CORE_UF2_MAX_BLOCKS) return;
    p->seen[block_no >> 3] |= (uint8_t)(1u << (block_no & 7));
}

// Returns ESP_OK for "consumed" — including blocks deliberately skipped
// (non-UF2 sector, duplicate, out-of-range). Only a genuine flash /
// partition / finish failure returns an error; callers treat that as
// fatal. This skip-don't-abort policy is what lets the parser survive a
// host filesystem that delivers blocks out of order, duplicated, and
// interleaved with non-UF2 sectors (Linux FAT writeback over MSC).
static esp_err_t process_block(dfu_core_uf2_parser_t *p)
{
    const uint8_t *b = p->block;
    uint32_t m0 = read_u32_le(b + 0);
    uint32_t m1 = read_u32_le(b + 4);
    uint32_t m_end = read_u32_le(b + 508);
    if (m0 != DFU_CORE_UF2_MAGIC_START0 ||
        m1 != DFU_CORE_UF2_MAGIC_START1 ||
        m_end != DFU_CORE_UF2_MAGIC_END) {
        // The upstream sniff is only 4 bytes; a coincidental or torn
        // match reaches here. Not our block — skip, never fatal.
        ESP_LOGD(TAG, "skip non-UF2 sector (%08" PRIx32 " %08" PRIx32 "..%08" PRIx32 ")",
                 m0, m1, m_end);
        return ESP_OK;
    }

    uint32_t flags        = read_u32_le(b + 8);
    uint32_t target_addr  = read_u32_le(b + 12);
    uint32_t payload_size = read_u32_le(b + 16);
    uint32_t block_no     = read_u32_le(b + 20);
    uint32_t num_blocks   = read_u32_le(b + 24);
    uint32_t file_or_fam  = read_u32_le(b + 28);

    if (flags & UF2_FLAG_FILE_CONTAINER) {
        ESP_LOGD(TAG, "skip file-container block");
        return ESP_OK;
    }
    if (num_blocks == 0) {
        ESP_LOGD(TAG, "skip block with numBlocks=0");
        return ESP_OK;
    }

    if (flags & UF2_FLAG_FAMILY_ID_PRESENT) {
#if CONFIG_DFU_CORE_UF2_STRICT_FAMILY_ID
        if (file_or_fam != CONFIG_DFU_CORE_UF2_FAMILY_ID) {
            ESP_LOGE(TAG, "UF2 family 0x%" PRIx32 " rejected (expected 0x%x)",
                     file_or_fam, (unsigned)CONFIG_DFU_CORE_UF2_FAMILY_ID);
            return ESP_ERR_NOT_SUPPORTED;
        }
#endif
        p->family_id = file_or_fam;
    }
#if CONFIG_DFU_CORE_UF2_STRICT_FAMILY_ID
    else {
        ESP_LOGE(TAG, "UF2 has no family ID; strict mode rejects");
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif

    if (!p->started) {
        esp_err_t err = dfu_core_internal_uf2_start();
        if (err != ESP_OK) return err;
        p->started = true;
    }

    // Write the block, then decide what it was. A combined uf2.bin holds
    // several segments (bootloader, partition table, otadata, app), each
    // with its OWN numBlocks restarting block_no at 0. Only the app
    // segment lands inside an OTA partition; the rest are factory-flash
    // regions the writer reports as not-written (out_written=false). We
    // drive completion purely off app-segment blocks, so a stray combined
    // image can't "complete" early on its 47-block bootloader segment
    // (the bug that ran set_boot on an app-less partition).
    bool written = false;
    if (!(flags & UF2_FLAG_NOT_MAIN_FLASH)) {
        if (payload_size == 0 || payload_size > DFU_CORE_UF2_BLOCK_SIZE - 32 - 4) {
            ESP_LOGD(TAG, "skip block: bad payload size %" PRIu32, payload_size);
            return ESP_OK;
        }
        esp_err_t err = dfu_core_internal_uf2_write(target_addr, b + 32,
                                                    payload_size, &written);
        if (err != ESP_OK) return err;   // genuine flash failure: fatal
    }
    if (!written) {
        // Metadata or a non-app (factory-flash) segment block — ignore.
        return ESP_OK;
    }

    // App-segment block. Adopt its numBlocks as the completion target the
    // first time we see one; count distinct block_no via the bitmap.
    // Re-writing a duplicate to NOR flash is harmless (identical bytes),
    // so the bitmap only guards against double-COUNTING, which is what
    // caused premature completion under the host's re-flushes.
    if (p->total_blocks == 0) {
        p->total_blocks = num_blocks;
        p->no_bitmap = (num_blocks > DFU_CORE_UF2_MAX_BLOCKS);
        ESP_LOGI(TAG, "UF2 app image: %" PRIu32 " blocks, family 0x%" PRIx32,
                 num_blocks, p->family_id);
        if (p->no_bitmap) {
            ESP_LOGW(TAG, "numBlocks > %d; duplicate-tolerant completion "
                     "disabled (counter fallback)", DFU_CORE_UF2_MAX_BLOCKS);
        }
    }
    if (block_no < p->total_blocks && block_seen(p, block_no)) {
        return ESP_OK;   // already counted
    }
    mark_block_seen(p, block_no);
    p->seen_blocks++;
    if (p->seen_blocks >= p->total_blocks) {
        esp_err_t err = dfu_core_internal_uf2_finish();
        if (err != ESP_OK) return err;
        p->complete = true;
    }
    return ESP_OK;
}

esp_err_t dfu_core_uf2_feed(dfu_core_uf2_parser_t *p, const void *data, size_t len)
{
    if (!p || !data) return ESP_ERR_INVALID_ARG;
    if (p->complete) return ESP_ERR_INVALID_STATE;

    const uint8_t *src = data;
    while (len > 0) {
        size_t room = DFU_CORE_UF2_BLOCK_SIZE - p->in_block;
        size_t take = len < room ? len : room;
        memcpy(p->block + p->in_block, src, take);
        p->in_block += take;
        src += take;
        len -= take;

        if (p->in_block == DFU_CORE_UF2_BLOCK_SIZE) {
            esp_err_t err = process_block(p);
            p->in_block = 0;
            if (err != ESP_OK) {
                dfu_core_internal_uf2_abort();
                return err;
            }
            if (p->complete && len > 0) {
                ESP_LOGW(TAG, "ignoring %zu trailing bytes past last block", len);
                return ESP_OK;
            }
        }
    }
    return ESP_OK;
}
