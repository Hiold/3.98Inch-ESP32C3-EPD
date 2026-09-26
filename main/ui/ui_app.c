#include "ui_app.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "display_driver.h"
#include "manual_canvas.h"
#include "photo_store.h"

static ui_app_state_t current;

static const char *const MONTHS_CN[] = {
    "一月", "二月", "三月", "四月", "五月", "六月",
    "七月", "八月", "九月", "十月", "十一月", "十二月",
};

static const char *const WEEKDAYS_CN[] = {
    "一", "二", "三", "四", "五", "六", "日",
};

enum {
    DAY_WORKDAY = 0,
    DAY_WEEKEND = 1,
    DAY_HOLIDAY = 2,
    DAY_WORK_SHIFT = 3,
};

typedef struct {
    int year;
    int month;
    int first_day;
    int last_day;
    const char *name;
} holiday_range_t;

static const holiday_range_t HOLIDAY_RANGES[] = {
    {2026, 1, 1, 1, "元旦"},
    {2026, 2, 15, 23, "春节"},
    {2026, 4, 4, 6, "清明"},
    {2026, 5, 1, 5, "劳动"},
    {2026, 6, 19, 21, "端午"},
    {2026, 9, 25, 27, "中秋"},
    {2026, 10, 1, 7, "国庆"},
};

static const holiday_range_t SHIFT_WORKDAYS[] = {
    {2026, 2, 14, 14, "调休"},
    {2026, 2, 28, 28, "调休"},
    {2026, 4, 26, 26, "调休"},
    {2026, 5, 9, 9, "调休"},
    {2026, 9, 20, 20, "调休"},
    {2026, 10, 10, 10, "调休"},
};

static const char *mode_name(ui_mode_t mode)
{
    switch (mode) {
    case UI_MODE_PHOTO:          return "相框";
    case UI_MODE_CALENDAR_PHOTO: return "日历+相框";
    case UI_MODE_STATUS:         return "状态";
    case UI_MODE_CALENDAR:
    default:                     return "日历";
    }
}

static void use_build_date(struct tm *date)
{
    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    char month[4] = {0};
    int day = 1;
    int year = 2026;
    if (sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3) return;
    for (int i = 0; i < 12; ++i) {
        if (strcmp(month, months[i]) == 0) {
            memset(date, 0, sizeof(*date));
            date->tm_mon = i;
            date->tm_mday = day;
            date->tm_year = year - 1900;
            date->tm_hour = 12;
            mktime(date);
            return;
        }
    }
}

static int month_days(const struct tm *date)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};
    if (!date || date->tm_mon < 0 || date->tm_mon > 11) return 31;
    if (date->tm_mon != 1) return days[date->tm_mon];
    const int year = date->tm_year + 1900;
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 29 : 28;
}

static int weekday_monday_first(const struct tm *date)
{
    struct tm first = *date;
    first.tm_mday = 1;
    first.tm_hour = 12;
    mktime(&first);
    return (first.tm_wday + 6) % 7;
}

static int weekday_index_monday_first(const struct tm *date)
{
    return (date->tm_wday + 6) % 7;
}

static bool in_range(const holiday_range_t *range, int year, int month, int day)
{
    return range->year == year && range->month == month + 1 &&
           day >= range->first_day && day <= range->last_day;
}

static int day_kind(int year, int month, int day, int monday_weekday)
{
    for (size_t i = 0; i < sizeof(SHIFT_WORKDAYS) / sizeof(SHIFT_WORKDAYS[0]); ++i) {
        if (in_range(&SHIFT_WORKDAYS[i], year, month, day)) return DAY_WORK_SHIFT;
    }
    for (size_t i = 0; i < sizeof(HOLIDAY_RANGES) / sizeof(HOLIDAY_RANGES[0]); ++i) {
        if (in_range(&HOLIDAY_RANGES[i], year, month, day)) return DAY_HOLIDAY;
    }
    return monday_weekday >= 5 ? DAY_WEEKEND : DAY_WORKDAY;
}

static const char *date_kind_label(const struct tm *date)
{
    const int weekday = weekday_index_monday_first(date);
    switch (day_kind(date->tm_year + 1900, date->tm_mon, date->tm_mday, weekday)) {
    case DAY_HOLIDAY: return "节假日";
    case DAY_WORK_SHIFT: return "调休工作日";
    case DAY_WEEKEND: return "周末休息";
    default: return "工作日";
    }
}

