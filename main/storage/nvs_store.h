#ifndef NVS_STORE_H
#define NVS_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_STORE_SSID_MAX 32
#define NVS_STORE_PASSWORD_MAX 64
#define NVS_STORE_TIMEZONE_MAX 63
#define NVS_STORE_TODAY_PLAN_MAX 30
#define NVS_STORE_MIN_REFRESH_SEC (15u * 60u)
#define NVS_STORE_DEFAULT_REFRESH_SEC (15u * 60u)
#define NVS_STORE_DEFAULT_WIFI_TX_QDBM 28u

typedef enum {
    NVS_STORE_MODE_CALENDAR = 0,
    NVS_STORE_MODE_PHOTO = 1,
    NVS_STORE_MODE_CALENDAR_PHOTO = 2,
    NVS_STORE_MODE_STATUS = 3,
} nvs_store_home_mode_t;

typedef struct {
    char wifi_ssid[NVS_STORE_SSID_MAX + 1];
    char wifi_password[NVS_STORE_PASSWORD_MAX + 1];
    uint8_t rotation;
    uint8_t home_mode;
    uint8_t current_photo_slot;
    char timezone[NVS_STORE_TIMEZONE_MAX + 1];
    char today_plan[NVS_STORE_TODAY_PLAN_MAX + 1];
    int32_t latitude_e7;
    int32_t longitude_e7;
    uint32_t auto_refresh_interval_sec;
    uint64_t weather_cache_unix;
    uint8_t wifi_tx_power_qdbm;
    uint8_t ble_tx_power_level;
} nvs_store_config_t;

void nvs_store_config_defaults(nvs_store_config_t *config);
/* Returns false for malformed values; does not modify the input. */
bool nvs_store_config_is_valid(const nvs_store_config_t *config);
/* Applies defaults and clamps safety limits. Returns false for NULL. */
bool nvs_store_config_normalize(nvs_store_config_t *config);

#ifdef ESP_PLATFORM
#include "esp_err.h"
esp_err_t nvs_store_init(void);
esp_err_t nvs_store_load(nvs_store_config_t *config);
esp_err_t nvs_store_save(const nvs_store_config_t *config);
esp_err_t nvs_store_erase(void);
#else
/* Host builds can use the pure config helpers without ESP-IDF. */
typedef int esp_err_t;
#define ESP_OK 0
esp_err_t nvs_store_init(void);
esp_err_t nvs_store_load(nvs_store_config_t *config);
esp_err_t nvs_store_save(const nvs_store_config_t *config);
esp_err_t nvs_store_erase(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
