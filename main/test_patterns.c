/*
 * Bring-up test pages.
 *
 * The point of this file is to turn "the panel does not work" into a specific
 * answer: which colours are wrong, which axis is flipped, whether the bus is
 * delivering bytes at all.
 */
#include "test_patterns.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ref_image.h"

static const char *TAG = "test";

static void label(int x, int y, const char *s, uint8_t color, int scale)
{
    for (; *s; s++) {
        epd_fb_char(x, y, *s, color, scale);
        x += 6 * scale;
    }
}

static void draw_swatch(int x, int y, int w, int h, uint8_t color,
                        const char *name, uint8_t ink)
{
    epd_fb_rect_fill(x, y, w, h, color);
    label(x + 10, y + 10, name, ink, 3);
}

/* Each test page draws through a per-strip callback: the renderer fills the
 * active strip in native coordinates (clipped), then the panel streams it. */
typedef void (*page_draw_arg_fn)(void *arg);

typedef struct {
    page_draw_arg_fn fn;
    void *arg;
} page_call_t;

static esp_err_t page_strip_render(void *ctx, int y0)
{
    (void)y0;
    page_call_t *call = (page_call_t *)ctx;
    call->fn(call->arg);
    return ESP_OK;
}

static esp_err_t display_page_arg(page_draw_arg_fn fn, void *arg)
{
    static page_call_t call;
    call.fn = fn;
    call.arg = arg;
    return epd_display(page_strip_render, &call);
}

/* ------------------------------------------------------------------ pages -- */

static void draw_solid(void *arg)
{
    epd_fb_fill((uint8_t)(uintptr_t)arg);
}

esp_err_t test_pattern_solid(uint8_t color)
{
    /* epd_fb_fill 只清当前条带，逐条带重跑即可填满整帧。 */
    return display_page_arg(draw_solid, (void *)(uintptr_t)color);
}

/*
 * The diagnostic page. Reading it answers every orientation question at once:
 *   - the four colour bars name themselves, so a wrong colour code is visible
 *   - the 2 px border is red on all sides
 *   - the "TL" corner marker is red, so a mirrored axis is visible
 *   - the 48 px grid makes stretching or dropped lines obvious
 */
static void draw_info(void *arg)
{
    (void)arg;
    const int wide = EPD_WIDTH / 4; /* 192 */

    epd_fb_fill(EPD_COLOR_WHITE);

    /* Four full-height colour bars, left to right. */
    draw_swatch(0 * wide, 0, wide, 400, EPD_COLOR_WHITE, "WHITE", EPD_COLOR_BLACK);
    draw_swatch(1 * wide, 0, wide, 400, EPD_COLOR_BLACK, "BLACK", EPD_COLOR_WHITE);
    draw_swatch(2 * wide, 0, wide, 400, EPD_COLOR_YELLOW, "YELLOW", EPD_COLOR_BLACK);
    draw_swatch(3 * wide, 0, wide, 400, EPD_COLOR_RED, "RED", EPD_COLOR_WHITE);

    /* Title strip. */
    epd_fb_rect_fill(0, 400, EPD_WIDTH, 40, EPD_COLOR_WHITE);
    label(8, 408, "ESP32-C3 JD79665 768X552 4COLOR", EPD_COLOR_BLACK, 3);

    /* Grid: 48 px minor, 192 px major, drawn so misalignment is measurable. */
    for (int x = 0; x < EPD_WIDTH; x += 48) {
        uint8_t c = (x % 192 == 0) ? EPD_COLOR_BLACK : EPD_COLOR_YELLOW;
        epd_fb_vline(x, 0, 399, c);
    }
    for (int y = 0; y < 400; y += 48) {
        uint8_t c = (y % 192 == 0) ? EPD_COLOR_BLACK : EPD_COLOR_YELLOW;
        epd_fb_hline(0, EPD_WIDTH - 1, y, c);
    }

    /* Orientation markers: TL red, TR yellow, BL yellow, BR red. */
    epd_fb_rect_fill(0, 0, 30, 30, EPD_COLOR_RED);
    epd_fb_rect_fill(EPD_WIDTH - 30, 0, 30, 30, EPD_COLOR_YELLOW);
    epd_fb_rect_fill(0, 370, 30, 30, EPD_COLOR_YELLOW);
    epd_fb_rect_fill(EPD_WIDTH - 30, 370, 30, 30, EPD_COLOR_RED);
    label(4, 34, "TL", EPD_COLOR_RED, 2);
    label(EPD_WIDTH - 34, 34, "TR", EPD_COLOR_YELLOW, 2);

    /* Bottom: colour-code reference and 1 px detail, which a dead bus cannot
     * fake because it needs real per-pixel data. */
    epd_fb_rect_fill(0, 440, EPD_WIDTH, EPD_HEIGHT - 440, EPD_COLOR_WHITE);
    label(8, 448, "CODE 00=BLACK 01=WHITE 10=YELLOW 11=RED", EPD_COLOR_BLACK, 2);
    label(8, 470, "1PX LINES BELOW MEAN THE BUS DELIVERS REAL DATA",
          EPD_COLOR_RED, 2);
    for (int y = 492; y < 540; y++) {
        if ((y & 1) == 0) {
            epd_fb_hline(8, EPD_WIDTH - 9, y, EPD_COLOR_BLACK);
        }
    }
    epd_fb_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_COLOR_RED);
    epd_fb_rect(1, 1, EPD_WIDTH - 2, EPD_HEIGHT - 2, EPD_COLOR_RED);
}

