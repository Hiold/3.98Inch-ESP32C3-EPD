/*
 * JD79665 4-colour e-paper driver for ESP32-C3 (ESP-IDF).
 *
 * Register table, timings and 2bpp packing follow
 * https://github.com/krstc/openepaperlinkforCN:
 *   refdoc/BLE_EPD_DISPLAY/EPD_3in98g.cpp        (init table + 768x552 geometry)
 *   refdoc/BLE_EPD_DISPLAY/EPD.cpp               (write-to-RAM / fillscreen)
 *   tag_fw/src/epd_driver/jd79665.cpp            (production driver for the
 *                                                 same controller family)
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "epd_panel.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";

static void stage_fail(epd_stage_t stage, esp_err_t err, const char *what);
esp_err_t epd_write_line(int y, const uint8_t *line192);

/* JD79665 command set, named as in the reference driver. */
#define CMD_PSR        0x00u /* panel setting            */
#define CMD_PWR        0x01u /* power setting            */
#define CMD_POF        0x02u /* power off                */
#define CMD_PON        0x04u /* power on                 */
#define CMD_BTST       0x06u /* booster soft start       */
#define CMD_DSLP       0x07u /* deep sleep               */
#define CMD_DTM        0x10u /* data start transmission  */
#define CMD_DSP        0x11u /* data stop: reports Data_flag */
#define CMD_DRF        0x12u /* display refresh          */
#define CMD_PLL        0x30u /* PLL control              */
#define CMD_VCOM       0x50u /* VCOM / data interval     */
#define CMD_TCON       0x60u /* TCON setting             */
#define CMD_TRES       0x61u /* resolution setting       */
#define CMD_GSST       0x65u /* GSST setting             */
#define CMD_PTL        0x83u /* partial window           */
#define CMD_AN_TM      0xAAu /* analog/analog mode       */
#define CMD_CCSET      0xE3u /* cascade setting          */
#define CMD_TSSET      0x84u /* temperature sensor        */

/* How pixel data is framed on the wire.
 *
 * The reference driver toggles CS once per byte (EPD::WriteDATA does
 * digitalWrite(CS,LOW); transfer; digitalWrite(CS,HIGH)). Our first attempt
 * streamed 8 KiB chunks under one CS assertion and left a stale band on the
 * panel, so CS framing is the prime suspect and must be testable.
 * epd_xfer_mode_t is declared in epd_panel.h. */
static const char *XFER_NAMES[] = {"chunk(8K)", "line(192B)", "byte(1B)",
                                   "continuous"};

static spi_device_handle_t s_spi;
static bool s_bus_ready;
/* The framebuffer is now a fixed 16-row strip (16 x 192 = 3072 bytes). Only
 * the currently active strip is drawn into; primitives clip to it in native
 * coordinates. No 106 KiB allocation is needed. */
#define EPD_STRIP_ROWS 16u
static uint8_t s_strip[EPD_STRIP_ROWS][EPD_BYTES_PER_LINE];
static int s_strip_y0;             /* current active strip's first row (native y), -1 = none */
static bool s_strip_active;
static epd_diag_t s_diag;

static inline uint8_t *strip_row_ptr(int r)
{
    return (r >= 0 && r < (int)EPD_STRIP_ROWS) ? s_strip[r] : NULL;
}

static epd_xfer_mode_t s_xfer = EPD_XFER_CHUNK;
/* Streamed full-frame writes are the path under test; EPD_FRAME_PER_LINE and
 * epd_write_line() remain available as the reference driver's alternative. */
static epd_frame_mode_t s_frame_mode = EPD_FRAME_SWEEP;
static uint32_t s_clock_hz = EPD_SPI_HZ;
static int s_x_offset = 0;
/* PSR1 as written by the reference driver. Clearing SHL and UD rotates the
 * output 180 degrees, which is what the calibration photo shows we need. */
static bool s_psr_normal = true;
/* false = full reference table (EPD_3in98g / jd79665),
 * true  = minimal table (EPD_3IN98G_Init: PSR1 + 0x61 only).
 * The minimal table leaves this panel refreshing to plain white because the
 * power/booster registers are never programmed, so the full table is the
 * power-on default and the switch exists only for a live console comparison. */
static bool s_init_variant = false;

void epd_set_init_variant(bool variant)
{
    s_init_variant = variant;
    ESP_LOGW(TAG, "init table: %s", variant ? "B (minimal)" : "A (reference)");
}

bool epd_init_variant(void) { return s_init_variant; }
/* Scratch line used when the frame has to be shifted before transmission. */
static uint8_t s_line_buf[EPD_BYTES_PER_LINE];

static const char *FRAME_NAMES[] = {"sweep", "per-line"};

/* Experiment switch: the reference drivers never program the partial window
 * before a full-frame write, they rely on whatever the power-on default is.
 * Setting this false reproduces that exactly. */
static bool s_use_partial_window = true;

void epd_set_partial_window_mode(bool enabled)
{
    s_use_partial_window = enabled;
    ESP_LOGW(TAG, "partial window (0x83) before full-frame write: %s",
             enabled ? "ON" : "OFF");
}

