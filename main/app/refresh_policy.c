#include "refresh_policy.h"

#include <stddef.h>

static uint64_t elapsed_ms(uint64_t now, uint64_t then)
{
    /* Unsigned subtraction deliberately handles a wrapping monotonic clock. */
    return now - then;
}

uint32_t refresh_policy_clamp_interval(uint32_t interval_ms)
{
    if (interval_ms < (uint32_t)REFRESH_POLICY_MIN_INTERVAL_MS) {
        return (uint32_t)REFRESH_POLICY_MIN_INTERVAL_MS;
    }
    return interval_ms;
}

void refresh_policy_init(refresh_policy_t *policy, uint64_t now_ms,
                         uint32_t min_interval_ms)
{
    if (policy == NULL) {
        return;
    }
    *policy = (refresh_policy_t){
        .min_interval_ms = refresh_policy_clamp_interval(min_interval_ms),
        .request_ms = now_ms,
    };
}

bool refresh_policy_set_interval(refresh_policy_t *policy, uint32_t interval_ms)
{
    if (policy == NULL) {
        return false;
    }
    const uint32_t clamped = refresh_policy_clamp_interval(interval_ms);
    const bool changed = policy->min_interval_ms != clamped;
    policy->min_interval_ms = clamped;
    return changed;
}

uint32_t refresh_policy_interval(const refresh_policy_t *policy)
{
    return policy != NULL ? policy->min_interval_ms
                          : (uint32_t)REFRESH_POLICY_DEFAULT_INTERVAL_MS;
}

bool refresh_policy_interval_elapsed(const refresh_policy_t *policy,
                                     uint64_t now_ms)
{
    if (policy == NULL || !policy->has_last_refresh) {
        return true;
    }
    return elapsed_ms(now_ms, policy->last_refresh_ms) >= policy->min_interval_ms;
}

refresh_policy_result_t refresh_policy_request(refresh_policy_t *policy,
                                               uint64_t now_ms,
                                               uint32_t reason_mask,
                                               bool force)
{
    if (policy == NULL || reason_mask == REFRESH_REASON_NONE) {
        return REFRESH_POLICY_REJECTED_INTERVAL;
    }

    /* Requests arriving while the EPD is busy are retained and merged. The
     * dispatch-time check in begin() will defer a non-forced request if the
     * just-completed frame restarted the automatic interval. */
    const bool allowed = policy->refreshing || policy->pending || force ||
                         refresh_policy_interval_elapsed(policy, now_ms);
    if (!allowed) {
        return REFRESH_POLICY_REJECTED_INTERVAL;
    }

    const bool already_pending = policy->pending;
    policy->pending = true;
    policy->pending_force = policy->pending_force || force;
    policy->pending_reason_mask |= reason_mask;
    policy->request_ms = now_ms;
    return already_pending || policy->refreshing ? REFRESH_POLICY_MERGED
                                                 : REFRESH_POLICY_QUEUED;
}

bool refresh_policy_begin(refresh_policy_t *policy, uint64_t now_ms,
                          uint32_t *reason_mask)
{
    if (policy == NULL || policy->refreshing || !policy->pending) {
        return false;
    }
    /* A request can sit behind an already-running forced refresh. Re-check
     * the automatic interval at dispatch time so it cannot accidentally cause
     * two full-screen updates less than 15 minutes apart. */
    if (!policy->pending_force &&
        !refresh_policy_interval_elapsed(policy, now_ms)) {
        return false;
    }
    policy->refreshing = true;
    policy->refresh_start_ms = now_ms;
    if (reason_mask != NULL) {
        *reason_mask = policy->pending_reason_mask;
    }
    policy->pending_reason_mask = REFRESH_REASON_NONE;
    policy->pending = false;
    policy->pending_force = false;
    return true;
}

void refresh_policy_complete(refresh_policy_t *policy, uint64_t now_ms,
                             uint32_t reason_mask)
{
    if (policy == NULL) {
        return;
    }
    policy->refreshing = false;
    policy->has_last_refresh = true;
    policy->last_refresh_ms = now_ms;
    policy->last_reason_mask = reason_mask;
    const uint64_t duration = elapsed_ms(now_ms, policy->refresh_start_ms);
    policy->last_duration_ms = duration > UINT32_MAX ? UINT32_MAX : (uint32_t)duration;
}

bool refresh_policy_is_refreshing(const refresh_policy_t *policy)
{
    return policy != NULL && policy->refreshing;
}

bool refresh_policy_has_pending(const refresh_policy_t *policy)
{
    return policy != NULL && policy->pending;
}

uint32_t refresh_policy_pending_reasons(const refresh_policy_t *policy)
{
    return policy != NULL ? policy->pending_reason_mask : REFRESH_REASON_NONE;
}

uint64_t refresh_policy_last_refresh_ms(const refresh_policy_t *policy)
{
    return policy != NULL ? policy->last_refresh_ms : 0;
}

uint32_t refresh_policy_last_duration_ms(const refresh_policy_t *policy)
{
    return policy != NULL ? policy->last_duration_ms : 0;
}

uint32_t refresh_policy_last_reasons(const refresh_policy_t *policy)
{
    return policy != NULL ? policy->last_reason_mask : REFRESH_REASON_NONE;
}