esp_err_t test_pattern_info(void)
{
    return display_page_arg(draw_info, NULL);
}

/* A high-contrast checkerboard: every wrong shift, mirror or line drop shows. */
static void draw_checker(void *arg)
{
    const int cell = (int)(intptr_t)arg;
    for (int y = 0; y < EPD_HEIGHT; y++) {
        for (int x = 0; x < EPD_WIDTH; x++) {
            int on = ((x / cell) + (y / cell)) & 1;
            epd_fb_set_pixel(x, y, on ? EPD_COLOR_BLACK : EPD_COLOR_WHITE);
        }
    }
    label(8, 8, "CHECKER", EPD_COLOR_RED, 2);
}

esp_err_t test_pattern_checker(int cell)
{
    if (cell < 1) {
        cell = 8;
    }
    return display_page_arg(draw_checker, (void *)(intptr_t)cell);
}

/*
 * Control experiment: the vendor's own image for this panel, byte for byte.
 * Same board, same driver, same refresh path - only the content differs. If
 * this shows the same frozen band, the defect is not in our rendering; if it
 * is clean, the defect is content dependent.
 */
static esp_err_t ref_image_strip_render(void *ctx, int y0)
{
    (void)ctx;
    int rows = EPD_STRIP_ROWS;
    if (y0 + rows > EPD_HEIGHT) {
        rows = EPD_HEIGHT - y0;   /* 最后一条带可能不足 16 行 */
    }
    if (rows > 0) {
        epd_strip_copy(ref_image + (size_t)y0 * EPD_BYTES_PER_LINE,
                       EPD_BYTES_PER_LINE, rows);
    }
    return ESP_OK;
}

esp_err_t test_pattern_ref_image(void)
{
    return epd_display(ref_image_strip_render, NULL);
}

/* Four horizontal bands: isolates a vertical (line-order) fault from a
 * horizontal (byte-order) one. */
static void draw_bands_h(void *arg)
{
    (void)arg;
    const int band = EPD_HEIGHT / 4;
    static const uint8_t colors[4] = {EPD_COLOR_BLACK, EPD_COLOR_WHITE,
                                      EPD_COLOR_YELLOW, EPD_COLOR_RED};
    static const char *names[4] = {"BLACK", "WHITE", "YELLOW", "RED"};
    for (int i = 0; i < 4; i++) {
        epd_fb_rect_fill(0, i * band, EPD_WIDTH, band, colors[i]);
        uint8_t ink = (colors[i] == EPD_COLOR_WHITE || colors[i] == EPD_COLOR_YELLOW)
                          ? EPD_COLOR_BLACK
                          : EPD_COLOR_WHITE;
        label(350, i * band + band / 2 - 10, names[i], ink, 3);
    }
}

esp_err_t test_pattern_bands_h(void)
{
    return display_page_arg(draw_bands_h, NULL);
}

