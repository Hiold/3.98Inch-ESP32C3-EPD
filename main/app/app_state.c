#include "app_state.h"

#include <stddef.h>

void app_state_init(app_state_t *state, uint64_t now_ms)
{
    if (!state) return;
    nvs_store_config_defaults(&state->config);
    refresh_policy_init(&state->refresh, now_ms,
                        state->config.auto_refresh_interval_sec * 1000u);
    state->wifi_connected = false;
    state->ntp_synced = false;
    state->weather_valid = false;
    state->weather_stale = false;
}

bool app_state_set_refresh_interval(app_state_t *state, uint32_t seconds)
{
    if (!state) return false;
    const uint32_t minimum = NVS_STORE_MIN_REFRESH_SEC;
    if (seconds < minimum) seconds = minimum;
    state->config.auto_refresh_interval_sec = seconds;
    return refresh_policy_set_interval(&state->refresh, seconds * 1000u);
}

bool app_state_request_refresh(app_state_t *state, uint64_t now_ms,
                               uint32_t reason, bool force)
{
    if (!state) return false;
    return refresh_policy_request(&state->refresh, now_ms, reason, force) !=
           REFRESH_POLICY_REJECTED_INTERVAL;
}