bool epd_partial_window_mode(void) { return s_use_partial_window; }

void epd_set_xfer_mode(epd_xfer_mode_t mode)
{
    s_xfer = mode;
    ESP_LOGW(TAG, "data framing: %s", XFER_NAMES[mode]);
}

epd_xfer_mode_t epd_xfer_mode(void) { return s_xfer; }

const char *epd_xfer_mode_name(epd_xfer_mode_t mode)
{
    return XFER_NAMES[mode];
}

void epd_set_frame_mode(epd_frame_mode_t mode)
{
    s_frame_mode = mode;
    ESP_LOGW(TAG, "frame strategy: %s", FRAME_NAMES[mode]);
}

epd_frame_mode_t epd_frame_mode(void) { return s_frame_mode; }

const char *epd_frame_mode_name(epd_frame_mode_t mode)
{
    return FRAME_NAMES[mode];
}

/*
 * Experiment switches retained for the live-console bring-up comparison. The
 * frame is now always streamed as 16-row strips, but the setters below remain
 * public API consumed by the diagnostic console.
 */
static bool s_column_major = false;
static bool s_interleaved = false;

void epd_set_interleaved(bool enabled)
{
    s_interleaved = enabled;
    ESP_LOGW(TAG, "frame write: %s",
             enabled ? "interleaved per-line (SE0398NZ07A0)" : "streamed");
}

bool epd_interleaved(void) { return s_interleaved; }

static const char *ROWMAP_NAMES[] = {"linear", "interleave", "interleave2"};

void epd_set_column_major(bool enabled)
{
    s_column_major = enabled;
    ESP_LOGW(TAG, "wire layout: %s", enabled ? "column-major" : "row-major");
}

bool epd_column_major(void) { return s_column_major; }

/* Physical gate order from SE0398NZ07A0.cpp: logical rows 0..275 map to
 * even physical rows, then logical rows 276..551 map to odd rows in reverse. */
static epd_rowmap_t s_rowmap = EPD_MAP_INTERLEAVE;

static int physical_row(int logical)
{
    switch (s_rowmap) {
    case EPD_MAP_INTERLEAVE:
        if (logical < EPD_HEIGHT / 2) {
            return logical * 2;
        }
        return EPD_HEIGHT - 1 - 2 * (logical - EPD_HEIGHT / 2);
    case EPD_MAP_INTERLEAVE2:
        if (logical < EPD_HEIGHT / 2) {
            return logical * 2 + 1;
        }
        return EPD_HEIGHT - 2 - 2 * (logical - EPD_HEIGHT / 2);
    case EPD_MAP_LINEAR:
    default:
        return logical;
    }
}

void epd_set_rowmap(epd_rowmap_t map)
{
    s_rowmap = map;
    ESP_LOGW(TAG, "row mapping: %s", ROWMAP_NAMES[map]);
}

epd_rowmap_t epd_rowmap(void) { return s_rowmap; }

const char *epd_rowmap_name(epd_rowmap_t map) { return ROWMAP_NAMES[map]; }

bool epd_psr_normal(void) { return s_psr_normal; }

void epd_set_psr_normal(bool normal)
{
    s_psr_normal = normal;
    ESP_LOGW(TAG, "PSR scan direction: %s",
             normal ? "reference (SHL=1 UD=1)" : "flipped (SHL=0 UD=0)");
}

/* JD79665 requires the controller-sized 800x600 frame on every DTM write. */
static bool s_physical_frame = true;

void epd_set_physical_frame(bool enabled)
{
    s_physical_frame = enabled;
    ESP_LOGW(TAG, "physical frame: %s",
             enabled ? "800x600 padded" : "768x552 tight");
}

bool epd_physical_frame(void) { return s_physical_frame; }

/* SE0398NZ07A0 reference sequence goes directly from line DTM writes to
 * PON -> DRF. It does not issue DSP (0x11); doing so leaves this controller
 * busy before DRF and produces no visible refresh. */
static bool s_data_stop = false;

void epd_set_data_stop(bool enabled)
{
    s_data_stop = enabled;
    ESP_LOGW(TAG, "R11H data stop command: %s", enabled ? "ON" : "OFF");
}

bool epd_data_stop_enabled(void) { return s_data_stop; }

/*
 * R11H (DSP) does two things: it terminates the data transmission and its
 * first parameter reports Data_flag, which is 1 only when the controller
 * received a complete frame. The datasheet also states that after 10h or 11h
 * with Data_flag=1, BUSY_N goes low and the panel starts refreshing.
 */
static esp_err_t write_cmd(uint8_t cmd);
static esp_err_t write_data(uint8_t data);
static esp_err_t write_data_block_continuous(const uint8_t *data, size_t len);