/*
 * Calibration page.
 *
 * Three solid squares, each in its own colour and far from anything else of
 * that colour, so the photo-to-panel homography can be solved by machine:
 *   red square top-left, yellow square top-right, black square bottom-left.
 * The rest of the page is plain white with a thin black frame, which also
 * makes the panel outline measurable.
 */
#define CAL_MARK 120
#define CAL_INSET 44

static void draw_corners(void *arg)
{
    (void)arg;
    epd_fb_fill(EPD_COLOR_WHITE);

    epd_fb_rect_fill(CAL_INSET, CAL_INSET, CAL_MARK, CAL_MARK,
                     EPD_COLOR_RED);                                   /* TL */
    epd_fb_rect_fill(EPD_WIDTH - CAL_INSET - CAL_MARK, CAL_INSET,
                     CAL_MARK, CAL_MARK, EPD_COLOR_YELLOW);            /* TR */
    epd_fb_rect_fill(CAL_INSET, EPD_HEIGHT - CAL_INSET - CAL_MARK,
                     CAL_MARK, CAL_MARK, EPD_COLOR_BLACK);             /* BL */

    /* Panel outline, 4 px. */
    epd_fb_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_COLOR_BLACK);
    epd_fb_rect(1, 1, EPD_WIDTH - 2, EPD_HEIGHT - 2, EPD_COLOR_BLACK);
    epd_fb_rect(2, 2, EPD_WIDTH - 4, EPD_HEIGHT - 4, EPD_COLOR_BLACK);
    epd_fb_rect(3, 3, EPD_WIDTH - 6, EPD_HEIGHT - 6, EPD_COLOR_BLACK);

    /* Nothing else on the page: no other red, yellow or black pixels. */
}

esp_err_t test_pattern_corners(void)
{
    return display_page_arg(draw_corners, NULL);
}

/* Marker centres, exposed for the analysis script. */
void test_calibration_markers(int *tl_x, int *tl_y, int *tr_x, int *tr_y,
                              int *bl_x, int *bl_y)
{
    int c = CAL_INSET + CAL_MARK / 2;
    *tl_x = c;
    *tl_y = c;
    *tr_x = EPD_WIDTH - c;
    *tr_y = c;
    *bl_x = c;
    *bl_y = EPD_HEIGHT - c;
}

/*
 * The sharpest test page: 24 px black/white stripes along x with a 3 px yellow
 * index every 96 px. A region that a full refresh fails to reach shows up as a
 * stripe that is neither black, white nor yellow, and the yellow indices pin
 * down its position to within a few pixels.
 */
static void draw_stripes_x(void *arg)
{
    (void)arg;
    epd_fb_fill(EPD_COLOR_WHITE);
    for (int x = 0; x < EPD_WIDTH; x += 48) {
        epd_fb_rect_fill(x, 0, 24, EPD_HEIGHT, EPD_COLOR_BLACK);
    }
    for (int x = 0; x < EPD_WIDTH; x += 96) {
        epd_fb_rect_fill(x, 0, 3, EPD_HEIGHT, EPD_COLOR_YELLOW);
    }
    label(8, 8, "STRIPES X", EPD_COLOR_RED, 3);
}

esp_err_t test_pattern_stripes_x(void)
{
    return display_page_arg(draw_stripes_x, NULL);
}

/*
 * Two-axis probe.
 *
 * Both pages paint a WHITE base and then overdraw stripes. Because the base is
 * white and the stripes are black, any region that is neither is stale content
 * that the frame failed to reach:
 *   - axis_probe_h: stripes vary along x, so leftover garbage means the
 *     failure is at a fixed byte offset inside every display line
 *   - axis_probe_v: stripes vary along y, so leftover garbage means whole
 *     display lines are never written
 * Running both localises the fault to one axis without any assumption about
 * how the panel is rotated on the bench.
 */
static void draw_probe_h(void *arg)
{
    (void)arg;
    epd_fb_fill(EPD_COLOR_WHITE);
    for (int x = 0; x < EPD_WIDTH; x += 128) {
        epd_fb_rect_fill(x, 0, 64, EPD_HEIGHT, EPD_COLOR_BLACK);
    }
    /* Yellow landmarks mark the exact stripe boundaries. */
    for (int x = 0; x < EPD_WIDTH; x += 128) {
        epd_fb_vline(x, 0, EPD_HEIGHT - 1, EPD_COLOR_YELLOW);
        epd_fb_vline(x + 63, 0, EPD_HEIGHT - 1, EPD_COLOR_YELLOW);
    }
    label(8, EPD_HEIGHT / 2, "PROBE H", EPD_COLOR_RED, 3);
}

