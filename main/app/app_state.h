#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "refresh_policy.h"
#include "../storage/nvs_store.h"

typedef struct {
    nvs_store_config_t config;
    refresh_policy_t refresh;
    bool wifi_connected;
    bool ntp_synced;
    bool weather_valid;
    bool weather_stale;
    int16_t weather_temperature_c10;
    uint64_t weather_updated_unix;
} app_state_t;

void app_state_init(app_state_t *state, uint64_t now_ms);
bool app_state_set_refresh_interval(app_state_t *state, uint32_t seconds);
bool app_state_request_refresh(app_state_t *state, uint64_t now_ms,
                               uint32_t reason, bool force);

#endif
