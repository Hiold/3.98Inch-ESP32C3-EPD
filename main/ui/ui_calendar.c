#include "ui_calendar.h"

#include <stdio.h>

int ui_calendar_format_hour(const struct tm *time, char *out, size_t capacity)
{
    if (!time || !out || capacity < 6) return -1;
    /* Deliberately omit seconds and use a stable hour marker. */
    return snprintf(out, capacity, "%02d:00", time->tm_hour);
}

int ui_calendar_format_weather(const ui_calendar_data_t *data,
                               char *out, size_t capacity)
{
    if (!data || !out || capacity == 0) return -1;
    if (!data->weather_summary || !data->weather_summary[0]) {
        return snprintf(out, capacity, "天气：--");
    }
    const int temperature = data->temperature_c10;
    const unsigned magnitude = (unsigned)(temperature < 0 ? -temperature : temperature);
    const char *suffix = data->weather_age_hours > 0 ? "（过期）" : "";
    return snprintf(out, capacity, "%s %s%d.%u°C%s", data->weather_summary,
                    temperature < 0 ? "-" : "", magnitude / 10u,
                    magnitude % 10u, suffix);
}
