#include "ble_commands.h"

#include <string.h>

#include "../app/refresh_policy.h"

typedef void (*config_mutator_t)(nvs_store_config_t *config,
                                 const void *argument);

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void append_u16(ble_command_reply_t *reply, uint16_t value)
{
    reply->data[reply->length++] = (uint8_t)value;
    reply->data[reply->length++] = (uint8_t)(value >> 8);
}

static void append_u32(ble_command_reply_t *reply, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) {
        reply->data[reply->length++] = (uint8_t)(value >> (i * 8));
    }
}

static void append_u64(ble_command_reply_t *reply, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) {
        reply->data[reply->length++] = (uint8_t)(value >> (i * 8));
    }
}

static bool payload_is_empty(const ble_frame_view_t *frame)
{
    return frame->payload_len == 0;
}

static bool timezone_is_valid(const uint8_t *value, size_t length)
{
    if (!value || length == 0 || length > NVS_STORE_TIMEZONE_MAX ||
        value[0] == '/' || value[length - 1] == '/') {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const uint8_t c = value[i];
        const bool valid = (c >= 'a' && c <= 'z') ||
                           (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '+' ||
                           c == '-' || c == '.' || c == '/';
        if (!valid || (c == '/' && i > 0 && value[i - 1] == '/')) return false;
    }
    return true;
}

static ble_command_result_t update_config(ble_commands_t *commands,
                                          config_mutator_t mutate,
                                          const void *argument)
{
    if (!commands->persist || !mutate) return BLE_COMMAND_NOT_SUPPORTED;
    if (commands->lock && !commands->lock(commands->ctx)) {
        return BLE_COMMAND_STATE_ERROR;
    }
    nvs_store_config_t updated = *commands->config;
    mutate(&updated, argument);
    if (!nvs_store_config_is_valid(&updated)) {
        if (commands->unlock) commands->unlock(commands->ctx);
        return BLE_COMMAND_INVALID;
    }
    if (commands->persist(&updated) != ESP_OK) {
        if (commands->unlock) commands->unlock(commands->ctx);
        return BLE_COMMAND_STORAGE_ERROR;
    }
    *commands->config = updated;
    if (commands->unlock) commands->unlock(commands->ctx);
    if (commands->config_changed) commands->config_changed(commands->ctx);
    return BLE_COMMAND_OK;
}

static void set_rotation(nvs_store_config_t *config, const void *argument)
{
    config->rotation = *(const uint8_t *)argument;
}

static void set_mode(nvs_store_config_t *config, const void *argument)
{
    config->home_mode = *(const uint8_t *)argument;
}

static void set_photo_slot(nvs_store_config_t *config, const void *argument)
{
    config->current_photo_slot = *(const uint8_t *)argument;
}

static void set_timezone(nvs_store_config_t *config, const void *argument)
{
    const char *timezone = argument;
    memset(config->timezone, 0, sizeof(config->timezone));
    memcpy(config->timezone, timezone, strlen(timezone));
}

typedef struct {
    int32_t latitude_e7;
    int32_t longitude_e7;
} location_t;

typedef struct {
    char ssid[NVS_STORE_SSID_MAX + 1];
    char password[NVS_STORE_PASSWORD_MAX + 1];
} wifi_credentials_t;

static void set_location(nvs_store_config_t *config, const void *argument)
{
    const location_t *location = argument;
    config->latitude_e7 = location->latitude_e7;
    config->longitude_e7 = location->longitude_e7;
}

static void set_wifi_credentials(nvs_store_config_t *config,
                                 const void *argument)
{
    const wifi_credentials_t *credentials = argument;
    memset(config->wifi_ssid, 0, sizeof(config->wifi_ssid));
    memset(config->wifi_password, 0, sizeof(config->wifi_password));
    memcpy(config->wifi_ssid, credentials->ssid, strlen(credentials->ssid));
    memcpy(config->wifi_password, credentials->password,
           strlen(credentials->password));
}

static bool wifi_credential_bytes_valid(const uint8_t *value, size_t length,
                                        size_t max_length, bool allow_empty)
{
    if (!value || length > max_length || (!allow_empty && length == 0)) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        /* The IDF Wi-Fi config is a NUL-terminated C buffer.  Reject embedded
         * NULs and control bytes so a BLE frame cannot truncate what is saved. */
        if (value[i] == 0 || value[i] < 0x20u || value[i] == 0x7fu) {
            return false;
        }
    }
    return true;
}

