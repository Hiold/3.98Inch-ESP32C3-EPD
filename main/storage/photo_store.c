#include "photo_store.h"

#include <stdlib.h>
#include <string.h>

#include "../ble/ble_protocol.h"

#ifdef ESP_PLATFORM
#include "esp_partition.h"
#include "esp_log.h"
#endif

#ifdef ESP_PLATFORM
/* Dynamic upload bitmaps are owned by this small registry rather than by the
 * four persistent slot objects. At boot this costs only eight pointers; an
 * active upload allocates its bitmap on demand. */
static photo_store_upload_t *s_bitmap_owners[PHOTO_STORE_SLOT_COUNT];
static uint8_t *s_upload_bitmaps[PHOTO_STORE_SLOT_COUNT];
#endif

static void upload_bitmap_release(photo_store_upload_t *upload)
{
#ifdef ESP_PLATFORM
    if (!upload) return;
    for (size_t i = 0; i < PHOTO_STORE_SLOT_COUNT; ++i) {
        if (s_bitmap_owners[i] == upload) {
            free(s_upload_bitmaps[i]);
            s_bitmap_owners[i] = NULL;
            s_upload_bitmaps[i] = NULL;
            upload->received = NULL;
            return;
        }
    }
#else
    (void)upload;
#endif
}

#ifdef ESP_PLATFORM
static uint8_t *upload_bitmap(photo_store_upload_t *upload)
{
    for (size_t i = 0; i < PHOTO_STORE_SLOT_COUNT; ++i) {
        if (s_bitmap_owners[i] == upload) return s_upload_bitmaps[i];
    }
    return NULL;
}

static bool upload_bitmap_take(photo_store_upload_t *upload, uint8_t *bitmap)
{
    for (size_t i = 0; i < PHOTO_STORE_SLOT_COUNT; ++i) {
        if (!s_bitmap_owners[i]) {
            s_bitmap_owners[i] = upload;
            s_upload_bitmaps[i] = bitmap;
            return true;
        }
    }
    return false;
}
#endif

void photo_store_upload_reset(photo_store_upload_t *upload)
{
    if (!upload) return;
    upload_bitmap_release(upload);
    memset(upload, 0, sizeof(*upload));
}

photo_store_status_t photo_store_upload_begin(photo_store_upload_t *upload,
                                              const photo_store_begin_t *begin)
{
    if (!upload || !begin) return PHOTO_STORE_ERR_ARG;
    if (begin->slot >= PHOTO_STORE_SLOT_COUNT || begin->total_len == 0 ||
        begin->total_len > PHOTO_STORE_MAX_SIZE) return PHOTO_STORE_ERR_RANGE;
    /* The ownership registry makes reset safe for both an uninitialised
     * stack object and a live upload that is being restarted. */
    photo_store_upload_reset(upload);
    upload->active = true;
    upload->photo_id = begin->photo_id;
    upload->slot = begin->slot;
    upload->total_len = begin->total_len;
    upload->expected_crc32 = begin->crc32;
    upload->chunk_count = (uint16_t)((begin->total_len + PHOTO_STORE_CHUNK_SIZE - 1u) /
                                     PHOTO_STORE_CHUNK_SIZE);
#ifdef ESP_PLATFORM
    const size_t bitmap_size = (begin->total_len + 7u) / 8u;
    uint8_t *bitmap = calloc(1, bitmap_size);
    if (!bitmap || !upload_bitmap_take(upload, bitmap)) {
        free(bitmap);
        memset(upload, 0, sizeof(*upload));
        return PHOTO_STORE_ERR_NO_SPACE;
    }
    upload->received = bitmap;
#endif
    return PHOTO_STORE_OK;
}

photo_store_status_t photo_store_upload_mark_data(photo_store_upload_t *upload,
                                                  uint32_t offset, size_t len)
{
    if (!upload || !upload->active) return PHOTO_STORE_ERR_STATE;
    if (len == 0 || offset >= upload->total_len || len > upload->total_len - offset) {
        return PHOTO_STORE_ERR_RANGE;
    }
#ifdef ESP_PLATFORM
    uint8_t *received = upload_bitmap(upload);
    if (!received) return PHOTO_STORE_ERR_STATE;
#else
    uint8_t *received = upload->received;
#endif
    for (uint32_t byte = offset; byte < offset + len; ++byte) {
        const uint32_t bitmap_byte = byte >> 3;
        const uint8_t bitmap_bit = (uint8_t)(1u << (byte & 7u));
        if ((received[bitmap_byte] & bitmap_bit) == 0) {
            received[bitmap_byte] |= bitmap_bit;
            upload->received_bytes++;
        }
    }
    return PHOTO_STORE_OK;
}

bool photo_store_upload_complete(const photo_store_upload_t *upload)
{
    return upload && upload->active && upload->received_bytes == upload->total_len;
}

