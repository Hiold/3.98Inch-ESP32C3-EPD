#ifndef BLE_COMMANDS_H
#define BLE_COMMANDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_protocol.h"
#include "ble_upload.h"
#include "../storage/nvs_store.h"

#define BLE_COMMAND_RESPONSE_DATA_MAX 192u
/* A response has one status byte plus the largest command reply, framing
 * header and CRC16.  Keeping this bound explicit avoids allocating a full
 * 1 KiB request frame for every notification response. */
#define BLE_COMMAND_ENCODED_RESPONSE_MAX \
    (BLE_PROTO_HEADER_SIZE + BLE_COMMAND_RESPONSE_DATA_MAX + 1u + \
     BLE_PROTO_CRC_SIZE)

typedef struct {
    bool wifi_connected;
    bool ntp_synced;
    bool weather_valid;
    bool weather_stale;
    bool wifi_credentials_present;
    int8_t wifi_tx_power_qdbm;
    int8_t ble_tx_power_dbm;
    int16_t weather_temperature_c10;
    uint64_t weather_updated_unix;
} ble_runtime_info_t;

typedef struct {
    int (*get_runtime_info)(void *ctx, ble_runtime_info_t *info);
    int (*request_refresh)(void *ctx, uint32_t reason, bool force);
    /* Applies the already-persisted credentials to the running STA and
     * requests a reconnect.  The callback is intentionally separate from
     * persistence so a host implementation can test both stages. */
    int (*apply_wifi_credentials)(void *ctx);
    int (*request_weather_refresh)(void *ctx);
} ble_commands_ops_t;

typedef enum {
    BLE_COMMAND_OK = 0,
    BLE_COMMAND_INVALID = 1,
    BLE_COMMAND_STORAGE_ERROR = 2,
    BLE_COMMAND_STATE_ERROR = 3,
    BLE_COMMAND_NOT_SUPPORTED = 4,
    BLE_COMMAND_BUSY = 5,
} ble_command_result_t;

typedef struct {
    uint8_t data[BLE_COMMAND_RESPONSE_DATA_MAX];
    uint16_t length;
} ble_command_reply_t;

#define BLE_COMMAND_REPLAY_CACHE_SIZE 4u
typedef struct {
    bool valid;
    uint8_t type;
    uint8_t status;
    uint16_t sequence;
    uint16_t payload_len;
    uint32_t payload_crc32;
    ble_command_reply_t reply;
} ble_command_cache_entry_t;

typedef struct {
    nvs_store_config_t *config;
    esp_err_t (*persist)(const nvs_store_config_t *config);
    bool (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void (*config_changed)(void *ctx);
    void *ctx;
    ble_commands_ops_t ops;
    ble_upload_t upload;
    ble_command_cache_entry_t replay_cache[BLE_COMMAND_REPLAY_CACHE_SIZE];
    uint8_t replay_next;
} ble_commands_t;

void ble_commands_init(ble_commands_t *commands, nvs_store_config_t *config,
                       esp_err_t (*persist)(const nvs_store_config_t *config),
                       bool (*lock)(void *ctx), void (*unlock)(void *ctx),
                       void (*config_changed)(void *ctx), void *ctx);
void ble_commands_set_ops(ble_commands_t *commands,
                          const ble_commands_ops_t *ops);
ble_command_result_t ble_commands_process_ex(ble_commands_t *commands,
                                              const ble_frame_view_t *frame,
                                              ble_command_reply_t *reply);
ble_command_result_t ble_commands_process(ble_commands_t *commands,
                                          const ble_frame_view_t *frame);
/* Callback-compatible adapter for simple integrations without acknowledgements. */
void ble_commands_on_frame(const ble_frame_view_t *frame, void *ctx);

#endif
