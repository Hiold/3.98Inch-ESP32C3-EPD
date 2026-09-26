#include "nvs_store.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
#include "nvs_flash.h"
#endif

static bool bounded_string_ok(const char *s, size_t max_len)
{
    if (!s) return false;
    for (size_t i = 0; i <= max_len; ++i) {
        if (s[i] == '\0') return true;
    }
    return false;
}

static bool utf8_text_ok(const char *text, size_t max_bytes)
{
    if (!bounded_string_ok(text, max_bytes)) return false;
    const uint8_t *p = (const uint8_t *)text;
    size_t length = strlen(text);
    size_t i = 0;
    while (i < length) {
        const uint8_t lead = p[i];
        if (lead < 0x80u) {
            if (lead < 0x20u || lead == 0x7fu) return false;
            ++i;
            continue;
        }
        unsigned extra;
        uint32_t codepoint;
        if (lead >= 0xc2u && lead <= 0xdfu) {
            extra = 1;
            codepoint = lead & 0x1fu;
        } else if (lead >= 0xe0u && lead <= 0xefu) {
            extra = 2;
            codepoint = lead & 0x0fu;
        } else if (lead >= 0xf0u && lead <= 0xf4u) {
            extra = 3;
            codepoint = lead & 0x07u;
        } else {
            return false;
        }
        if (i + extra >= length) return false;
        for (unsigned n = 1; n <= extra; ++n) {
            if ((p[i + n] & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6) | (p[i + n] & 0x3fu);
        }
        if ((extra == 2 && codepoint < 0x800u) ||
            (extra == 3 && codepoint < 0x10000u) ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
            codepoint > 0x10ffffu) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

void nvs_store_config_defaults(nvs_store_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    strcpy(config->timezone, "Asia/Shanghai");
    strcpy(config->today_plan, "无");
    config->home_mode = NVS_STORE_MODE_CALENDAR;
    config->auto_refresh_interval_sec = NVS_STORE_DEFAULT_REFRESH_SEC;
    config->wifi_tx_power_qdbm = NVS_STORE_DEFAULT_WIFI_TX_QDBM;
}

bool nvs_store_config_is_valid(const nvs_store_config_t *config)
{
    if (!config || !bounded_string_ok(config->wifi_ssid, NVS_STORE_SSID_MAX) ||
        !bounded_string_ok(config->wifi_password, NVS_STORE_PASSWORD_MAX) ||
        !bounded_string_ok(config->timezone, NVS_STORE_TIMEZONE_MAX) ||
        !utf8_text_ok(config->today_plan, NVS_STORE_TODAY_PLAN_MAX)) {
        return false;
    }
    if (config->rotation > 3 || config->home_mode > NVS_STORE_MODE_STATUS ||
        config->current_photo_slot >= 4 ||
        config->auto_refresh_interval_sec < NVS_STORE_MIN_REFRESH_SEC ||
        config->wifi_tx_power_qdbm > NVS_STORE_DEFAULT_WIFI_TX_QDBM) {
        return false;
    }
    if (config->latitude_e7 < -900000000 || config->latitude_e7 > 900000000 ||
        config->longitude_e7 < -1800000000 || config->longitude_e7 > 1800000000) {
        return false;
    }
    return true;
}

bool nvs_store_config_normalize(nvs_store_config_t *config)
{
    if (!config) {
        return false;
    }
    config->wifi_ssid[NVS_STORE_SSID_MAX] = '\0';
    config->wifi_password[NVS_STORE_PASSWORD_MAX] = '\0';
    config->timezone[NVS_STORE_TIMEZONE_MAX] = '\0';
    config->today_plan[NVS_STORE_TODAY_PLAN_MAX] = '\0';
    if (config->rotation > 3) config->rotation = 0;
    if (config->home_mode > NVS_STORE_MODE_STATUS) config->home_mode = NVS_STORE_MODE_CALENDAR;
    if (config->current_photo_slot >= 4) config->current_photo_slot = 0;
    if (config->auto_refresh_interval_sec < NVS_STORE_MIN_REFRESH_SEC) {
        config->auto_refresh_interval_sec = NVS_STORE_MIN_REFRESH_SEC;
    }
    if (config->wifi_tx_power_qdbm > NVS_STORE_DEFAULT_WIFI_TX_QDBM) {
        config->wifi_tx_power_qdbm = NVS_STORE_DEFAULT_WIFI_TX_QDBM;
    }
    if (config->latitude_e7 < -900000000 || config->latitude_e7 > 900000000) config->latitude_e7 = 0;
    if (config->longitude_e7 < -1800000000 || config->longitude_e7 > 1800000000) config->longitude_e7 = 0;
    return nvs_store_config_is_valid(config);
}

#ifdef ESP_PLATFORM
static const char *NVS_NAMESPACE = "epd";

esp_err_t nvs_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) return err;
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t nvs_store_load(nvs_store_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    nvs_store_config_defaults(config);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
#define GET_BLOB(name) do { size_t sz = sizeof(config->name); esp_err_t e = nvs_get_blob(handle, #name, config->name, &sz); if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) { nvs_close(handle); return e; } } while (0)
    GET_BLOB(wifi_ssid); GET_BLOB(wifi_password); GET_BLOB(timezone);
    GET_BLOB(today_plan);
#undef GET_BLOB
    nvs_get_u8(handle, "rotation", &config->rotation);
    nvs_get_u8(handle, "home_mode", &config->home_mode);
    nvs_get_u8(handle, "photo_slot", &config->current_photo_slot);
    nvs_get_i32(handle, "latitude", &config->latitude_e7);
    nvs_get_i32(handle, "longitude", &config->longitude_e7);
    nvs_get_u32(handle, "refresh_sec", &config->auto_refresh_interval_sec);
    nvs_get_u64(handle, "weather_time", &config->weather_cache_unix);
    nvs_get_u8(handle, "wifi_tx_qdbm", &config->wifi_tx_power_qdbm);
    nvs_get_u8(handle, "ble_tx_level", &config->ble_tx_power_level);
    nvs_close(handle);
    return nvs_store_config_normalize(config) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t nvs_store_save(const nvs_store_config_t *config)
{
    if (!config || !nvs_store_config_is_valid(config)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
#define SET_BLOB(name) do { err = nvs_set_blob(handle, #name, config->name, sizeof(config->name)); if (err != ESP_OK) goto done; } while (0)
    SET_BLOB(wifi_ssid); SET_BLOB(wifi_password); SET_BLOB(timezone);
    SET_BLOB(today_plan);
#undef SET_BLOB
    if ((err = nvs_set_u8(handle, "rotation", config->rotation)) != ESP_OK) goto done;
    if ((err = nvs_set_u8(handle, "home_mode", config->home_mode)) != ESP_OK) goto done;
    if ((err = nvs_set_u8(handle, "photo_slot", config->current_photo_slot)) != ESP_OK) goto done;
    if ((err = nvs_set_i32(handle, "latitude", config->latitude_e7)) != ESP_OK) goto done;
    if ((err = nvs_set_i32(handle, "longitude", config->longitude_e7)) != ESP_OK) goto done;
    if ((err = nvs_set_u32(handle, "refresh_sec", config->auto_refresh_interval_sec)) != ESP_OK) goto done;
    if ((err = nvs_set_u64(handle, "weather_time", config->weather_cache_unix)) != ESP_OK) goto done;
    if ((err = nvs_set_u8(handle, "wifi_tx_qdbm", config->wifi_tx_power_qdbm)) != ESP_OK) goto done;
    if ((err = nvs_set_u8(handle, "ble_tx_level", config->ble_tx_power_level)) != ESP_OK) goto done;
    err = nvs_commit(handle);
done:
    nvs_close(handle);
    return err;
}

esp_err_t nvs_store_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
#else
esp_err_t nvs_store_init(void) { return ESP_OK; }
esp_err_t nvs_store_load(nvs_store_config_t *config) { if (!config) return -1; nvs_store_config_defaults(config); return ESP_OK; }
esp_err_t nvs_store_save(const nvs_store_config_t *config) { return (config && nvs_store_config_is_valid(config)) ? ESP_OK : -1; }
esp_err_t nvs_store_erase(void) { return ESP_OK; }
#endif