esp_err_t epd_data_stop(bool *data_flag)
{
    esp_err_t err = write_cmd(CMD_DSP);
    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level(PIN_EPD_DC, 1);
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_RXDATA,
        .length = 8,
    };
    /* A read needs the bus to shift in; a dummy byte is clocked out. */
    t.tx_buffer = NULL;
    err = spi_device_polling_transmit(s_spi, &t);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DSP read failed: %s", esp_err_to_name(err));
        return err;
    }
    uint8_t value = t.rx_data[0];
    if (data_flag) {
        *data_flag = (value & 0x80u) != 0;
    }
    s_diag.last_data_flag = (value & 0x80u) != 0;
    s_diag.last_dsp_value = value;
    ESP_LOGI(TAG, "DSP: Data_flag=%d (raw 0x%02x) -> %s", (value >> 7) & 1,
             value, (value & 0x80u) ? "complete frame received"
                                    : "controller says frame is INCOMPLETE");
    return ESP_OK;
}

/* One padded physical row, produced on demand so no 120 KB buffer is needed. */
static void strip_phys_line(int r, uint8_t *out)
{
    memset(out, 0x55, EPD_PHYS_BYTES_PER_LINE); /* white */
    memcpy(out + EPD_PHYS_X_OFFSET / 4, s_strip[r], EPD_BYTES_PER_LINE);
}

uint32_t epd_clock_hz(void) { return s_clock_hz; }

void epd_set_x_offset(int offset)
{
    if (offset < 0) {
        offset = 0;
    }
    if (offset > EPD_WIDTH) {
        offset = EPD_WIDTH;
    }
    s_x_offset = offset;
    ESP_LOGW(TAG, "frame x offset: %d px", offset);
}

int epd_x_offset(void) { return s_x_offset; }

void epd_set_clock_hz(uint32_t hz)
{
    if (!s_bus_ready || hz == s_clock_hz) {
        s_clock_hz = hz;
        return;
    }
    /* The SPI device has to be re-created for a frequency change, because
     * re-adding an existing device fails outright. */
    spi_bus_remove_device(s_spi);
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = (int)hz,
        .mode = 0,
        .spics_io_num = PIN_EPD_CS,
        .queue_size = 1,
    };
    esp_err_t err = spi_bus_add_device(EPD_SPI_HOST, &devcfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot set SPI clock to %" PRIu32 " Hz: %s", hz,
                 esp_err_to_name(err));
        /* Put the old device back so the panel stays usable. */
        devcfg.clock_speed_hz = (int)s_clock_hz;
        spi_bus_add_device(EPD_SPI_HOST, &devcfg, &s_spi);
        return;
    }
    s_clock_hz = hz;
    ESP_LOGW(TAG, "SPI clock: %" PRIu32 " Hz", hz);
}

/* ------------------------------------------------------------- diagnostics - */

const epd_diag_t *epd_diag(void) { return &s_diag; }

const char *epd_stage_name(epd_stage_t stage)
{
    switch (stage) {
    case EPD_STAGE_IDLE:         return "IDLE";
    case EPD_STAGE_BUS_UP:       return "BUS_UP";
    case EPD_STAGE_RESET:        return "RESET";
    case EPD_STAGE_BUSY_RELEASE: return "BUSY_RELEASE";
    case EPD_STAGE_REGISTERS:    return "REGISTERS";
    case EPD_STAGE_POWER_ON:     return "POWER_ON";
    case EPD_STAGE_DATA:         return "DATA";
    case EPD_STAGE_REFRESH:      return "REFRESH";
    case EPD_STAGE_POWER_OFF:    return "POWER_OFF";
    case EPD_STAGE_SLEEP:        return "SLEEP";
    default:                     return "?";
    }
}

int epd_busy_level(void)
{
    return gpio_get_level(PIN_EPD_BUSY);
}

static void stage_fail(epd_stage_t stage, esp_err_t err, const char *what)
{
    s_diag.last_stage = stage;
    s_diag.last_err = err;
    ESP_LOGE(TAG, "stage %s FAILED (%s): %s", epd_stage_name(stage),
             esp_err_to_name(err), what);
}

/* ---------------------------------------------------------------- low level - */

static esp_err_t spi_tx(const uint8_t *buf, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }
    /* The whole frame fits in one transaction; the driver splits it internally
     * as needed. Command byte and payload are sent together with DC held low
     * for the first byte only, which is exactly what the panel requires. */
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = buf,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t write_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_EPD_DC, 0);
    return spi_tx(&cmd, 1);
}

static esp_err_t write_data(uint8_t data)
{
    gpio_set_level(PIN_EPD_DC, 1);
    return spi_tx(&data, 1);
}

/* Data is sent in chunks: one transaction per byte would cost >100000
 * transactions per frame, one transaction for the whole frame risks per-
 * transaction length limits. 8 KiB is the sweet spot. */
#define EPD_DATA_CHUNK 8192

static esp_err_t write_data_block(const uint8_t *data, size_t len)
{
    gpio_set_level(PIN_EPD_DC, 1);

    if (s_xfer == EPD_XFER_CONTINUOUS) {
        return write_data_block_continuous(data, len);
    }

    switch (s_xfer) {
    case EPD_XFER_BYTE:
        for (size_t i = 0; i < len; i++) {
            esp_err_t err = spi_tx(&data[i], 1);
            if (err != ESP_OK) {
                return err;
            }
        }
        return ESP_OK;

    case EPD_XFER_LINE:
        while (len > 0) {
            size_t n = len > EPD_BYTES_PER_LINE ? EPD_BYTES_PER_LINE : len;
            esp_err_t err = spi_tx(data, n);
            if (err != ESP_OK) {
                return err;
            }
            data += n;
            len -= n;
        }
        return ESP_OK;

    case EPD_XFER_CHUNK:
    default:
        while (len > 0) {
            size_t n = len > EPD_DATA_CHUNK ? EPD_DATA_CHUNK : len;
            esp_err_t err = spi_tx(data, n);
            if (err != ESP_OK) {
                return err;
            }
            data += n;
            len -= n;
        }
        return ESP_OK;
    }
}