esp_err_t test_pattern_axis_probe_h(void)
{
    return display_page_arg(draw_probe_h, NULL);
}

static void draw_probe_v(void *arg)
{
    (void)arg;
    epd_fb_fill(EPD_COLOR_WHITE);
    for (int y = 0; y < EPD_HEIGHT; y += 128) {
        epd_fb_rect_fill(0, y, EPD_WIDTH, 64, EPD_COLOR_BLACK);
    }
    for (int y = 0; y < EPD_HEIGHT; y += 128) {
        epd_fb_hline(0, EPD_WIDTH - 1, y, EPD_COLOR_YELLOW);
        epd_fb_hline(0, EPD_WIDTH - 1, y + 63, EPD_COLOR_YELLOW);
    }
    label(8, EPD_HEIGHT / 2, "PROBE V", EPD_COLOR_RED, 3);
}

esp_err_t test_pattern_axis_probe_v(void)
{
    return display_page_arg(draw_probe_v, NULL);
}

/*
 * Ruler. A dropped or misplaced block of pixel data is immediately visible as
 * a stripe that changes colour where it should not:
 *   - 1 px black separator between stripes fixes the exact boundary
 *   - "R<n>" marks the first row of every fourth stripe
 *   - the row number is also printed in the middle and at the end of the line
 *     so a partially written line is still identifiable
 *   - a solid red band sits at rows 300..330 as a known landmark
 */
static void draw_stripes(void *arg)
{
    (void)arg;
    epd_fb_fill(EPD_COLOR_BLACK);
    for (int y = 0; y < EPD_HEIGHT; y += 24) {
        epd_fb_rect_fill(0, y, EPD_WIDTH, 23, EPD_COLOR_WHITE);
        if (y % 96 == 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "R%d", y);
            label(6, y + 8, buf, EPD_COLOR_BLACK, 2);
            label(EPD_WIDTH / 2, y + 8, buf, EPD_COLOR_BLACK, 2);
            label(EPD_WIDTH - 70, y + 8, buf, EPD_COLOR_BLACK, 2);
        }
    }
    label(6, 130, "STRIPE 24PX", EPD_COLOR_RED, 3);
    epd_fb_rect_fill(0, 300, EPD_WIDTH, 31, EPD_COLOR_RED);
}

esp_err_t test_pattern_stripes(void)
{
    return display_page_arg(draw_stripes, NULL);
}

/* Same ruler rotated for alignment checks along x. */
static void draw_stripes_v(void *arg)
{
    (void)arg;
    epd_fb_fill(EPD_COLOR_BLACK);
    for (int x = 0; x < EPD_WIDTH; x += 24) {
        epd_fb_rect_fill(x, 0, 23, EPD_HEIGHT, EPD_COLOR_WHITE);
    }
    for (int x = 0; x < EPD_WIDTH; x += 96) {
        epd_fb_rect_fill(x, 0, 1, EPD_HEIGHT, EPD_COLOR_RED);
    }
    epd_fb_rect_fill(EPD_WIDTH / 2 - 1, 0, 3, EPD_HEIGHT, EPD_COLOR_YELLOW);
}

esp_err_t test_pattern_stripes_v(void)
{
    return display_page_arg(draw_stripes_v, NULL);
}

/* Alternate-page variant: same colour bars as the info page but with the
 * bottom half replaced, so a stale ROM image cannot be mistaken for output. */
static void draw_info_b(void *arg)
{
    (void)arg;
    draw_info(NULL);
    /* Repaint only the lower half with a distinguishable pattern. */
    for (int y = EPD_HEIGHT / 2; y < EPD_HEIGHT; y += 16) {
        epd_fb_rect_fill(0, y, EPD_WIDTH, 8, EPD_COLOR_YELLOW);
    }
    label(8, EPD_HEIGHT / 2 + 20, "PAGE B HALF", EPD_COLOR_BLACK, 3);
}

esp_err_t test_pattern_info_b(void)
{
    return display_page_arg(draw_info_b, NULL);
}