uint32_t photo_store_upload_next_offset(const photo_store_upload_t *upload)
{
    if (!upload || !upload->active) return 0;
#ifdef ESP_PLATFORM
    const uint8_t *received = upload_bitmap((photo_store_upload_t *)upload);
    if (!received) return 0;
#else
    const uint8_t *received = upload->received;
#endif
    for (uint32_t byte = 0; byte < upload->total_len; ++byte) {
        if ((received[byte >> 3] & (uint8_t)(1u << (byte & 7u))) == 0) return byte;
    }
    return upload->total_len;
}

photo_store_status_t photo_store_upload_finish(photo_store_upload_t *upload,
                                               uint32_t actual_crc32,
                                               photo_store_info_t *info)
{
    if (!upload || !upload->active || !info) return PHOTO_STORE_ERR_ARG;
    if (!photo_store_upload_complete(upload)) return PHOTO_STORE_ERR_STATE;
    if (actual_crc32 != upload->expected_crc32) return PHOTO_STORE_ERR_CRC;
    memset(info, 0, sizeof(*info));
    info->valid = true;
    info->photo_id = upload->photo_id;
    info->slot = upload->slot;
    info->length = upload->total_len;
    info->crc32 = actual_crc32;
    upload_bitmap_release(upload);
    upload->active = false;
    return PHOTO_STORE_OK;
}

#ifdef ESP_PLATFORM
static const esp_partition_t *photo_partition;
static photo_store_info_t committed[PHOTO_STORE_SLOT_COUNT];
static photo_store_upload_t uploads[PHOTO_STORE_SLOT_COUNT];
static uint8_t committed_bank[PHOTO_STORE_SLOT_COUNT];
static uint32_t committed_generation[PHOTO_STORE_SLOT_COUNT];
/* CRC verification intentionally uses one BLE-sized block.  It costs a few
 * more flash reads but avoids reserving a 4 KiB RAM buffer for rare commits. */
static uint8_t crc_read_buf[PHOTO_STORE_CHUNK_SIZE];

#define PHOTO_STORE_META_MAGIC 0x50534d31u
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t photo_id;
    uint32_t length;
    uint32_t crc32;
    uint32_t generation;
    uint32_t header_crc32;
} photo_store_flash_meta_t;

static size_t bank_offset(uint8_t slot, uint8_t bank);

esp_err_t photo_store_init(void)
{
    photo_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "photos");
    if (!photo_partition || photo_partition->size < PHOTO_STORE_PARTITION_SIZE) {
        return ESP_ERR_NOT_FOUND;
    }
    memset(committed, 0, sizeof(committed));
    memset(uploads, 0, sizeof(uploads));
    memset(committed_bank, 0, sizeof(committed_bank));
    memset(committed_generation, 0, sizeof(committed_generation));
    for (uint8_t slot = 0; slot < PHOTO_STORE_SLOT_COUNT; ++slot) {
        for (uint8_t bank = 0; bank < PHOTO_STORE_BANK_COUNT; ++bank) {
            photo_store_flash_meta_t meta;
            if (esp_partition_read(photo_partition, bank_offset(slot, bank) + PHOTO_STORE_MAX_SIZE,
                                   &meta, sizeof(meta)) != ESP_OK ||
                meta.magic != PHOTO_STORE_META_MAGIC || meta.length == 0 ||
                meta.length > PHOTO_STORE_MAX_SIZE ||
                ble_proto_crc32((const uint8_t *)&meta, sizeof(meta) - sizeof(meta.header_crc32)) !=
                    meta.header_crc32) {
                continue;
            }
            if (!committed[slot].valid || meta.generation >= committed_generation[slot]) {
                committed_bank[slot] = bank;
                committed_generation[slot] = meta.generation;
                committed[slot].valid = true;
                committed[slot].photo_id = meta.photo_id;
                committed[slot].slot = slot;
                committed[slot].length = meta.length;
                committed[slot].crc32 = meta.crc32;
            }
        }
    }
    return ESP_OK;
}

static size_t bank_offset(uint8_t slot, uint8_t bank)
{
    return ((size_t)slot * PHOTO_STORE_BANK_COUNT + bank) * PHOTO_STORE_SLOT_STRIDE;
}

esp_err_t photo_store_get(uint8_t slot, photo_store_info_t *info,
                          void *buffer, size_t capacity, size_t *out_len)
{
    if (!photo_partition || slot >= PHOTO_STORE_SLOT_COUNT || !info) return ESP_ERR_INVALID_ARG;
    *info = committed[slot];
    if (out_len) *out_len = info->valid ? info->length : 0;
    if (!info->valid) return ESP_ERR_NOT_FOUND;
    if (!buffer || capacity < info->length) return ESP_ERR_INVALID_SIZE;
    return esp_partition_read(photo_partition, bank_offset(slot, committed_bank[slot]), buffer, info->length);
}