/* Retain CS across bounded SPI transactions for a payload. The DTM command
 * itself is a separate command transaction, matching the public JD79665
 * driver; this keeps only the payload stream contiguous. */
static esp_err_t write_data_block_continuous(const uint8_t *data, size_t len)
{
    esp_err_t err = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }

    while (len > 0) {
        size_t n = len > EPD_DATA_CHUNK ? EPD_DATA_CHUNK : len;
        const uint8_t *tx = data;
        spi_transaction_t t = {
            .length = n * 8,
            .tx_buffer = tx,
            .flags = (len > n) ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
        };
        err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) {
            break;
        }
        data += n;
        len -= n;
    }
    spi_device_release_bus(s_spi);
    return err;
}

static esp_err_t cmd_with_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_err_t err = write_cmd(cmd);
    if (err != ESP_OK) {
        return err;
    }
    return write_data_block(data, len);
}

/* -------------------------------------------------------------------- busy - */

esp_err_t epd_wait_busy(uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time();
    int64_t deadline = start + (int64_t)timeout_ms * 1000;
    int loops = 0;

    while (gpio_get_level(PIN_EPD_BUSY) != EPD_BUSY_READY_LEVEL) {
        loops++;
        if (esp_timer_get_time() >= deadline) {
            s_diag.busy_loop_count = loops;
            s_diag.last_busy_wait_us = esp_timer_get_time() - start;
            s_diag.busy_timeouts++;
            ESP_LOGE(TAG, "BUSY stuck LOW for %u ms (BUSY=GPIO%d, %d polls)",
                     (unsigned)timeout_ms, PIN_EPD_BUSY, loops);
            return ESP_ERR_TIMEOUT;
        }
        if (loops < 200) {
            /* Short edges (reset release, PON) resolve in microseconds; a
             * busy-wait measures them accurately without a tick of jitter. */
            esp_rom_delay_us(50);
        } else {
            /* Long edges (a full four-colour refresh) take seconds: yield so
             * the idle task and the rest of the system keep running. */
            vTaskDelay(1);
        }
    }

    s_diag.busy_loop_count = loops;
    s_diag.last_busy_wait_us = esp_timer_get_time() - start;
    return ESP_OK;
}

/* ------------------------------------------------------------------- reset - */

static esp_err_t epd_hw_reset(void)
{
    /* Timing follows EPD_3IN98G_Reset() in the reference driver. */
    gpio_set_level(PIN_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_EPD_RST, 0);
    /* 2 ms is below the RTOS tick; use a ROM busy-wait so the pulse is real. */
    esp_rom_delay_us(2000);
    gpio_set_level(PIN_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

/* -------------------------------------------------------------------- init - */

esp_err_t epd_bus_init(void)
{
    if (s_bus_ready) {
        return ESP_OK;
    }
    s_diag.last_stage = EPD_STAGE_BUS_UP;

    /* Sample BUSY before wiring anything else up: a floating or stuck pin here
     * is the cheapest possible hardware fault to spot. */
    gpio_config_t busy_cfg = {
        .pin_bit_mask = 1ULL << PIN_EPD_BUSY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&busy_cfg));
    vTaskDelay(pdMS_TO_TICKS(2));
    s_diag.busy_level_at_boot = gpio_get_level(PIN_EPD_BUSY);

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << PIN_EPD_CS) | (1ULL << PIN_EPD_DC) |
                        (1ULL << PIN_EPD_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));

    gpio_set_level(PIN_EPD_CS, 1);
    gpio_set_level(PIN_EPD_DC, 1);
    gpio_set_level(PIN_EPD_RST, 1);

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_EPD_DIN,
        .miso_io_num = -1,
        .sclk_io_num = PIN_EPD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_SPI_MAX_XFER,
    };
    esp_err_t err = spi_bus_initialize(EPD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        stage_fail(EPD_STAGE_BUS_UP, err, "spi_bus_initialize");
        return err;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = (int)s_clock_hz,
        .mode = 0,
        .spics_io_num = PIN_EPD_CS,
        /* CS is driven by the peripheral, so every transaction is framed. */
        .queue_size = 1,
    };
    err = spi_bus_add_device(EPD_SPI_HOST, &devcfg, &s_spi);
    if (err != ESP_OK) {
        stage_fail(EPD_STAGE_BUS_UP, err, "spi_bus_add_device");
        return err;
    }

    s_bus_ready = true;
    ESP_LOGI(TAG, "bus up: host=%d sck=IO%d mosi=IO%d cs=IO%d dc=IO%d rst=IO%d busy=IO%d @%d Hz",
             EPD_SPI_HOST, PIN_EPD_SCK, PIN_EPD_DIN, PIN_EPD_CS, PIN_EPD_DC,
             PIN_EPD_RST, PIN_EPD_BUSY, EPD_SPI_HZ);
    ESP_LOGI(TAG, "BUSY at boot = %d (1=ready, 0=busy/stuck)",
             s_diag.busy_level_at_boot);
    return ESP_OK;
}

