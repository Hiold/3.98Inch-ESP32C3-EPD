#ifndef BLE_UPLOAD_H
#define BLE_UPLOAD_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "../storage/photo_store.h"

typedef struct {
    photo_store_upload_t state;
    uint32_t crc32;
    bool active;
} ble_upload_t;

int ble_upload_begin(ble_upload_t *upload, const photo_store_begin_t *begin);
int ble_upload_data(ble_upload_t *upload, uint32_t offset,
                    const void *data, size_t len);
int ble_upload_end(ble_upload_t *upload, uint32_t crc32,
                   photo_store_info_t *info);
void ble_upload_abort(ble_upload_t *upload);
uint32_t ble_upload_next_offset(const ble_upload_t *upload);

#endif
