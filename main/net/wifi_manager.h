#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "../storage/nvs_store.h"

#define WIFI_MANAGER_MAX_TX_POWER_QDBM 28

typedef enum {
    WIFI_MANAGER_DISCONNECTED = 0,
    WIFI_MANAGER_CONNECTING,
    WIFI_MANAGER_CONNECTED,
} wifi_manager_state_t;

typedef struct {
    wifi_manager_state_t state;
    uint8_t retries;
    int8_t applied_tx_power_qdbm;
    bool credentials_present;
} wifi_manager_t;

void wifi_manager_init(wifi_manager_t *manager,
                       const nvs_store_config_t *config);
int wifi_manager_apply_tx_limit(void);
bool wifi_manager_has_credentials(const nvs_store_config_t *config);
int8_t wifi_manager_effective_tx_power_qdbm(void);
wifi_manager_state_t wifi_manager_state(const wifi_manager_t *manager);
int wifi_manager_start(wifi_manager_t *manager,
                       const nvs_store_config_t *config);

#endif
