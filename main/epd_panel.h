/*
 * Low level JD79665 4-colour panel driver plus a 2bpp framebuffer.
 */
#ifndef EPD_PANEL_H
#define EPD_PANEL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>

#include "epd_config.h"
#include "esp_err.h"

/* ---------------------------------------------------------------- stages --- */
/* Every bring-up stage is observable over the serial console so that a silent
 * panel can be localised to wiring, bus, controller or RAM. */
typedef enum {
    EPD_STAGE_IDLE = 0,
    EPD_STAGE_BUS_UP,
    EPD_STAGE_RESET,
    EPD_STAGE_BUSY_RELEASE,
    EPD_STAGE_REGISTERS,
    EPD_STAGE_POWER_ON,
    EPD_STAGE_DATA,
    EPD_STAGE_REFRESH,
    EPD_STAGE_POWER_OFF,
    EPD_STAGE_SLEEP,
} epd_stage_t;

/* ------------------------------------------------------------ diagnostics -- */
typedef struct {
    epd_stage_t last_stage;        /* stage reached before the last failure */
    esp_err_t   last_err;          /* ESP_OK when the last operation succeeded */
    int         busy_level_at_boot;/* BUSY sampled before any SPI activity */
    int         busy_loop_count;   /* poll iterations during the last wait */
    int64_t     last_busy_wait_us; /* duration of the last BUSY wait */
    int         refresh_count;     /* successful DISPLAY_REFRESH operations */
    int         busy_timeouts;     /* BUSY waits that hit the timeout */
    int         frame_count;       /* frames pushed into panel RAM */
    uint64_t    bytes_sent;        /* pixel bytes clocked out of the SPI bus */
    int         last_data_flag;    /* Data_flag from the last DSP command */
    int         last_dsp_value;    /* raw byte read back by DSP */
} epd_diag_t;

/* How pixel bytes are framed on the wire. The reference driver asserts CS for
 * every byte; streaming under a single CS assertion is much faster but must be
 * proven on this panel. */
typedef enum {
    EPD_XFER_CHUNK = 0,   /* 8 KiB per CS assertion, fastest */
    EPD_XFER_LINE,        /* one CS assertion per 192 byte line */
    EPD_XFER_BYTE,        /* one CS assertion per byte, as the reference does */
    EPD_XFER_CONTINUOUS,  /* CS held low across payload transactions */
} epd_xfer_mode_t;

/* How a whole frame reaches panel RAM. The reference drivers disagree: the
 * BLE demo streams one big block, the production jd79665 driver re-programs
 * the RAM window for every single display line. */
typedef enum {
    EPD_FRAME_SWEEP = 0,  /* stream the whole frame after one window setup */
    EPD_FRAME_PER_LINE,   /* set a one-row window, then that row's 192 bytes */
} epd_frame_mode_t;

void epd_set_xfer_mode(epd_xfer_mode_t mode);
epd_xfer_mode_t epd_xfer_mode(void);
const char *epd_xfer_mode_name(epd_xfer_mode_t mode);

void epd_set_frame_mode(epd_frame_mode_t mode);
epd_frame_mode_t epd_frame_mode(void);
const char *epd_frame_mode_name(epd_frame_mode_t mode);

/*
 * Physical row mapping. The reference repository disagrees with itself here,
 * so it is selectable at runtime.
 */
typedef enum {
    EPD_MAP_LINEAR = 0,
    EPD_MAP_INTERLEAVE,
    EPD_MAP_INTERLEAVE2,
} epd_rowmap_t;

void epd_set_rowmap(epd_rowmap_t map);
epd_rowmap_t epd_rowmap(void);
const char *epd_rowmap_name(epd_rowmap_t map);

/*
 * The controller requires an 800x600x2bit transfer frame. In physical mode
 * the 768x552 image is placed at (32,0) and the remainder is packed white.
 */
void epd_set_physical_frame(bool enabled);
bool epd_physical_frame(void);

/* Send R11H (DSP) after the data and return the controller's Data_flag:
 * true means it received a complete frame. */
esp_err_t epd_data_stop(bool *data_flag);

/* Whether R11H is sent before each refresh. */
void epd_set_data_stop(bool enabled);
bool epd_data_stop_enabled(void);

