#include "wifi_provisioning.h"

#include <string.h>

void wifi_provisioning_init(wifi_provisioning_t *provisioning,
                            uint8_t failure_threshold)
{
    if (!provisioning) return;
    memset(provisioning, 0, sizeof(*provisioning));
    provisioning->failure_threshold = failure_threshold ? failure_threshold : 3;
}

bool wifi_provisioning_note_failure(wifi_provisioning_t *provisioning)
{
    if (!provisioning) return false;
    if (provisioning->failures < UINT8_MAX) provisioning->failures++;
    return provisioning->failures >= provisioning->failure_threshold;
}

void wifi_provisioning_begin(wifi_provisioning_t *provisioning)
{
    if (provisioning) provisioning->active = true;
}

void wifi_provisioning_stop(wifi_provisioning_t *provisioning)
{
    if (provisioning) {
        provisioning->active = false;
        provisioning->failures = 0;
    }
}

bool wifi_provisioning_active(const wifi_provisioning_t *provisioning)
{
    return provisioning && provisioning->active;
}