/*
 * Band probe.
 *
 * The bench photo shows a narrow band of the panel that is identical in every
 * frame. Two explanations fit that photo and nothing in the driver can tell
 * them apart:
 *
 *   1. a physical object (tape, lifted front film, flex) lies on the panel's
 *      front surface, so the band simply is not the panel we are driving
 *   2. a band of gate lines is never driven, so those pixels keep whatever
 *      state they had when the panel left the factory
 *
 * Two pages separate them:
 *
 *   A: a white page whose rows 96..336 carry 48 px black/white blocks, i.e.
 *      maximum-contrast content exactly inside the suspect band
 *   B: the same page with the whole panel blank white
 *
 * Then watch the band:
 *   - blocks still visible on page B  -> the panel is really holding page A's
 *     content there, so those gate lines never switch: a panel defect
 *   - band blank on page B, and the blocks faintly tinted through it on page A
 *     -> the panel is being written correctly and the object is in front of it
 *
 * Both pages keep a black frame so the panel outline stays measurable in a
 * photo even when the page is white.
 */
#define PROBE_ROW_FIRST  96
#define PROBE_ROW_LAST   336
#define PROBE_BLOCK      48

static void draw_probe_a(void *arg)
{
    (void)arg;
    epd_fb_fill(EPD_COLOR_WHITE);

    for (int x = 0; x < EPD_WIDTH; x += 2 * PROBE_BLOCK) {
        epd_fb_rect_fill(x, PROBE_ROW_FIRST, PROBE_BLOCK,
                         PROBE_ROW_LAST - PROBE_ROW_FIRST, EPD_COLOR_BLACK);
    }

    label(8, 8, "PROBE A BLOCKS IN ROWS 96 TO 336", EPD_COLOR_BLACK, 2);
    label(8, 26, "NOW PRESS UPPER CASE B", EPD_COLOR_RED, 2);
    label(8, 44, "A BLOCK LEFT IN THE BAND MEANS DEAD GATES",
          EPD_COLOR_BLACK, 2);

    epd_fb_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_COLOR_BLACK);
    epd_fb_rect(1, 1, EPD_WIDTH - 2, EPD_HEIGHT - 2, EPD_COLOR_BLACK);
}

esp_err_t test_pattern_probe_a(void)
{
    return display_page_arg(draw_probe_a, NULL);
}

static void draw_probe_b(void *arg)
{
    (void)arg;
    epd_fb_fill(EPD_COLOR_WHITE);

    label(8, 8, "PROBE B EMPTY WHITE PAGE", EPD_COLOR_BLACK, 2);
    label(8, 26, "ANYTHING VISIBLE IN THE BAND IS STALE",
          EPD_COLOR_RED, 2);

    epd_fb_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, EPD_COLOR_BLACK);
    epd_fb_rect(1, 1, EPD_WIDTH - 2, EPD_HEIGHT - 2, EPD_COLOR_BLACK);
}

esp_err_t test_pattern_probe_b(void)
{
    return display_page_arg(draw_probe_b, NULL);
}

/* Alternate the two probe pages so the stale-content question is answered
 * without a serial console in reach. */
void test_probe_sequence(unsigned dwell_ms)
{
    ESP_LOGI(TAG, "probe A: black blocks in rows %d..%d, watch the band",
             PROBE_ROW_FIRST, PROBE_ROW_LAST);
    test_pattern_probe_a();
    vTaskDelay(pdMS_TO_TICKS(dwell_ms));
    ESP_LOGI(TAG, "probe B: blank white page; the band must be blank too");
    test_pattern_probe_b();
}

/* -------------------------------------------------------------- diagnostics - */

/* Sampling the framebuffer tells us whether we are at least sending the bytes
 * we think we are, independently of what the panel shows. */
void test_dump_fb(void)
{
    const epd_diag_t *d = epd_diag();
    ESP_LOGI(TAG, "framebuffer: strip-based (%d rows x %d bytes), no full-frame buffer",
             (int)EPD_STRIP_ROWS, EPD_BYTES_PER_LINE);
    ESP_LOGI(TAG, "last frame bytes sent: %llu", (unsigned long long)d->bytes_sent);
}