/*
 * PSR scan direction. The reference driver uses SHL=1 UD=1; clearing both
 * rotates the output 180 degrees. Requires a re-init to take effect.
 */
void epd_set_psr_normal(bool normal);
bool epd_psr_normal(void);

/* Pack the four pixels of a byte in the opposite order. */
void epd_set_bit_order(bool reversed);
bool epd_bit_order_reversed(void);

/* Row-major (reference drivers) versus column-major (per-panel sketch). */
void epd_set_column_major(bool enabled);
bool epd_column_major(void);

/* Two init tables exist for this panel; variant B is PSR1 + 0x61 only. */
void epd_set_init_variant(bool variant);
bool epd_init_variant(void);

/*
 * Write the frame the way the dedicated SE0398NZ07A0 driver does: one display
 * line at a time with the logical lines interleaved onto alternating physical
 * gate lines.
 */
void epd_set_interleaved(bool enabled);
bool epd_interleaved(void);

/* SPI clock, so a bus that is too fast for the board can be ruled out. */
void epd_set_clock_hz(uint32_t hz);
uint32_t epd_clock_hz(void);

/*
 * Shift the whole frame by n pixels along x while transmitting. If a defective
 * region moves with the content it is a data/addressing problem; if it stays
 * at the same physical place it is a panel or waveform problem.
 */
void epd_set_x_offset(int offset);
int epd_x_offset(void);

/* Whether a full-panel 0x83 window is programmed before each full-frame write. */
void epd_set_partial_window_mode(bool enabled);
bool epd_partial_window_mode(void);

/* ------------------------------------------------------------------- api --- */

/* Bring up GPIO and the SPI bus. Does not touch the panel. */
esp_err_t epd_bus_init(void);

/* Hardware reset and register programming. */
esp_err_t epd_init(void);

/* Allocate the framebuffer. Required before any fb_* call. */
esp_err_t epd_fb_alloc(void);

/* --------------------------------------------------------------- strip ---- */
/* The framebuffer is a single 16-row strip. A render callback fills each
 * active strip in native coordinates, then the panel streams it. */
#define EPD_STRIP_ROWS 16u

/* 条带生命周期：渲染器对每个 16 行条带调用 begin -> 绘制 -> flush。 */
typedef esp_err_t (*epd_strip_render_fn)(void *ctx, int y0);
void epd_strip_begin(int y0);
bool epd_strip_active(void);
void epd_strip_copy(const uint8_t *src, size_t src_stride, int rows);
esp_err_t epd_write_strips(epd_strip_render_fn render, void *ctx);

/* ---------------------------------------------------------------- draw --- */
void epd_fb_fill(uint8_t color);
void epd_fb_set_pixel(int x, int y, uint8_t color);
void epd_fb_hline(int x0, int x1, int y, uint8_t color);
void epd_fb_vline(int x, int y0, int y1, uint8_t color);
void epd_fb_rect(int x, int y, int w, int h, uint8_t color);
void epd_fb_rect_fill(int x, int y, int w, int h, uint8_t color);
void epd_fb_rect_fill_frame(int x, int y, int w, int h, uint8_t color, int thickness);
void epd_fb_char(int x, int y, char c, uint8_t color, int scale);

/* ---------------------------------------------------------------- update --- */
/* Push one display line; used by the line-by-line transfer mode. */
esp_err_t epd_write_line(int y, const uint8_t *line192);

/* POWER_ON -> (data already in RAM) -> DISPLAY_REFRESH -> POWER_OFF. */
esp_err_t epd_refresh(bool then_power_off);
esp_err_t epd_power_on(void);
esp_err_t epd_power_off(void);
esp_err_t epd_sleep(void);

/* Wait for BUSY to reach EPD_BUSY_READY_LEVEL. */
esp_err_t epd_wait_busy(uint32_t timeout_ms);

/* Refresh the panel from the current framebuffer, end to end. */
esp_err_t epd_display(epd_strip_render_fn render, void *ctx);

const epd_diag_t *epd_diag(void);
const char *epd_stage_name(epd_stage_t stage);
int epd_busy_level(void);

#endif /* EPD_PANEL_H */
