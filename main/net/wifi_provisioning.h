#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t failure_threshold;
    uint8_t failures;
    bool active;
} wifi_provisioning_t;

void wifi_provisioning_init(wifi_provisioning_t *provisioning,
                            uint8_t failure_threshold);
bool wifi_provisioning_note_failure(wifi_provisioning_t *provisioning);
void wifi_provisioning_begin(wifi_provisioning_t *provisioning);
void wifi_provisioning_stop(wifi_provisioning_t *provisioning);
bool wifi_provisioning_active(const wifi_provisioning_t *provisioning);

#endif