static void set_today_plan(nvs_store_config_t *config, const void *argument)
{
    const char *plan = argument;
    memset(config->today_plan, 0, sizeof(config->today_plan));
    memcpy(config->today_plan, plan, strlen(plan));
}

void ble_commands_init(ble_commands_t *commands, nvs_store_config_t *config,
                       esp_err_t (*persist)(const nvs_store_config_t *config),
                       bool (*lock)(void *ctx), void (*unlock)(void *ctx),
                       void (*config_changed)(void *ctx), void *ctx)
{
    if (!commands) return;
    memset(commands, 0, sizeof(*commands));
    commands->config = config;
    commands->persist = persist;
    commands->lock = lock;
    commands->unlock = unlock;
    commands->config_changed = config_changed;
    commands->ctx = ctx;
}

void ble_commands_set_ops(ble_commands_t *commands,
                          const ble_commands_ops_t *ops)
{
    if (!commands) return;
    if (ops) commands->ops = *ops;
    else memset(&commands->ops, 0, sizeof(commands->ops));
}

static ble_command_result_t build_device_info(ble_commands_t *commands,
                                              ble_command_reply_t *reply)
{
    if (!commands->ops.get_runtime_info) return BLE_COMMAND_NOT_SUPPORTED;
    nvs_store_config_t config;
    if (commands->lock && !commands->lock(commands->ctx)) {
        return BLE_COMMAND_STATE_ERROR;
    }
    config = *commands->config;
    if (commands->unlock) commands->unlock(commands->ctx);

    ble_runtime_info_t runtime = {0};
    if (commands->ops.get_runtime_info(commands->ctx, &runtime) != 0) {
        return BLE_COMMAND_STATE_ERROR;
    }

    /* GET_INFO v1 fixed fields followed by length-prefixed UTF-8 strings and
     * the current in-RAM upload resume state. See docs/ble_protocol.md. */
    reply->data[reply->length++] = 1u; /* info schema */
    reply->data[reply->length++] = config.home_mode;
    reply->data[reply->length++] = config.rotation;
    reply->data[reply->length++] = config.current_photo_slot;
    uint8_t flags = 0;
    if (runtime.wifi_connected) flags |= 1u << 0;
    if (runtime.ntp_synced) flags |= 1u << 1;
    if (runtime.weather_valid) flags |= 1u << 2;
    if (runtime.weather_stale) flags |= 1u << 3;
    if (runtime.wifi_credentials_present) flags |= 1u << 4;
    reply->data[reply->length++] = flags;
    reply->data[reply->length++] = (uint8_t)runtime.wifi_tx_power_qdbm;
    reply->data[reply->length++] = (uint8_t)runtime.ble_tx_power_dbm;
    append_u32(reply, config.auto_refresh_interval_sec);
    append_u32(reply, (uint32_t)config.latitude_e7);
    append_u32(reply, (uint32_t)config.longitude_e7);
    append_u16(reply, (uint16_t)runtime.weather_temperature_c10);
    append_u64(reply, runtime.weather_updated_unix);

    const size_t timezone_len = strnlen(config.timezone, sizeof(config.timezone));
    reply->data[reply->length++] = (uint8_t)timezone_len;
    memcpy(reply->data + reply->length, config.timezone, timezone_len);
    reply->length = (uint16_t)(reply->length + timezone_len);
    const size_t plan_len = strnlen(config.today_plan, sizeof(config.today_plan));
    reply->data[reply->length++] = (uint8_t)plan_len;
    memcpy(reply->data + reply->length, config.today_plan, plan_len);
    reply->length = (uint16_t)(reply->length + plan_len);

    reply->data[reply->length++] = commands->upload.active ? 1u : 0u;
    reply->data[reply->length++] = commands->upload.active
                                       ? commands->upload.state.slot : 0xffu;
    append_u32(reply, commands->upload.active
                         ? commands->upload.state.photo_id : 0u);
    append_u32(reply, commands->upload.active
                         ? commands->upload.state.total_len : 0u);
    append_u32(reply, ble_upload_next_offset(&commands->upload));
    return BLE_COMMAND_OK;
}

