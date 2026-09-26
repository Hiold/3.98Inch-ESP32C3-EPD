#ifndef PHOTO_STORE_H
#define PHOTO_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PHOTO_STORE_SLOT_COUNT 4u
#define PHOTO_STORE_WIDTH 768u
#define PHOTO_STORE_HEIGHT 552u
#define PHOTO_STORE_BYTES ((PHOTO_STORE_WIDTH * PHOTO_STORE_HEIGHT * 2u) / 8u)
#define PHOTO_STORE_CHUNK_SIZE 256u
#define PHOTO_STORE_MAX_SIZE PHOTO_STORE_BYTES
#define PHOTO_STORE_SLOT_STRIDE (((PHOTO_STORE_MAX_SIZE + 4095u) / 4096u) * 4096u)
#define PHOTO_STORE_BANK_COUNT 2u
#define PHOTO_STORE_PARTITION_SIZE (PHOTO_STORE_SLOT_COUNT * PHOTO_STORE_BANK_COUNT * PHOTO_STORE_SLOT_STRIDE)
#define PHOTO_STORE_MAX_CHUNKS ((PHOTO_STORE_MAX_SIZE + PHOTO_STORE_CHUNK_SIZE - 1u) / PHOTO_STORE_CHUNK_SIZE)

typedef enum {
    PHOTO_STORE_OK = 0,
    PHOTO_STORE_ERR_ARG = -1,
    PHOTO_STORE_ERR_STATE = -2,
    PHOTO_STORE_ERR_RANGE = -3,
    PHOTO_STORE_ERR_CRC = -4,
    PHOTO_STORE_ERR_IO = -5,
    PHOTO_STORE_ERR_NO_SPACE = -6,
} photo_store_status_t;

typedef struct {
    uint32_t photo_id;
    uint8_t slot;
    uint32_t total_len;
    uint32_t crc32;
} photo_store_begin_t;

typedef struct {
    bool active;
    uint32_t photo_id;
    uint8_t slot;
    uint32_t total_len;
    uint32_t expected_crc32;
    uint32_t received_bytes;
    uint16_t chunk_count;
#ifdef ESP_PLATFORM
    /* The bitmap is allocated only for the lifetime of an active upload.
     * Keeping it out of the persistent per-slot state saves about 53 KiB of
     * boot-time RAM while preserving byte-granular resume/duplicate checks. */
    uint8_t *received;
#else
    /* Byte-level bitmap makes arbitrary MTU-sized, out-of-order fragments safe. */
    uint8_t received[(PHOTO_STORE_MAX_SIZE + 7u) / 8u];
#endif
} photo_store_upload_t;

typedef struct {
    bool valid;
    uint32_t photo_id;
    uint8_t slot;
    uint32_t length;
    uint32_t crc32;
} photo_store_info_t;

void photo_store_upload_reset(photo_store_upload_t *upload);
photo_store_status_t photo_store_upload_begin(photo_store_upload_t *upload,
                                              const photo_store_begin_t *begin);
photo_store_status_t photo_store_upload_mark_data(photo_store_upload_t *upload,
                                                  uint32_t offset, size_t len);
bool photo_store_upload_complete(const photo_store_upload_t *upload);
uint32_t photo_store_upload_next_offset(const photo_store_upload_t *upload);
photo_store_status_t photo_store_upload_finish(photo_store_upload_t *upload,
                                               uint32_t actual_crc32,
                                               photo_store_info_t *info);

#ifdef ESP_PLATFORM
#include "esp_err.h"
esp_err_t photo_store_init(void);
esp_err_t photo_store_get(uint8_t slot, photo_store_info_t *info,
                          void *buffer, size_t capacity, size_t *out_len);
/* Read a bounded slice without allocating a complete 106 KiB image buffer. */
esp_err_t photo_store_read(uint8_t slot, size_t offset, void *buffer,
                           size_t len);
esp_err_t photo_store_begin(const photo_store_begin_t *begin);
esp_err_t photo_store_write(uint8_t slot, uint32_t offset,
                            const void *data, size_t len);
esp_err_t photo_store_end(const photo_store_begin_t *begin, uint32_t actual_crc32);
esp_err_t photo_store_abort(uint8_t slot);
#else
typedef int esp_err_t;
#define ESP_OK 0
esp_err_t photo_store_init(void);
esp_err_t photo_store_get(uint8_t slot, photo_store_info_t *info,
                          void *buffer, size_t capacity, size_t *out_len);
esp_err_t photo_store_read(uint8_t slot, size_t offset, void *buffer,
                           size_t len);
esp_err_t photo_store_begin(const photo_store_begin_t *begin);
esp_err_t photo_store_write(uint8_t slot, uint32_t offset,
                            const void *data, size_t len);
esp_err_t photo_store_end(const photo_store_begin_t *begin, uint32_t actual_crc32);
esp_err_t photo_store_abort(uint8_t slot);
#endif

#ifdef __cplusplus
}
#endif

#endif