esp_err_t epd_init(void)
{
    if (!s_bus_ready) {
        esp_err_t err = epd_bus_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    /* --- reset ---------------------------------------------------------- */
    s_diag.last_stage = EPD_STAGE_RESET;
    ESP_LOGI(TAG, "resetting panel...");
    epd_hw_reset();

    /* --- BUSY must be released ------------------------------------------ */
    s_diag.last_stage = EPD_STAGE_BUSY_RELEASE;
    esp_err_t err = epd_wait_busy(EPD_RESET_BUSY_MS);
    if (err != ESP_OK) {
        stage_fail(EPD_STAGE_BUSY_RELEASE, err,
                   "panel never released BUSY after reset");
        ESP_LOGE(TAG, "check: panel power (VDD/VCI/boost), FPC seating, "
                      "BUSY=IO%d wiring, RST=IO%d, and CS/DC/SCK/DIN",
                 PIN_EPD_BUSY, PIN_EPD_RST);
        return err;
    }
    ESP_LOGI(TAG, "BUSY released after reset in %lld us",
             (long long)s_diag.last_busy_wait_us);

    /* Two init tables exist for this panel in the reference repository. */
    if (s_init_variant) {
        ESP_LOGW(TAG, "init variant B: PSR + 0x61 only (EPD_3IN98G_Init)");
        static const uint8_t psr_b[] = {0x0B};
        static const uint8_t tres_b[] = {EPD_PHYS_WIDTH >> 8, EPD_PHYS_WIDTH & 0xFF,
                                         EPD_PHYS_HEIGHT >> 8, EPD_PHYS_HEIGHT & 0xFF};
        esp_err_t e1 = cmd_with_data(CMD_PSR, psr_b, sizeof(psr_b));
        if (e1 != ESP_OK) {
            stage_fail(EPD_STAGE_REGISTERS, e1, "variant B PSR");
            return e1;
        }
        esp_err_t e2 = cmd_with_data(CMD_TRES, tres_b, sizeof(tres_b));
        if (e2 != ESP_OK) {
            stage_fail(EPD_STAGE_REGISTERS, e2, "variant B TRES");
            return e2;
        }
        s_diag.last_stage = EPD_STAGE_IDLE;
        s_diag.last_err = ESP_OK;
        ESP_LOGI(TAG, "panel registers programmed (variant B)");
        return ESP_OK;
    }

    /* The calibration page shows the image mirrored in x and flipped in y,
     * i.e. rotated 180 degrees. PSR bits SHL (shift direction) and UD (gate
     * scan direction) control exactly that. */
    uint8_t psr0 = s_psr_normal ? 0x4B : 0x43;
    ESP_LOGW(TAG, "PSR1 = 0x%02x (%s)", psr0,
             s_psr_normal ? "SHL=1 UD=1, reference default"
                          : "SHL=0 UD=0, 180 degree flip");

    /* --- register table ------------------------------------------------- */
    s_diag.last_stage = EPD_STAGE_REGISTERS;

#define EPD_CHECK(expr)                        \
    do {                                       \
        err = (expr);                          \
        if (err != ESP_OK) {                   \
            stage_fail(EPD_STAGE_REGISTERS, err, #expr); \
            return err;                        \
        }                                      \
    } while (0)

    static const uint8_t an_tm[]  = {0x49, 0x55, 0x20, 0x08, 0x09, 0x18};
    static const uint8_t pwr[]    = {0x3F};
    const uint8_t psr[]           = {psr0, 0x69};
    static const uint8_t tcon[]   = {0x02, 0x00};
    static const uint8_t vcom[]   = {0x3F};
    static const uint8_t tres[]   = {EPD_PHYS_WIDTH >> 8, EPD_PHYS_WIDTH & 0xFF,
                                     EPD_PHYS_HEIGHT >> 8, EPD_PHYS_HEIGHT & 0xFF};
    static const uint8_t gsst[]   = {0x10, 0x00, 0x20, 0x00};

    EPD_CHECK(cmd_with_data(CMD_AN_TM, an_tm, sizeof(an_tm)));
    EPD_CHECK(cmd_with_data(CMD_PWR, pwr, sizeof(pwr)));
    EPD_CHECK(cmd_with_data(CMD_PSR, psr, sizeof(psr)));
    { static const uint8_t d[] = {0x40, 0x1F, 0x1F, 0x2C};
      EPD_CHECK(cmd_with_data(0x05, d, sizeof(d))); }
    { static const uint8_t d[] = {0x6F, 0x1F, 0x1F, 0x22};
      EPD_CHECK(cmd_with_data(0x08, d, sizeof(d))); }
    { static const uint8_t d[] = {0x6F, 0x1F, 0x14, 0x14};
      EPD_CHECK(cmd_with_data(CMD_BTST, d, sizeof(d))); }
    { static const uint8_t d[] = {0x00, 0x54, 0x00, 0x44};
      EPD_CHECK(cmd_with_data(0x03, d, sizeof(d))); }
    EPD_CHECK(cmd_with_data(CMD_TCON, tcon, sizeof(tcon)));
    { static const uint8_t d[] = {0x08};
      EPD_CHECK(cmd_with_data(CMD_PLL, d, sizeof(d))); }
    EPD_CHECK(cmd_with_data(CMD_VCOM, vcom, sizeof(vcom)));
    EPD_CHECK(cmd_with_data(CMD_TRES, tres, sizeof(tres)));
    EPD_CHECK(cmd_with_data(CMD_GSST, gsst, sizeof(gsst)));
    { static const uint8_t d[] = {0x2F};
      EPD_CHECK(cmd_with_data(CMD_CCSET, d, sizeof(d))); }
    { static const uint8_t d[] = {0x01};
      EPD_CHECK(cmd_with_data(CMD_TSSET, d, sizeof(d))); }

    /* Full-panel partial window; PMODE=1 as in the reference driver. */
    { uint8_t w[9] = {0x00, 0x00,
                      (EPD_PHYS_WIDTH - 1) >> 8, (EPD_PHYS_WIDTH - 1) & 0xFF,
                      0x00, 0x00,
                      (EPD_PHYS_HEIGHT - 1) >> 8, (EPD_PHYS_HEIGHT - 1) & 0xFF,
                      0x01};
      EPD_CHECK(cmd_with_data(CMD_PTL, w, sizeof(w))); }
#undef EPD_CHECK

    s_diag.last_stage = EPD_STAGE_IDLE;
    s_diag.last_err = ESP_OK;
    ESP_LOGI(TAG, "panel registers programmed (controller %dx%d, image %dx%d, %d byte frame)",
             EPD_PHYS_WIDTH, EPD_PHYS_HEIGHT, EPD_WIDTH, EPD_HEIGHT,
             EPD_PHYS_FRAME_BYTES);
    return ESP_OK;
}

/* ---------------------------------------------------------------- framebuf - */

esp_err_t epd_fb_alloc(void)
{
    /* 条带缓冲为静态数组，无需运行时分配。 */
    s_strip_active = false;
    return ESP_OK;
}

void epd_strip_begin(int y0)
{
    if (y0 < 0) y0 = 0;
    if (y0 >= EPD_HEIGHT) y0 = EPD_HEIGHT - (int)EPD_STRIP_ROWS;
    s_strip_y0 = y0;
    s_strip_active = true;
    /* 白 = 0x55（4 像素均为白 01）。 */
    const uint8_t white = 0x55u;
    for (size_t i = 0; i < EPD_STRIP_ROWS; ++i) {
        memset(s_strip[i], white, EPD_BYTES_PER_LINE);
    }
}

bool epd_strip_active(void) { return s_strip_active; }

void epd_strip_copy(const uint8_t *src, size_t src_stride, int rows)
{
    if (!src || !s_strip_active) return;
    if (rows > (int)EPD_STRIP_ROWS) rows = (int)EPD_STRIP_ROWS;
    for (int r = 0; r < rows; ++r) {
        memcpy(s_strip[r], src + (size_t)r * src_stride, EPD_BYTES_PER_LINE);
    }
}

void epd_fb_fill(uint8_t color)
{
    if (!s_strip_active) return;
    uint8_t packed = (uint8_t)((color << 6) | (color << 4) | (color << 2) | color);
    for (size_t i = 0; i < EPD_STRIP_ROWS; ++i) {
        memset(s_strip[i], packed, EPD_BYTES_PER_LINE);
    }
}

void epd_fb_set_pixel(int x, int y, uint8_t color)
{
    if (!s_strip_active || x < 0 || y < 0 || x >= EPD_WIDTH) {
        return;
    }
    const int r = y - s_strip_y0;
    if (r < 0 || r >= (int)EPD_STRIP_ROWS) {
        return; /* 不在当前条带，丢弃 */
    }
    size_t idx = (size_t)(x >> 2);
    int shift = 6 - 2 * (x & 3);
    uint8_t *byte = &s_strip[r][idx];
    *byte = (uint8_t)((*byte & ~(0x03u << shift)) | ((color & 0x03u) << shift));
}

void epd_fb_hline(int x0, int x1, int y, uint8_t color)
{
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    for (int x = x0; x <= x1; x++) {
        epd_fb_set_pixel(x, y, color);
    }
}

void epd_fb_vline(int x, int y0, int y1, uint8_t color)
{
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    for (int y = y0; y <= y1; y++) {
        epd_fb_set_pixel(x, y, color);
    }
}

void epd_fb_rect(int x, int y, int w, int h, uint8_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    epd_fb_hline(x, x + w - 1, y, color);
    epd_fb_hline(x, x + w - 1, y + h - 1, color);
    epd_fb_vline(x, y, y + h - 1, color);
    epd_fb_vline(x + w - 1, y, y + h - 1, color);
}

void epd_fb_rect_fill(int x, int y, int w, int h, uint8_t color)
{
    for (int row = y; row < y + h; row++) {
        epd_fb_hline(x, x + w - 1, row, color);
    }
}

void epd_fb_rect_fill_frame(int x, int y, int w, int h, uint8_t color, int thickness)
{
    for (int t = 0; t < thickness; t++) {
        epd_fb_rect(x + t, y + t, w - 2 * t, h - 2 * t, color);
    }
}

/* ------------------------------------------------------------------- fonts - */

/* 5x7 glyphs, one byte per row, bit 4 = leftmost column. */
typedef struct {
    char c;
    uint8_t rows[7];
} glyph_t;

static const glyph_t FONT5X7[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
};

void epd_fb_char(int x, int y, char c, uint8_t color, int scale)
{
    if (scale < 1) {
        scale = 1;
    }
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    const uint8_t *rows = NULL;
    static const uint8_t blank[7] = {0};
    for (size_t i = 0; i < sizeof(FONT5X7) / sizeof(FONT5X7[0]); i++) {
        if (FONT5X7[i].c == c) {
            rows = FONT5X7[i].rows;
            break;
        }
    }
    if (!rows) {
        rows = blank;
    }
    for (int r = 0; r < 7; r++) {
        for (int b = 0; b < 5; b++) {
            if (rows[r] & (1u << (4 - b))) {
                if (scale == 1) {
                    epd_fb_set_pixel(x + b, y + r, color);
                } else {
                    epd_fb_rect_fill(x + b * scale, y + r * scale, scale, scale, color);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ update - */

static esp_err_t epd_set_full_window(void)
{
    /* Match the window to the frame we actually transmit: the panel is driven
     * from the whole physical frame in physical mode. */
    uint16_t w = s_physical_frame ? EPD_PHYS_WIDTH : EPD_WIDTH;
    uint16_t h = s_physical_frame ? EPD_PHYS_HEIGHT : EPD_HEIGHT;
    uint8_t win[9] = {0x00, 0x00,
                      (uint8_t)((w - 1) >> 8), (uint8_t)((w - 1) & 0xFF),
                      0x00, 0x00,
                      (uint8_t)((h - 1) >> 8), (uint8_t)((h - 1) & 0xFF),
                      0x01};
    return cmd_with_data(CMD_PTL, win, sizeof(win));
}

/*
 * Transmit the controller-sized 800x600 frame as 16-row strips. The render
 * callback fills the active strip in native coordinates before each group of
 * physical rows is streamed. Reuses the proven "one bus acquisition + row-
 * continuous SPI" mechanism from the old full-frame writer.
 */
typedef esp_err_t (*epd_strip_render_fn)(void *ctx, int y0);

esp_err_t epd_write_strips(epd_strip_render_fn render, void *ctx)
{
    if (!s_bus_ready) return ESP_ERR_INVALID_STATE;
    s_diag.last_stage = EPD_STAGE_DATA;

    esp_err_t err = epd_set_full_window();
    if (err != ESP_OK) { stage_fail(EPD_STAGE_DATA, err, "set full window"); return err; }
    err = write_cmd(CMD_DTM);
    if (err != ESP_OK) { stage_fail(EPD_STAGE_DATA, err, "DTM"); return err; }

    static uint8_t line[EPD_PHYS_BYTES_PER_LINE];
    err = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (err != ESP_OK) { stage_fail(EPD_STAGE_DATA, err, "acquire strip bus"); return err; }
    gpio_set_level(PIN_EPD_DC, 1);

    int py = 0;
    for (int y0 = 0; y0 < EPD_HEIGHT; y0 += (int)EPD_STRIP_ROWS) {
        epd_strip_begin(y0);
        if (render) {
            err = render(ctx, y0);
            if (err != ESP_OK) break;
        }
        for (int r = 0; r < (int)EPD_STRIP_ROWS; ++r) {
            strip_phys_line(r, line);
            spi_transaction_t t = {
                .length = EPD_PHYS_BYTES_PER_LINE * 8,
                .tx_buffer = line,
                .flags = (py + 1 < EPD_PHYS_HEIGHT) ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
            };
            err = spi_device_polling_transmit(s_spi, &t);
            if (err != ESP_OK) break;
            ++py;
            if (py >= EPD_PHYS_HEIGHT) break;
        }
        if (err != ESP_OK) break;
    }
    /* 补足图像区之后的物理行（552..599）为白色，凑满 600 行控制器帧。 */
    while (err == ESP_OK && py < EPD_PHYS_HEIGHT) {
        memset(line, 0x55, EPD_PHYS_BYTES_PER_LINE);
        spi_transaction_t t = {
            .length = EPD_PHYS_BYTES_PER_LINE * 8,
            .tx_buffer = line,
            .flags = (py + 1 < EPD_PHYS_HEIGHT) ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
        };
        err = spi_device_polling_transmit(s_spi, &t);
        ++py;
    }
    spi_device_release_bus(s_spi);
    if (err != ESP_OK) { stage_fail(EPD_STAGE_DATA, err, "strip payload"); return err; }

    s_diag.frame_count++;
    s_diag.bytes_sent += EPD_PHYS_FRAME_BYTES;
    s_diag.last_stage = EPD_STAGE_IDLE;
    s_diag.last_err = ESP_OK;
    ESP_LOGI(TAG, "strip frame sent: %d rows, %u bytes total",
             EPD_PHYS_HEIGHT, (unsigned)EPD_PHYS_FRAME_BYTES);
    return ESP_OK;
}

esp_err_t epd_write_line(int y, const uint8_t *line192)
{
    if (!s_bus_ready || y < 0 || y >= EPD_PHYS_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Single-row window, as in SE0398NZ07A0::setWriteRow(). */
    uint8_t w[9] = {0x00, 0x00,
                    (EPD_WIDTH - 1) >> 8, (EPD_WIDTH - 1) & 0xFF,
                    (uint8_t)(y >> 8), (uint8_t)(y & 0xFF),
                    (uint8_t)(y >> 8), (uint8_t)(y & 0xFF),
                    0x01};
    esp_err_t err = cmd_with_data(CMD_PTL, w, sizeof(w));
    if (err != ESP_OK) {
        return err;
    }
    err = write_cmd(CMD_DTM);
    if (err != ESP_OK) {
        return err;
    }
    return write_data_block(line192, EPD_BYTES_PER_LINE);
}

esp_err_t epd_power_on(void)
{
    s_diag.last_stage = EPD_STAGE_POWER_ON;
    esp_err_t err = write_cmd(CMD_PON);
    if (err != ESP_OK) {
        stage_fail(EPD_STAGE_POWER_ON, err, "PON");
        return err;
    }
    err = epd_wait_busy(EPD_BUSY_TIMEOUT_MS);
    if (err != ESP_OK) {
        stage_fail(EPD_STAGE_POWER_ON, err, "BUSY never released after PON");
        return err;
    }
    ESP_LOGI(TAG, "PON ok, BUSY released in %lld us",
             (long long)s_diag.last_busy_wait_us);
    return ESP_OK;
}

esp_err_t epd_refresh(bool then_power_off)
{
    s_diag.last_stage = EPD_STAGE_REFRESH;
    /* The public JD79665 sequence reasserts the complete physical PTL before
     * DRF. This also prevents a preceding diagnostic row write from leaving
     * a stale partial window active. */
    esp_err_t err = s_physical_frame ? epd_set_full_window() : ESP_OK;
    if (err != ESP_OK) {
        stage_fail(EPD_STAGE_REFRESH, err, "restore full physical window");
        return err;
    }
    err = write_cmd(CMD_DRF);
    if (err != ESP_OK) {
        stage_fail(EPD_STAGE_REFRESH, err, "DRF");
        return err;
    }
    err = write_data(0x00);
    if (err != ESP_OK) {
        stage_fail(EPD_STAGE_REFRESH, err, "DRF argument");
        return err;
    }
    int64_t start = esp_timer_get_time();
    err = epd_wait_busy(EPD_BUSY_TIMEOUT_MS);
    if (err != ESP_OK) {
        stage_fail(EPD_STAGE_REFRESH, err, "BUSY never released after DRF");
        ESP_LOGE(TAG, "a powered panel that never goes busy usually means "
                      "the controller never saw valid SPI traffic");
        return err;
    }
    ESP_LOGI(TAG, "refresh complete in %lld ms",
             (esp_timer_get_time() - start) / 1000);
    s_diag.refresh_count++;

    if (then_power_off) {
        return epd_power_off();
    }
    s_diag.last_stage = EPD_STAGE_IDLE;
    s_diag.last_err = ESP_OK;
    return ESP_OK;
}

esp_err_t epd_power_off(void)
{
    s_diag.last_stage = EPD_STAGE_POWER_OFF;
    esp_err_t err = write_cmd(CMD_POF);
    if (err != ESP_OK) {
        stage_fail(EPD_STAGE_POWER_OFF, err, "POF");
        return err;
    }
    err = write_data(0x00);
    if (err != ESP_OK) {
        stage_fail(EPD_STAGE_POWER_OFF, err, "POF argument");
        return err;
    }
    err = epd_wait_busy(EPD_BUSY_TIMEOUT_MS);
    if (err != ESP_OK) {
        stage_fail(EPD_STAGE_POWER_OFF, err, "BUSY never released after POF");
        return err;
    }
    s_diag.last_stage = EPD_STAGE_IDLE;
    s_diag.last_err = ESP_OK;
    return ESP_OK;
}

esp_err_t epd_sleep(void)
{
    s_diag.last_stage = EPD_STAGE_SLEEP;
    esp_err_t err = write_cmd(CMD_DSLP);
    if (err != ESP_OK) {
        return err;
    }
    return write_data(0xA5);
}

esp_err_t epd_display(epd_strip_render_fn render, void *ctx)
{
    esp_err_t err = epd_power_on();
    if (err != ESP_OK) return err;
    err = epd_write_strips(render, ctx);
    if (err != ESP_OK) return err;
    return epd_refresh(true);
}
