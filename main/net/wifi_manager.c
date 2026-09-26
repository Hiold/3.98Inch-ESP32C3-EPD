#include "wifi_manager.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_wifi.h"
#endif

bool wifi_manager_has_credentials(const nvs_store_config_t *config)
{
    return config && config->wifi_ssid[0] != '\0';
}

int wifi_manager_apply_tx_limit(void)
{
#ifdef ESP_PLATFORM
    /* esp_wifi uses quarter-dBm units. The PHY/eFuse may apply a lower limit. */
    esp_err_t err = esp_wifi_set_max_tx_power(WIFI_MANAGER_MAX_TX_POWER_QDBM);
    if (err != ESP_OK) {
        /* IDF 6.0.2 exposes a target-specific C3 ceiling of 20 qdBm.
         * Keep the requested 7 dBm ceiling in the source configuration, but
         * use the SDK's stricter effective ceiling when it rejects 28. */
        int fallback = CONFIG_ESP_PHY_MAX_WIFI_TX_POWER;
        if (fallback > WIFI_MANAGER_MAX_TX_POWER_QDBM) {
            fallback = WIFI_MANAGER_MAX_TX_POWER_QDBM;
        }
        err = esp_wifi_set_max_tx_power((int8_t)fallback);
    }
    return (int)err;
#else
    return 0;
#endif
}

int8_t wifi_manager_effective_tx_power_qdbm(void)
{
#ifdef ESP_PLATFORM
    return (int8_t)(CONFIG_ESP_PHY_MAX_WIFI_TX_POWER < WIFI_MANAGER_MAX_TX_POWER_QDBM
                        ? CONFIG_ESP_PHY_MAX_WIFI_TX_POWER
                        : WIFI_MANAGER_MAX_TX_POWER_QDBM);
#else
    return WIFI_MANAGER_MAX_TX_POWER_QDBM;
#endif
}

void wifi_manager_init(wifi_manager_t *manager,
                       const nvs_store_config_t *config)
{
    if (!manager) return;
    memset(manager, 0, sizeof(*manager));
    manager->state = WIFI_MANAGER_DISCONNECTED;
    manager->applied_tx_power_qdbm = wifi_manager_effective_tx_power_qdbm();
    manager->credentials_present = wifi_manager_has_credentials(config);
}

wifi_manager_state_t wifi_manager_state(const wifi_manager_t *manager)
{
    return manager ? manager->state : WIFI_MANAGER_DISCONNECTED;
}

int wifi_manager_start(wifi_manager_t *manager,
                       const nvs_store_config_t *config)
{
    if (!manager || !wifi_manager_has_credentials(config)) return -1;
    manager->credentials_present = true;
    manager->state = WIFI_MANAGER_CONNECTING;
#ifdef ESP_PLATFORM
    wifi_config_t sta = {0};
    memcpy(sta.sta.ssid, config->wifi_ssid,
           strnlen(config->wifi_ssid, sizeof(sta.sta.ssid)));
    memcpy(sta.sta.password, config->wifi_password,
           strnlen(config->wifi_password, sizeof(sta.sta.password)));
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        manager->state = WIFI_MANAGER_DISCONNECTED;
        return (int)err;
    }
    (void)wifi_manager_apply_tx_limit();
#endif
    return 0;
}