esp_err_t photo_store_read(uint8_t slot, size_t offset, void *buffer, size_t len)
{
    if (!photo_partition || slot >= PHOTO_STORE_SLOT_COUNT || !buffer || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const photo_store_info_t *info = &committed[slot];
    if (!info->valid || offset > info->length || len > info->length - offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    return esp_partition_read(photo_partition,
                              bank_offset(slot, committed_bank[slot]) + offset,
                              buffer, len);
}

esp_err_t photo_store_begin(const photo_store_begin_t *begin)
{
    if (!photo_partition || !begin || begin->slot >= PHOTO_STORE_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    const photo_store_status_t status = photo_store_upload_begin(&uploads[begin->slot], begin);
    if (status != PHOTO_STORE_OK) return ESP_ERR_INVALID_ARG;
    const uint8_t temp_bank = (uint8_t)(committed_bank[begin->slot] ^ 1u);
    const esp_err_t erase_err = esp_partition_erase_range(
        photo_partition, bank_offset(begin->slot, temp_bank),
        PHOTO_STORE_SLOT_STRIDE);
    if (erase_err != ESP_OK) photo_store_upload_reset(&uploads[begin->slot]);
    return erase_err;
}

esp_err_t photo_store_write(uint8_t slot, uint32_t offset, const void *data, size_t len)
{
    if (!photo_partition || slot >= PHOTO_STORE_SLOT_COUNT || !data) return ESP_ERR_INVALID_ARG;
    if (!uploads[slot].active || len == 0 || offset >= uploads[slot].total_len ||
        len > uploads[slot].total_len - offset) return ESP_ERR_INVALID_SIZE;
    const uint8_t temp_bank = (uint8_t)(committed_bank[slot] ^ 1u);
    esp_err_t err = esp_partition_write(photo_partition, bank_offset(slot, temp_bank) + offset, data, len);
    if (err == ESP_OK && photo_store_upload_mark_data(&uploads[slot], offset, len) != PHOTO_STORE_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

esp_err_t photo_store_end(const photo_store_begin_t *begin, uint32_t actual_crc32)
{
    if (!begin || begin->slot >= PHOTO_STORE_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    photo_store_info_t info;
    const photo_store_status_t status = photo_store_upload_finish(&uploads[begin->slot], actual_crc32, &info);
    if (status != PHOTO_STORE_OK) return status == PHOTO_STORE_ERR_CRC ? ESP_ERR_INVALID_CRC : ESP_ERR_INVALID_STATE;
    /* The inactive bank is committed only after a complete read-back CRC. */
    const uint8_t temp_bank = (uint8_t)(committed_bank[begin->slot] ^ 1u);
    uint32_t crc = 0xffffffffu;
    uint32_t remaining = info.length;
    size_t offset = 0;
    while (remaining) {
        const size_t take = remaining < sizeof(crc_read_buf) ? remaining : sizeof(crc_read_buf);
        esp_err_t err = esp_partition_read(photo_partition, bank_offset(begin->slot, temp_bank) + offset,
                                            crc_read_buf, take);
        if (err != ESP_OK) return err;
        for (size_t i = 0; i < take; ++i) {
            crc ^= crc_read_buf[i];
            for (unsigned bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
            }
        }
        offset += take;
        remaining -= (uint32_t)take;
    }
    if ((~crc) != info.crc32) return ESP_ERR_INVALID_CRC;
    photo_store_flash_meta_t meta = {
        .magic = PHOTO_STORE_META_MAGIC,
        .photo_id = info.photo_id,
        .length = info.length,
        .crc32 = info.crc32,
        .generation = committed_generation[begin->slot] + 1u,
    };
    meta.header_crc32 = ble_proto_crc32((const uint8_t *)&meta,
                                        sizeof(meta) - sizeof(meta.header_crc32));
    esp_err_t meta_err = esp_partition_write(photo_partition,
                                             bank_offset(begin->slot, temp_bank) + PHOTO_STORE_MAX_SIZE,
                                             &meta, sizeof(meta));
    if (meta_err != ESP_OK) return meta_err;
    committed_generation[begin->slot] = meta.generation;
    committed_bank[begin->slot] = temp_bank;
    committed[begin->slot] = info;
    return ESP_OK;
}

esp_err_t photo_store_abort(uint8_t slot)
{
    if (slot >= PHOTO_STORE_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    photo_store_upload_reset(&uploads[slot]);
    return ESP_OK;
}
#else
esp_err_t photo_store_init(void) { return ESP_OK; }
esp_err_t photo_store_get(uint8_t slot, photo_store_info_t *info, void *buffer, size_t capacity, size_t *out_len)
{ (void)slot; (void)info; (void)buffer; (void)capacity; if (out_len) *out_len = 0; return -1; }
esp_err_t photo_store_read(uint8_t slot, size_t offset, void *buffer, size_t len)
{ (void)slot; (void)offset; (void)buffer; (void)len; return -1; }
esp_err_t photo_store_begin(const photo_store_begin_t *begin) { (void)begin; return -1; }
esp_err_t photo_store_write(uint8_t slot, uint32_t offset, const void *data, size_t len)
{ (void)slot; (void)offset; (void)data; (void)len; return -1; }
esp_err_t photo_store_end(const photo_store_begin_t *begin, uint32_t actual_crc32)
{ (void)begin; (void)actual_crc32; return -1; }
esp_err_t photo_store_abort(uint8_t slot) { (void)slot; return ESP_OK; }
#endif
