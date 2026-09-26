#ifndef UI_CALENDAR_H
#define UI_CALENDAR_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    struct tm local_time;
    int16_t temperature_c10;
    const char *weather_summary;
    int weather_age_hours;
    bool wifi_connected;
    bool ntp_synced;
} ui_calendar_data_t;

/* Format the calendar's hour-level status without a minute-driven redraw. */
int ui_calendar_format_hour(const struct tm *time, char *out, size_t capacity);
int ui_calendar_format_weather(const ui_calendar_data_t *data,
                               char *out, size_t capacity);

#endif