static void draw_frame(const manual_canvas_t *canvas, int32_t x, int32_t y,
                       int32_t width, int32_t height, uint8_t color)
{
    for (int thickness = 0; thickness < 2; ++thickness) {
        manual_canvas_rect(canvas, x + thickness, y + thickness,
                           width - thickness * 2, height - thickness * 2,
                           color);
    }
}

static void draw_weather_card(const manual_canvas_t *canvas, int32_t x,
                              int32_t y, int32_t width, int32_t height)
{
    draw_frame(canvas, x, y, width, height, MANUAL_COLOR_BLACK);
    manual_canvas_text_utf8(canvas, x + 10, y + 8, "天气", MANUAL_COLOR_BLACK, 1);

    uint8_t weather_kind = 0;
    const char *summary = current.weather_summary[0] ? current.weather_summary : "待更新";
    if (strstr(summary, "雨") || strstr(summary, "rain")) weather_kind = 3;
    else if (strstr(summary, "雪") || strstr(summary, "snow")) weather_kind = 4;
    else if (strstr(summary, "云") || strstr(summary, "cloud")) weather_kind = 2;
    const int icon_scale = width < 200 ? 2 : 3;
    manual_canvas_weather_icon(canvas,
                               x + width - 24 * icon_scale - 8, y + 18,
                               weather_kind,
                               current.weather_stale ? MANUAL_COLOR_YELLOW :
                                                       MANUAL_COLOR_RED,
                               icon_scale);

    char temperature[16];
    if (current.weather_summary[0]) {
        const int value = current.temperature_c10;
        const unsigned magnitude = (unsigned)(value < 0 ? -value : value);
        snprintf(temperature, sizeof(temperature), "%s%d.%d C",
                 value < 0 ? "-" : "", (int)(magnitude / 10u),
                 (int)(magnitude % 10u));
    } else {
        snprintf(temperature, sizeof(temperature), "-- C");
    }
    const int32_t temp_y = y + 52;
    const int32_t summary_y = y + 112;
    const int32_t update_y = y + height - 24;
    manual_canvas_text_utf8(canvas, x + 10, temp_y, temperature,
                            MANUAL_COLOR_RED, 1);
    manual_canvas_text_utf8(canvas, x + 10, summary_y, summary,
                            current.weather_stale ? MANUAL_COLOR_YELLOW :
                                                    MANUAL_COLOR_BLACK,
                            1);
    manual_canvas_text_compact(canvas, x + 10, update_y,
                               current.weather_stale ? "天气过期" : "每6小时更新",
                               MANUAL_COLOR_BLACK);
}

static void draw_status_footer(const manual_canvas_t *canvas, int32_t x,
                               int32_t y)
{
    manual_canvas_wifi_icon(canvas, x, y + 1, current.wifi_connected,
                            current.wifi_connected ? MANUAL_COLOR_BLACK :
                                                     MANUAL_COLOR_RED,
                            1);
    manual_canvas_text_compact(canvas, x + 20, y,
                               current.wifi_connected ? "在线" : "配网中",
                               current.wifi_connected ? MANUAL_COLOR_BLACK :
                                                        MANUAL_COLOR_RED);
    manual_canvas_text_compact(canvas, x + 78, y,
                               current.ntp_synced ? "已同步" : "未同步",
                               current.ntp_synced ? MANUAL_COLOR_BLACK :
                                                    MANUAL_COLOR_YELLOW);
}

