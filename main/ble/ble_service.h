#ifndef BLE_SERVICE_H
#define BLE_SERVICE_H

#include "ble_gatt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the open NimBLE GATT control/image service and advertising. */
int ble_service_start(ble_gatt_rx_cb_t callback, void *ctx);
/* Sends one already encoded response frame to connected subscribed clients. */
int ble_service_notify(const uint8_t *frame, size_t frame_len);

#ifdef __cplusplus
}
#endif

#endif