static ble_command_result_t photo_status_to_command(int status)
{
    switch (status) {
    case PHOTO_STORE_OK: return BLE_COMMAND_OK;
    case PHOTO_STORE_ERR_NO_SPACE: return BLE_COMMAND_BUSY;
    case PHOTO_STORE_ERR_IO: return BLE_COMMAND_STORAGE_ERROR;
    case PHOTO_STORE_ERR_STATE: return BLE_COMMAND_STATE_ERROR;
    case PHOTO_STORE_ERR_CRC: return BLE_COMMAND_INVALID;
    default: return BLE_COMMAND_INVALID;
    }
}

static ble_command_result_t process_uncached(ble_commands_t *commands,
                                              const ble_frame_view_t *frame,
                                              ble_command_reply_t *reply)
{
    /* The normal task path supplies a reply object.  Keep the optional
     * discard sink in static storage so the NULL-compatible API does not
     * consume another ~200 bytes of the command task stack. */
    static ble_command_reply_t discarded;
    if (!reply) reply = &discarded;
    reply->length = 0;
    if (!commands || !commands->config || !frame ||
        frame->version != BLE_PROTO_VERSION ||
        (frame->payload_len != 0 && !frame->payload) ||
        frame->payload_len > BLE_PROTO_MAX_PAYLOAD) {
        return BLE_COMMAND_INVALID;
    }

    switch (frame->type) {
    case BLE_CMD_GET_INFO:
        if (!payload_is_empty(frame)) return BLE_COMMAND_INVALID;
        return build_device_info(commands, reply);

    case BLE_CMD_SET_ROTATION:
        if (frame->payload_len != 1 || frame->payload[0] > 3u) {
            return BLE_COMMAND_INVALID;
        }
        return update_config(commands, set_rotation, frame->payload);

    case BLE_CMD_SET_MODE:
        if (frame->payload_len != 1 ||
            frame->payload[0] > NVS_STORE_MODE_STATUS) {
            return BLE_COMMAND_INVALID;
        }
        return update_config(commands, set_mode, frame->payload);

    case BLE_CMD_REFRESH:
        if (!payload_is_empty(frame)) return BLE_COMMAND_INVALID;
        if (!commands->ops.request_refresh) return BLE_COMMAND_NOT_SUPPORTED;
        return commands->ops.request_refresh(commands->ctx, REFRESH_REASON_BLE,
                                             true) == 0
                   ? BLE_COMMAND_OK : BLE_COMMAND_BUSY;

    case BLE_CMD_NEXT_PHOTO: {
        if (!payload_is_empty(frame)) return BLE_COMMAND_INVALID;
        uint8_t next;
        if (commands->lock && !commands->lock(commands->ctx)) {
            return BLE_COMMAND_STATE_ERROR;
        }
        next = (uint8_t)((commands->config->current_photo_slot + 1u) %
                         PHOTO_STORE_SLOT_COUNT);
        if (commands->unlock) commands->unlock(commands->ctx);
        const ble_command_result_t status = update_config(commands,
                                                          set_photo_slot,
                                                          &next);
        if (status == BLE_COMMAND_OK) reply->data[reply->length++] = next;
        return status;
    }

    case BLE_CMD_SET_TIMEZONE: {
        if (!timezone_is_valid(frame->payload, frame->payload_len)) {
            return BLE_COMMAND_INVALID;
        }
        char timezone[NVS_STORE_TIMEZONE_MAX + 1] = {0};
        memcpy(timezone, frame->payload, frame->payload_len);
        return update_config(commands, set_timezone, timezone);
    }

    case BLE_CMD_SET_LOCATION: {
        if (frame->payload_len != 8) return BLE_COMMAND_INVALID;
        location_t location = {
            .latitude_e7 = (int32_t)read_le32(frame->payload),
            .longitude_e7 = (int32_t)read_le32(frame->payload + 4),
        };
        const ble_command_result_t status = update_config(commands,
                                                          set_location,
                                                          &location);
        if (status != BLE_COMMAND_OK) return status;
        if (commands->ops.request_weather_refresh &&
            commands->ops.request_weather_refresh(commands->ctx) != 0) {
            /* The location remains committed; the next scheduled weather
             * poll will retry if the asynchronous queue is temporarily full. */
            return BLE_COMMAND_BUSY;
        }
        return BLE_COMMAND_OK;
    }

    case BLE_CMD_WIFI_SET_CREDENTIALS: {
        /* Payload: u8 ssid_len, u8 password_len, SSID bytes, password bytes.
         * The credentials never travel through the console or SmartConfig. */
        if (frame->payload_len < 2u) return BLE_COMMAND_INVALID;
        const size_t ssid_len = frame->payload[0];
        const size_t password_len = frame->payload[1];
        if (ssid_len > NVS_STORE_SSID_MAX ||
            password_len > NVS_STORE_PASSWORD_MAX ||
            (ssid_len == 0 && password_len != 0) ||
            frame->payload_len != 2u + ssid_len + password_len ||
            !wifi_credential_bytes_valid(frame->payload + 2u, ssid_len,
                                          NVS_STORE_SSID_MAX, ssid_len == 0) ||
            !wifi_credential_bytes_valid(frame->payload + 2u + ssid_len,
                                          password_len,
                                          NVS_STORE_PASSWORD_MAX, true)) {
            return BLE_COMMAND_INVALID;
        }
        if (!commands->ops.apply_wifi_credentials) {
            return BLE_COMMAND_NOT_SUPPORTED;
        }
        wifi_credentials_t credentials = {0};
        memcpy(credentials.ssid, frame->payload + 2u, ssid_len);
        memcpy(credentials.password, frame->payload + 2u + ssid_len,
               password_len);
        const ble_command_result_t status = update_config(
            commands, set_wifi_credentials, &credentials);
        if (status != BLE_COMMAND_OK) return status;
        return commands->ops.apply_wifi_credentials(commands->ctx) == 0
                   ? BLE_COMMAND_OK : BLE_COMMAND_STATE_ERROR;
    }

    case BLE_CMD_WEATHER_REFRESH:
        if (!payload_is_empty(frame)) return BLE_COMMAND_INVALID;
        if (!commands->ops.request_weather_refresh) {
            return BLE_COMMAND_NOT_SUPPORTED;
        }
        return commands->ops.request_weather_refresh(commands->ctx) == 0
                   ? BLE_COMMAND_OK : BLE_COMMAND_BUSY;

    case BLE_CMD_PHOTO_BEGIN: {
        if (frame->payload_len != 13) return BLE_COMMAND_INVALID;
        const photo_store_begin_t begin = {
            .photo_id = read_le32(frame->payload),
            .slot = frame->payload[4],
            .total_len = read_le32(frame->payload + 5),
            .crc32 = read_le32(frame->payload + 9),
        };
        if (begin.slot >= PHOTO_STORE_SLOT_COUNT ||
            begin.total_len != PHOTO_STORE_BYTES) {
            return BLE_COMMAND_INVALID;
        }
        if (commands->upload.active) {
            const photo_store_upload_t *active = &commands->upload.state;
            if (active->photo_id != begin.photo_id || active->slot != begin.slot ||
                active->total_len != begin.total_len ||
                active->expected_crc32 != begin.crc32) {
                return BLE_COMMAND_BUSY;
            }
        } else {
            const int status = ble_upload_begin(&commands->upload, &begin);
            if (status != PHOTO_STORE_OK) return photo_status_to_command(status);
        }
        reply->data[reply->length++] = begin.slot;
        append_u32(reply, ble_upload_next_offset(&commands->upload));
        append_u32(reply, begin.total_len);
        return BLE_COMMAND_OK;
    }

    case BLE_CMD_PHOTO_DATA: {
        if (frame->payload_len <= 4 || !commands->upload.active) {
            return frame->payload_len <= 4 ? BLE_COMMAND_INVALID :
                                            BLE_COMMAND_STATE_ERROR;
        }
        const uint32_t offset = read_le32(frame->payload);
        const size_t data_len = frame->payload_len - 4u;
        const int status = ble_upload_data(&commands->upload, offset,
                                           frame->payload + 4, data_len);
        if (status != PHOTO_STORE_OK) return photo_status_to_command(status);
        append_u32(reply, ble_upload_next_offset(&commands->upload));
        return BLE_COMMAND_OK;
    }

    case BLE_CMD_PHOTO_END: {
        if (frame->payload_len != 4 || !commands->upload.active) {
            return frame->payload_len != 4 ? BLE_COMMAND_INVALID :
                                            BLE_COMMAND_STATE_ERROR;
        }
        photo_store_info_t info = {0};
        const uint32_t crc32 = read_le32(frame->payload);
        const int status = ble_upload_end(&commands->upload, crc32, &info);
        if (status != PHOTO_STORE_OK) {
            ble_upload_abort(&commands->upload);
            return photo_status_to_command(status);
        }
        const ble_command_result_t config_status = update_config(
            commands, set_photo_slot, &info.slot);
        if (config_status != BLE_COMMAND_OK) return config_status;
        reply->data[reply->length++] = info.slot;
        append_u32(reply, info.photo_id);
        append_u32(reply, info.length);
        return BLE_COMMAND_OK;
    }

    case BLE_CMD_PHOTO_ABORT:
        if (!payload_is_empty(frame)) return BLE_COMMAND_INVALID;
        if (!commands->upload.active) return BLE_COMMAND_STATE_ERROR;
        ble_upload_abort(&commands->upload);
        return BLE_COMMAND_OK;

    case BLE_CMD_SET_TODAY_PLAN: {
        char plan[NVS_STORE_TODAY_PLAN_MAX + 1] = {0};
        if (ble_proto_decode_today_plan(frame->payload, frame->payload_len,
                                        plan, sizeof(plan)) != BLE_PROTO_OK) {
            return BLE_COMMAND_INVALID;
        }
        return update_config(commands, set_today_plan, plan);
    }

    default:
        return BLE_COMMAND_NOT_SUPPORTED;
    }
}

