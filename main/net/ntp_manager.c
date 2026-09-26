#include "ntp_manager.h"

#include <string.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_netif_sntp.h"
#endif

static const char *default_timezone = "Asia/Shanghai";

void ntp_manager_init(ntp_manager_t *manager, const char *timezone)
{
    if (!manager) return;
    memset(manager, 0, sizeof(*manager));
    ntp_manager_set_timezone(manager, timezone ? timezone : default_timezone);
}

int ntp_manager_set_timezone(ntp_manager_t *manager, const char *timezone)
{
    if (!manager || !timezone || !timezone[0] || strlen(timezone) >= sizeof(manager->timezone)) {
        return -1;
    }
    strcpy(manager->timezone, timezone);
#ifdef ESP_PLATFORM
    setenv("TZ", manager->timezone, 1);
    tzset();
#endif
    return 0;
}

int ntp_manager_start(ntp_manager_t *manager)
{
    if (!manager) return -1;
    if (!manager->timezone[0]) ntp_manager_set_timezone(manager, default_timezone);
#ifdef ESP_PLATFORM
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = false;
    if (esp_netif_sntp_init(&config) != ESP_OK) return -1;
    if (esp_netif_sntp_start() != ESP_OK) return -1;
    setenv("TZ", manager->timezone, 1);
    tzset();
#endif
    manager->started = true;
    return 0;
}

bool ntp_manager_is_synced(const ntp_manager_t *manager)
{
    if (!manager || !manager->started) return false;
#ifdef ESP_PLATFORM
    return esp_netif_sntp_sync_wait(0) == ESP_OK;
#else
    return manager->synced;
#endif
}

bool ntp_manager_get_localtime(const ntp_manager_t *manager, struct tm *out)
{
    if (!manager || !out) return false;
    time_t now = time(NULL);
    if (now < 1000000000) return false;
    return localtime_r(&now, out) != NULL;
}