static void draw_calendar_grid(const manual_canvas_t *canvas,
                               const struct tm *date, int32_t x, int32_t y,
                               int32_t cell_width, int32_t cell_height)
{
    const int days = month_days(date);
    const int first = weekday_monday_first(date);
    const int year = date->tm_year + 1900;
    const int32_t grid_width = cell_width * 7;
    const int32_t grid_height = cell_height * 6;
    const int32_t body_y = y + 28;

    for (int col = 0; col < 7; ++col) {
        manual_canvas_text_utf8(canvas, x + col * cell_width + cell_width / 2 - 16,
                                y - 8, WEEKDAYS_CN[col],
                                col >= 5 ? MANUAL_COLOR_RED : MANUAL_COLOR_BLACK,
                                1);
    }
    manual_canvas_rect(canvas, x, body_y, grid_width, grid_height,
                       MANUAL_COLOR_BLACK);
    for (int col = 1; col < 7; ++col) {
        manual_canvas_line(canvas, x + col * cell_width, body_y,
                           x + col * cell_width, body_y + grid_height - 1,
                           MANUAL_COLOR_BLACK);
    }
    for (int row = 1; row < 6; ++row) {
        manual_canvas_line(canvas, x, body_y + row * cell_height,
                           x + grid_width - 1, body_y + row * cell_height,
                           MANUAL_COLOR_BLACK);
    }

    char cell[12];
    for (int day = 1; day <= days; ++day) {
        const int position = first + day - 1;
        const int col = position % 7;
        const int row = position / 7;
        const int32_t cell_x = x + col * cell_width;
        const int32_t cell_y = body_y + row * cell_height;
        const int weekday = (first + day - 1) % 7;
        const int kind = day_kind(year, date->tm_mon, day, weekday);
        const bool today = day == date->tm_mday;
        if (today) {
            manual_canvas_rect_fill(canvas, cell_x + 2, cell_y + 2,
                                    cell_width - 3, cell_height - 3,
                                    MANUAL_COLOR_YELLOW);
        } else if (kind == DAY_HOLIDAY) {
            manual_canvas_rect_fill(canvas, cell_x + 2, cell_y + cell_height - 9,
                                    cell_width - 3, 7, MANUAL_COLOR_YELLOW);
        }
        snprintf(cell, sizeof(cell), "%d", day);
        const int digit_width = day >= 10 ? 36 : 18;
        const uint8_t digit_color = (today || kind == DAY_HOLIDAY || weekday >= 5)
                                         ? MANUAL_COLOR_RED : MANUAL_COLOR_BLACK;
        manual_canvas_text_utf8(canvas, cell_x + cell_width / 2 - digit_width / 2,
                                cell_y + 8, cell, digit_color, 1);
        if (kind == DAY_HOLIDAY || kind == DAY_WORK_SHIFT) {
            manual_canvas_text_utf8(canvas, cell_x + cell_width - 36,
                                    cell_y + cell_height - 33,
                                    kind == DAY_HOLIDAY ? "休" : "班",
                                    kind == DAY_HOLIDAY ? MANUAL_COLOR_RED :
                                                          MANUAL_COLOR_BLACK,
                                    1);
        }
    }
}

static void draw_calendar_landscape(const manual_canvas_t *canvas,
                                    const struct tm *date, bool with_photo)
{
    const int32_t margin = 16;
    const int32_t side_x = margin;
    const int32_t side_width = 242;
    const int32_t grid_x = side_x + side_width + 16;
    const int32_t grid_width = canvas->width - grid_x - margin;
    const int32_t cell_width = grid_width / 7;

    draw_frame(canvas, 4, 4, canvas->width - 8, canvas->height - 8,
               MANUAL_COLOR_BLACK);
    draw_frame(canvas, side_x, margin, side_width, canvas->height - margin * 2,
               MANUAL_COLOR_BLACK);

    char title[32];
    snprintf(title, sizeof(title), "%04d", date->tm_year + 1900);
    manual_canvas_text_utf8(canvas, side_x + 16, margin + 8, title,
                            MANUAL_COLOR_BLACK, 1);
    manual_canvas_text_utf8(canvas, side_x + 108, margin + 4,
                            MONTHS_CN[date->tm_mon], MANUAL_COLOR_RED, 1);
    char day[8];
    snprintf(day, sizeof(day), "%d", date->tm_mday);
    manual_canvas_date_digits(canvas, side_x + 16, margin + 50, day,
                              MANUAL_COLOR_RED);
    char weekday[32];
    snprintf(weekday, sizeof(weekday), "星期%s",
             WEEKDAYS_CN[weekday_index_monday_first(date)]);
    manual_canvas_text_utf8(canvas, side_x + 18, margin + 142, weekday,
                            MANUAL_COLOR_BLACK, 1);
    manual_canvas_text_utf8(canvas, side_x + 18, margin + 194,
                            date_kind_label(date), MANUAL_COLOR_RED, 1);

    draw_weather_card(canvas, side_x + 16, margin + 230, side_width - 32, 218);
    if (with_photo) {
        char photo[24];
        snprintf(photo, sizeof(photo), "相框 %u", current.photo_slot + 1u);
        manual_canvas_text_compact(canvas, side_x + 18, canvas->height - 66,
                                   photo, MANUAL_COLOR_RED);
    }
    draw_status_footer(canvas, side_x + 18, canvas->height - 44);

    manual_canvas_text_utf8(canvas, grid_x, margin + 4, "月历",
                            MANUAL_COLOR_BLACK, 1);
    draw_calendar_grid(canvas, date, grid_x, margin + 62, cell_width, 64);
    char plan[64];
    snprintf(plan, sizeof(plan), "今日计划:%s",
             current.today_plan[0] ? current.today_plan : "无");
    manual_canvas_text_medium(canvas, grid_x, canvas->height - 42, plan,
                              MANUAL_COLOR_BLACK);
}

