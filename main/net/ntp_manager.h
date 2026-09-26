#ifndef NTP_MANAGER_H
#define NTP_MANAGER_H

#include <stdbool.h>
#include <time.h>

typedef struct {
    char timezone[64];
    bool started;
    bool synced;
} ntp_manager_t;

void ntp_manager_init(ntp_manager_t *manager, const char *timezone);
int ntp_manager_set_timezone(ntp_manager_t *manager, const char *timezone);
int ntp_manager_start(ntp_manager_t *manager);
bool ntp_manager_is_synced(const ntp_manager_t *manager);
bool ntp_manager_get_localtime(const ntp_manager_t *manager, struct tm *out);

#endif
