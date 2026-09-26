#ifndef UI_APP_H
#define UI_APP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_MODE_CALENDAR = 0,
    UI_MODE_PHOTO,
    UI_MODE_CALENDAR_PHOTO,
    UI_MODE_STATUS,
} ui_mode_t;

typedef struct {
    ui_mode_t mode;
    uint8_t photo_slot;
    /* NVS stores 0..3; the renderer maps this to 0/90/180/270 degrees. */
    uint8_t rotation;
    bool wifi_connected;
    bool ntp_synced;
    bool weather_stale;
    int16_t temperature_c10;
    char weather_summary[48];
    char timezone[64];
    char today_plan[31];
} ui_app_state_t;

void ui_app_init(void);
void ui_app_set_state(const ui_app_state_t *state);
ui_mode_t ui_app_mode(void);
int ui_app_render(void);

#endif