static void draw_calendar_portrait(const manual_canvas_t *canvas,
                                   const struct tm *date, bool with_photo)
{
    const int32_t margin = 14;
    const int32_t top_right = canvas->width - 208;
    const int32_t grid_width = canvas->width - margin * 2;
    const int32_t cell_width = grid_width / 7;

    draw_frame(canvas, 4, 4, canvas->width - 8, canvas->height - 8,
               MANUAL_COLOR_BLACK);
    char title[32];
    snprintf(title, sizeof(title), "%04d", date->tm_year + 1900);
    manual_canvas_text_utf8(canvas, margin, 18, title, MANUAL_COLOR_BLACK, 1);
    manual_canvas_text_utf8(canvas, margin + 108, 14, MONTHS_CN[date->tm_mon],
                            MANUAL_COLOR_RED, 1);
    char day[8];
    snprintf(day, sizeof(day), "%d", date->tm_mday);
    manual_canvas_date_digits(canvas, margin, 58, day, MANUAL_COLOR_RED);
    char weekday[32];
    snprintf(weekday, sizeof(weekday), "星期%s",
             WEEKDAYS_CN[weekday_index_monday_first(date)]);
    manual_canvas_text_utf8(canvas, margin, 142, weekday, MANUAL_COLOR_BLACK, 1);
    manual_canvas_text_utf8(canvas, margin, 194, date_kind_label(date),
                            MANUAL_COLOR_RED, 1);

    draw_weather_card(canvas, top_right, 16, 194, 208);
    draw_calendar_grid(canvas, date, margin, 282, cell_width, 64);
    char plan[64];
    snprintf(plan, sizeof(plan), "今日计划:%s",
             current.today_plan[0] ? current.today_plan : "无");
    manual_canvas_text_medium(canvas, margin, canvas->height - 72, plan,
                              MANUAL_COLOR_BLACK);
    if (with_photo) {
        char photo[24];
        snprintf(photo, sizeof(photo), "相框 %u", current.photo_slot + 1u);
        manual_canvas_text_compact(canvas, top_right, 232, photo,
                                   MANUAL_COLOR_RED);
    }
    draw_status_footer(canvas, margin, canvas->height - 44);
}

static void draw_status_page(const manual_canvas_t *canvas)
{
    const int32_t margin = canvas->width < 600 ? 18 : 28;
    draw_frame(canvas, 4, 4, canvas->width - 8, canvas->height - 8,
               MANUAL_COLOR_BLACK);
    manual_canvas_text_utf8(canvas, margin, margin + 12, "状态",
                            MANUAL_COLOR_BLACK, 1);
    manual_canvas_text_utf8(canvas, margin, margin + 54, "网络", MANUAL_COLOR_BLACK, 1);
    manual_canvas_text_utf8(canvas, margin + 180, margin + 54,
                            current.wifi_connected ? "已连接" : "未连接",
                            current.wifi_connected ? MANUAL_COLOR_BLACK :
                                                     MANUAL_COLOR_RED,
                            1);
    manual_canvas_text_utf8(canvas, margin, margin + 88, "时间同步",
                            MANUAL_COLOR_BLACK, 1);
    manual_canvas_text_utf8(canvas, margin + 180, margin + 88,
                            current.ntp_synced ? "已同步" : "等待校时",
                            current.ntp_synced ? MANUAL_COLOR_BLACK :
                                                 MANUAL_COLOR_YELLOW,
                            1);
    manual_canvas_text_utf8(canvas, margin, margin + 122, "主页",
                            MANUAL_COLOR_BLACK, 1);
    manual_canvas_text_utf8(canvas, margin + 180, margin + 122,
                            mode_name(current.mode), MANUAL_COLOR_RED, 1);
    manual_canvas_text_utf8(canvas, margin, margin + 156, "时区",
                            MANUAL_COLOR_BLACK, 1);
    manual_canvas_text_utf8(canvas, margin + 180, margin + 156,
                            current.timezone[0] ? current.timezone : "--",
                            MANUAL_COLOR_BLACK, 1);
    draw_status_footer(canvas, margin, canvas->height - 44);
}