void test_report(void)
{
    const epd_diag_t *d = epd_diag();
    ESP_LOGI(TAG, "---- diagnostics ----");
    ESP_LOGI(TAG, "last stage      : %s", epd_stage_name(d->last_stage));
    ESP_LOGI(TAG, "last error      : %s", esp_err_to_name(d->last_err));
    ESP_LOGI(TAG, "BUSY at boot    : %d", d->busy_level_at_boot);
    ESP_LOGI(TAG, "BUSY now        : %d", epd_busy_level());
    ESP_LOGI(TAG, "frames written  : %d", d->frame_count);
    ESP_LOGI(TAG, "refreshes ok    : %d", d->refresh_count);
    ESP_LOGI(TAG, "BUSY timeouts   : %d", d->busy_timeouts);
    ESP_LOGI(TAG, "last BUSY wait  : %lld us (%d polls)",
             (long long)d->last_busy_wait_us, d->busy_loop_count);
    ESP_LOGI(TAG, "framebuffer     : %d bytes logical %dx%d; wire frame %d bytes %dx%d",
             EPD_FRAME_BYTES, EPD_WIDTH, EPD_HEIGHT,
             EPD_PHYS_FRAME_BYTES, EPD_PHYS_WIDTH, EPD_PHYS_HEIGHT);
    ESP_LOGI(TAG, "data framing    : %s", epd_xfer_mode_name(epd_xfer_mode()));
    ESP_LOGI(TAG, "frame strategy  : %s",
             epd_frame_mode_name(epd_frame_mode()));
    ESP_LOGI(TAG, "row mapping     : %s", epd_rowmap_name(epd_rowmap()));
    ESP_LOGI(TAG, "wire layout     : %s",
             epd_column_major() ? "column-major" : "row-major");
    ESP_LOGI(TAG, "init table      : %s",
             epd_init_variant() ? "B (minimal)" : "A (reference)");
    ESP_LOGI(TAG, "PSR direction   : %s",
             epd_psr_normal() ? "SHL=1 UD=1" : "SHL=0 UD=0");
    ESP_LOGI(TAG, "SPI clock       : %" PRIu32 " Hz", epd_clock_hz());
    ESP_LOGI(TAG, "frame x offset  : %d px", epd_x_offset());
    ESP_LOGI(TAG, "physical frame  : %s",
             epd_physical_frame() ? "800x600 padded" : "768x552 tight");
    ESP_LOGI(TAG, "R11H data stop  : %s", epd_data_stop_enabled() ? "on" : "off");
    ESP_LOGI(TAG, "last Data_flag  : %d (raw 0x%02x)", d->last_data_flag,
             d->last_dsp_value);
    ESP_LOGI(TAG, "0x83 window     : %s",
             epd_partial_window_mode() ? "set before each frame" : "not set");
    ESP_LOGI(TAG, "pixel bytes out : %llu", (unsigned long long)d->bytes_sent);
    ESP_LOGI(TAG, "--------------------");
}

/* ----------------------------------------------------------------- driver -- */

esp_err_t test_run(const char *cmd)
{
    if (!cmd || !*cmd) {
        return ESP_ERR_INVALID_ARG;
    }
    switch (cmd[0]) {
    case '1': ESP_LOGI(TAG, "solid WHITE");  return test_pattern_solid(EPD_COLOR_WHITE);
    case '2': ESP_LOGI(TAG, "solid BLACK");  return test_pattern_solid(EPD_COLOR_BLACK);
    case '3': ESP_LOGI(TAG, "solid YELLOW"); return test_pattern_solid(EPD_COLOR_YELLOW);
    case '4': ESP_LOGI(TAG, "solid RED");    return test_pattern_solid(EPD_COLOR_RED);
    case '5': ESP_LOGI(TAG, "info page");    return test_pattern_info();
    case '6': ESP_LOGI(TAG, "horizontal bands"); return test_pattern_bands_h();
    case '7': ESP_LOGI(TAG, "checker 8px");  return test_pattern_checker(8);
    case '8': ESP_LOGI(TAG, "stripe ruler 24px"); return test_pattern_stripes();
    case '9': ESP_LOGI(TAG, "vertical stripe ruler"); return test_pattern_stripes_v();
    case '0': ESP_LOGI(TAG, "alternate info page"); return test_pattern_info_b();
    case 't': ESP_LOGI(TAG, "x stripes with index"); return test_pattern_stripes_x();
    case 'c': ESP_LOGI(TAG, "calibration corners"); return test_pattern_corners();
    case 'i': ESP_LOGI(TAG, "vendor reference image"); return test_pattern_ref_image();
    case 'z': ESP_LOGI(TAG, "axis probe along x"); return test_pattern_axis_probe_h();
    case 'y': ESP_LOGI(TAG, "axis probe along y"); return test_pattern_axis_probe_v();
    case 'A': ESP_LOGI(TAG, "band probe A (blocks)"); return test_pattern_probe_a();
    case 'B': ESP_LOGI(TAG, "band probe B (blank)"); return test_pattern_probe_b();
    default:
        ESP_LOGW(TAG, "unknown test '%c'", cmd[0]);
        return ESP_ERR_INVALID_ARG;
    }
}

