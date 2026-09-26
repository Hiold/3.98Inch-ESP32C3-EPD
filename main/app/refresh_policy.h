/* Refresh request arbitration shared by UI, networking and BLE tasks. */
#ifndef REFRESH_POLICY_H
#define REFRESH_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Automatic full-screen updates must never happen more often than this. */
#define REFRESH_POLICY_MIN_INTERVAL_MS (15ULL * 60ULL * 1000ULL)
#define REFRESH_POLICY_DEFAULT_INTERVAL_MS REFRESH_POLICY_MIN_INTERVAL_MS
#define REFRESH_POLICY_MIN_INTERVAL_SEC (15u * 60u)
#define REFRESH_POLICY_DEFAULT_INTERVAL_SEC REFRESH_POLICY_MIN_INTERVAL_SEC

typedef enum {
    REFRESH_REASON_NONE = 0,
    REFRESH_REASON_BOOT = 1u << 0,
    REFRESH_REASON_AUTO_TIMER = 1u << 1,
    REFRESH_REASON_NTP_SYNC = 1u << 2,
    REFRESH_REASON_WEATHER = 1u << 3,
    REFRESH_REASON_PHOTO_UPLOAD = 1u << 4,
    REFRESH_REASON_MODE_CHANGE = 1u << 5,
    REFRESH_REASON_CONFIG_CHANGE = 1u << 6,
    REFRESH_REASON_BLE = 1u << 7,
    REFRESH_REASON_USER = 1u << 8,
} refresh_reason_t;

typedef enum {
    REFRESH_POLICY_REJECTED_INTERVAL = 0,
    REFRESH_POLICY_QUEUED,
    REFRESH_POLICY_MERGED,
} refresh_policy_result_t;

typedef struct {
    uint32_t min_interval_ms;
    uint64_t last_refresh_ms;
    uint32_t last_reason_mask;
    uint32_t last_duration_ms;
    uint64_t request_ms;
    uint64_t refresh_start_ms;
    uint32_t pending_reason_mask;
    bool has_last_refresh;
    bool refreshing;
    bool pending;
    bool pending_force;
} refresh_policy_t;

/* Initialize an empty policy. A zero interval or a value below the hard
 * minimum is clamped to REFRESH_POLICY_MIN_INTERVAL_MS. */
void refresh_policy_init(refresh_policy_t *policy, uint64_t now_ms,
                         uint32_t min_interval_ms);

uint32_t refresh_policy_clamp_interval(uint32_t interval_ms);
bool refresh_policy_set_interval(refresh_policy_t *policy, uint32_t interval_ms);
uint32_t refresh_policy_interval(const refresh_policy_t *policy);

/* Return true when an automatic request is allowed at now_ms. */
bool refresh_policy_interval_elapsed(const refresh_policy_t *policy,
                                     uint64_t now_ms);

/* Submit a request. Forced requests bypass the interval gate (but are still
 * serialized while a refresh is in progress). Requests arriving during an
 * active refresh, or while another request is pending, are merged into the
 * pending reason bitmask. */
refresh_policy_result_t refresh_policy_request(refresh_policy_t *policy,
                                               uint64_t now_ms,
                                               uint32_t reason_mask,
                                               bool force);

/* Begin the next queued refresh. Returns false when there is no request. The
 * returned reason mask is cleared from the pending queue. */
bool refresh_policy_begin(refresh_policy_t *policy, uint64_t now_ms,
                          uint32_t *reason_mask);

/* Complete a refresh and record its duration/causes. If a request arrived
 * during the operation, it remains queued for the next begin() call. */
void refresh_policy_complete(refresh_policy_t *policy, uint64_t now_ms,
                             uint32_t reason_mask);

bool refresh_policy_is_refreshing(const refresh_policy_t *policy);
bool refresh_policy_has_pending(const refresh_policy_t *policy);
uint32_t refresh_policy_pending_reasons(const refresh_policy_t *policy);
uint64_t refresh_policy_last_refresh_ms(const refresh_policy_t *policy);
uint32_t refresh_policy_last_duration_ms(const refresh_policy_t *policy);
uint32_t refresh_policy_last_reasons(const refresh_policy_t *policy);

#ifdef __cplusplus
}
#endif

#endif /* REFRESH_POLICY_H */