ble_command_result_t ble_commands_process_ex(ble_commands_t *commands,
                                              const ble_frame_view_t *frame,
                                              ble_command_reply_t *reply)
{
    static ble_command_reply_t discarded;
    if (!reply) reply = &discarded;
    reply->length = 0;
    if (!commands || !commands->config || !frame ||
        frame->version != BLE_PROTO_VERSION ||
        (frame->payload_len != 0 && !frame->payload) ||
        frame->payload_len > BLE_PROTO_MAX_PAYLOAD) {
        return BLE_COMMAND_INVALID;
    }

    const uint32_t payload_crc = ble_proto_crc32(frame->payload,
                                                 frame->payload_len);
    for (size_t i = 0; i < BLE_COMMAND_REPLAY_CACHE_SIZE; ++i) {
        const ble_command_cache_entry_t *entry = &commands->replay_cache[i];
        if (!entry->valid || entry->sequence != frame->sequence) continue;
        if (entry->type != frame->type ||
            entry->payload_len != frame->payload_len ||
            entry->payload_crc32 != payload_crc) {
            return BLE_COMMAND_INVALID;
        }
        *reply = entry->reply;
        return (ble_command_result_t)entry->status;
    }

    const ble_command_result_t status = process_uncached(commands, frame, reply);
    ble_command_cache_entry_t *entry =
        &commands->replay_cache[commands->replay_next];
    entry->valid = true;
    entry->type = frame->type;
    entry->status = (uint8_t)status;
    entry->sequence = frame->sequence;
    entry->payload_len = frame->payload_len;
    entry->payload_crc32 = payload_crc;
    entry->reply = *reply;
    commands->replay_next = (uint8_t)((commands->replay_next + 1u) %
                                      BLE_COMMAND_REPLAY_CACHE_SIZE);
    return status;
}

ble_command_result_t ble_commands_process(ble_commands_t *commands,
                                          const ble_frame_view_t *frame)
{
    return ble_commands_process_ex(commands, frame, NULL);
}

void ble_commands_on_frame(const ble_frame_view_t *frame, void *ctx)
{
    (void)ble_commands_process((ble_commands_t *)ctx, frame);
}
