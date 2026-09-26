#include "ble_gatt.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_bt.h"
#endif

static ble_gatt_rx_cb_t rx_callback;
static void *rx_context;

int ble_gatt_init(ble_gatt_t *gatt, ble_gatt_rx_cb_t callback, void *ctx)
{
    if (!gatt) return -1;
    memset(gatt, 0, sizeof(*gatt));
    gatt->initialized = true;
    gatt->applied_power_dbm = 6;
    rx_callback = callback;
    rx_context = ctx;
    return ble_gatt_apply_tx_limit(gatt);
}

int ble_gatt_set_connected(ble_gatt_t *gatt, bool connected)
{
    if (!gatt || !gatt->initialized) return -1;
    gatt->connected = connected;
    return 0;
}

int ble_gatt_apply_tx_limit(ble_gatt_t *gatt)
{
    if (!gatt) return -1;
#ifdef ESP_PLATFORM
    /* ESP32-C3 has no exact 7 dBm level; P6 is the closest lower level. */
    const esp_power_level_t level = ESP_PWR_LVL_P6;
    /* The default level is the connection fallback. Do not try all nine
     * connection slots before a link exists: IDF documents those as
     * post-connect settings and the target supports a single NimBLE peer. */
    esp_err_t err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, level);
    if (err != ESP_OK) return (int)err;
    err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, level);
    if (err != ESP_OK) return (int)err;
    err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, level);
    if (err != ESP_OK) return (int)err;
#endif
    gatt->applied_power_dbm = 6;
    return 0;
}

int ble_gatt_apply_connection_tx_limit(ble_gatt_t *gatt,
                                      uint16_t connection_handle)
{
    if (!gatt || !gatt->initialized) return -1;
#ifdef ESP_PLATFORM
    /* ESP-IDF 6.0.2 maps the legacy per-connection selector to handles 0..8.
     * The controller default is already capped; this tightens the active
     * connection where the selector can represent its handle. */
    if (connection_handle > 8u) return 0;
    const esp_err_t err = esp_ble_tx_power_set(
        (esp_ble_power_type_t)(ESP_BLE_PWR_TYPE_CONN_HDL0 + connection_handle),
        ESP_PWR_LVL_P6);
    if (err != ESP_OK) return (int)err;
#else
    (void)connection_handle;
#endif
    return 0;
}

bool ble_gatt_requires_encryption(void)
{
    return false;
}

int8_t ble_gatt_applied_power_dbm(const ble_gatt_t *gatt)
{
    return gatt ? gatt->applied_power_dbm : 0;
}

/* Kept as a small dispatch hook so the eventual NimBLE characteristic
 * callbacks can feed the same stream parser without sharing frame buffers. */
void ble_gatt_dispatch_frame(const ble_frame_view_t *frame)
{
    if (rx_callback) rx_callback(frame, rx_context);
}
