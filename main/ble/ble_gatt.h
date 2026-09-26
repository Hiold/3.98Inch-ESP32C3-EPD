#ifndef BLE_GATT_H
#define BLE_GATT_H

#include <stdbool.h>
#include <stdint.h>

#include "ble_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_GATT_SERVICE_UUID "7b4d0001-6a6c-4c61-9f20-65706463616c"
#define BLE_GATT_CONTROL_UUID "7b4d0002-6a6c-4c61-9f20-65706463616c"
#define BLE_GATT_NOTIFY_UUID "7b4d0003-6a6c-4c61-9f20-65706463616c"
#define BLE_GATT_IMAGE_UUID "7b4d0004-6a6c-4c61-9f20-65706463616c"

typedef struct {
    bool initialized;
    bool connected;
    int8_t applied_power_dbm;
} ble_gatt_t;

typedef void (*ble_gatt_rx_cb_t)(const ble_frame_view_t *frame, void *ctx);

int ble_gatt_init(ble_gatt_t *gatt, ble_gatt_rx_cb_t callback, void *ctx);
int ble_gatt_set_connected(ble_gatt_t *gatt, bool connected);
int ble_gatt_apply_tx_limit(ble_gatt_t *gatt);
int ble_gatt_apply_connection_tx_limit(ble_gatt_t *gatt,
                                      uint16_t connection_handle);
bool ble_gatt_requires_encryption(void);
int8_t ble_gatt_applied_power_dbm(const ble_gatt_t *gatt);
void ble_gatt_dispatch_frame(const ble_frame_view_t *frame);

#ifdef __cplusplus
}
#endif

#endif