static void draw_photo_page(const manual_canvas_t *canvas)
{
    static uint8_t row[PHOTO_STORE_WIDTH / 4u];
    photo_store_info_t info = {0};
    size_t image_length = 0;
    const esp_err_t state = photo_store_get(current.photo_slot, &info, NULL, 0,
                                            &image_length);
    const bool valid = state == ESP_ERR_INVALID_SIZE && info.valid &&
                       image_length == PHOTO_STORE_BYTES;
    if (valid) {
        for (uint32_t y = 0; y < PHOTO_STORE_HEIGHT; ++y) {
            if (photo_store_read(current.photo_slot,
                                 (size_t)y * sizeof(row), row,
                                 sizeof(row)) != ESP_OK) {
                break;
            }
            for (uint32_t x = 0; x < PHOTO_STORE_WIDTH; ++x) {
                display_point_t logical;
                if (!display_rotation_unmap_point(
                        canvas->rotation, DISPLAY_EPD_WIDTH, DISPLAY_EPD_HEIGHT,
                        (int32_t)x, (int32_t)y, &logical)) {
                    continue;
                }
                const uint8_t packed = row[x >> 2];
                const uint8_t color = (packed >> (6 - 2 * (x & 3))) & 0x03u;
                manual_canvas_pixel(canvas, logical.x, logical.y, color);
            }
        }
        draw_frame(canvas, 4, 4, canvas->width - 8, canvas->height - 8,
                   MANUAL_COLOR_BLACK);
        char label[24];
        snprintf(label, sizeof(label), "相框 %u", current.photo_slot + 1u);
        manual_canvas_text_utf8(canvas, 16, 16, label, MANUAL_COLOR_RED, 1);
    } else {
        draw_frame(canvas, 12, 12, canvas->width - 24, canvas->height - 24,
                   MANUAL_COLOR_BLACK);
        manual_canvas_text_utf8(canvas, 28, 38, "相框", MANUAL_COLOR_BLACK, 1);
        char label[24];
        snprintf(label, sizeof(label), "槽位 %u 空", current.photo_slot + 1u);
        manual_canvas_text_utf8(canvas, 28, 82, label, MANUAL_COLOR_RED, 1);
        manual_canvas_text_utf8(canvas, 28, 124, "上传 2bpp 图片", MANUAL_COLOR_BLACK, 1);
    }
}

void ui_app_init(void)
{
    memset(&current, 0, sizeof(current));
    current.mode = UI_MODE_CALENDAR;
}

void ui_app_set_state(const ui_app_state_t *state)
{
    if (state) current = *state;
}

ui_mode_t ui_app_mode(void)
{
    return current.mode;
}

int ui_app_render(void)
{
    manual_canvas_t canvas;
    const display_rotation_t rotation =
        (display_rotation_t)((current.rotation & 0x03u) * 90u);
    if (!manual_canvas_init(&canvas, rotation)) return -1;

    manual_canvas_clear(&canvas, MANUAL_COLOR_WHITE);
    time_t timestamp = time(NULL);
    struct tm local = {0};
    const bool time_valid = timestamp >= 1000000000 &&
                            localtime_r(&timestamp, &local) != NULL;
    if (!time_valid) use_build_date(&local);

    if (current.mode == UI_MODE_STATUS) {
        draw_status_page(&canvas);
    } else if (current.mode == UI_MODE_PHOTO) {
        draw_photo_page(&canvas);
    } else if (canvas.width >= 600) {
        draw_calendar_landscape(&canvas, &local,
                                current.mode == UI_MODE_CALENDAR_PHOTO);
    } else {
        draw_calendar_portrait(&canvas, &local,
                               current.mode == UI_MODE_CALENDAR_PHOTO);
    }
    return 0;
}