/* ------------------------------------------------------------ console repl - */

/*
 * A whole token is routed here, not one character at a time, so that
 * multi-letter commands such as "clock 1000000" can coexist with the single
 * character test selectors.
 */
bool test_console_command(const char *cmd, size_t len)
{
    if (len == 1) {
        switch (cmd[0]) {
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7':
        case '8': case '9': case '0':
        case 'z': case 'y': case 't': case 'c': case 'i':
        case 'A': case 'B':
            test_run(cmd);
            return true;
        case 'd': test_report();  return true;
        case 'j': test_dump_fb(); return true;
        case 's':
            ESP_LOGI(TAG, "deep sleep");
            epd_power_off();
            epd_sleep();
            return true;
        case 'f': {
            epd_frame_mode_t m = (epd_frame_mode_t)((epd_frame_mode() + 1) % 2);
            epd_set_frame_mode(m);
            return true;
        }
        case 'x': {
            epd_xfer_mode_t m = (epd_xfer_mode_t)((epd_xfer_mode() + 1) % 4);
            epd_set_xfer_mode(m);
            return true;
        }
        case 'w':
            epd_set_partial_window_mode(!epd_partial_window_mode());
            return true;
        case 'p':
            epd_set_physical_frame(!epd_physical_frame());
            return true;
        case 'q':
            epd_set_data_stop(!epd_data_stop_enabled());
            return true;
        case 'r': {
            epd_rowmap_t m = (epd_rowmap_t)((epd_rowmap() + 1) % 3);
            epd_set_rowmap(m);
            return true;
        }
        case 'g':
            /* Toggle scan direction and re-program the registers. */
            epd_set_psr_normal(!epd_psr_normal());
            epd_init();
            return true;
        case 'b':
            epd_set_bit_order(!epd_bit_order_reversed());
            return true;
        case 'n':
            /* Switch init table and re-program the registers. */
            epd_set_init_variant(!epd_init_variant());
            epd_init();
            return true;
        case 'm':
            epd_set_column_major(!epd_column_major());
            return true;
        case 'l':
            epd_set_interleaved(!epd_interleaved());
            return true;
        default:
            return false;
        }
    }

    if (strncmp(cmd, "offset ", 7) == 0) {
        long px = strtol(cmd + 7, NULL, 10);
        if (px >= 0 && px <= EPD_WIDTH) {
            epd_set_x_offset((int)px);
        } else {
            ESP_LOGW(TAG, "offset must be 0..%d", EPD_WIDTH);
        }
        return true;
    }
    if (strncmp(cmd, "clock ", 6) == 0) {
        long hz = strtol(cmd + 6, NULL, 10);
        if (hz >= 100000 && hz <= 40000000) {
            epd_set_clock_hz((uint32_t)hz);
        } else {
            ESP_LOGW(TAG, "clock out of range: %ld", hz);
        }
        return true;
    }
    if (strncmp(cmd, "mode ", 5) == 0) {
        if (strcmp(cmd + 5, "sweep") == 0) {
            epd_set_frame_mode(EPD_FRAME_SWEEP);
        } else if (strcmp(cmd + 5, "line") == 0) {
            epd_set_frame_mode(EPD_FRAME_PER_LINE);
        } else {
            ESP_LOGW(TAG, "mode is sweep or line");
        }
        return true;
    }
    return false;
}
