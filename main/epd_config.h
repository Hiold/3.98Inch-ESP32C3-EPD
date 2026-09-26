/*
 * 3.98 inch 4-colour (black / white / yellow / red) e-paper on ESP32-C3.
 *
 * Reference: https://github.com/krstc/openepaperlinkforCN
 *   refdoc/BLE_EPD_DISPLAY/EPD_3in98g.cpp          (768x552 BWRY)
 *   refdoc/BLE_EPD_DISPLAY/SE0398NZ07A0.cpp        (same panel, other panel family)
 *   tag_fw/src/epd_driver/jd79665.cpp              (maintainer's production driver)
 *
 * All three sources agree on the same JD79665-family register table and on
 * 2-bits-per-pixel packed data, so that is what this project implements.
 */
#ifndef EPD_CONFIG_H
#define EPD_CONFIG_H

#include "driver/gpio.h"
#include "driver/spi_master.h"

/* ---------------------------------------------------------------- panel ---- */

#define EPD_WIDTH            768
#define EPD_HEIGHT           552
/* 2 bits per pixel, packed 4 pixels per byte, leftmost pixel in bits 7:6. */
#define EPD_BYTES_PER_LINE   (EPD_WIDTH / 4)          /* 192 */
#define EPD_FRAME_BYTES      (EPD_BYTES_PER_LINE * EPD_HEIGHT) /* 105984 */

/*
 * The controller's built-in frame memory is 800 x 600 x 2 bit and the public
 * JD79665 driver requires that exact transfer geometry. The visible 768x552
 * image is embedded at x=32, y=0; all remaining pixels are packed white.
 */
#define EPD_PHYS_WIDTH       800
#define EPD_PHYS_HEIGHT      600
#define EPD_PHYS_BYTES_PER_LINE (EPD_PHYS_WIDTH / 4)             /* 200 */
#define EPD_PHYS_FRAME_BYTES (EPD_PHYS_BYTES_PER_LINE * EPD_PHYS_HEIGHT)
/* JD79665 public driver layout: the 768x552 visible image starts at x=32,
 * y=0 in the controller's mandatory 800x600 transfer frame. */
#define EPD_PHYS_X_OFFSET    32
#define EPD_PHYS_Y_OFFSET    0

_Static_assert(EPD_PHYS_WIDTH == 800 && EPD_PHYS_HEIGHT == 600,
               "JD79665 transfer geometry must remain 800x600");
_Static_assert(EPD_PHYS_BYTES_PER_LINE == 200 && EPD_PHYS_FRAME_BYTES == 120000,
               "JD79665 frame must be 600 rows of 200 bytes");
_Static_assert(EPD_PHYS_X_OFFSET == 32 && EPD_PHYS_Y_OFFSET == 0,
               "JD79665 visible image offset must be (32,0)");
_Static_assert((EPD_PHYS_X_OFFSET % 4) == 0,
               "JD79665 image offset must be byte aligned");

/* --------------------------------------------------------------- pinout ---- */
/* As wired on this board. */
#define PIN_EPD_DIN          3   /* MOSI */
#define PIN_EPD_SCK          4   /* SCLK */
#define PIN_EPD_CS           5
#define PIN_EPD_DC           6
#define PIN_EPD_RST          7
#define PIN_EPD_BUSY         10
/* No MISO: the panel is write-only. */

/* ------------------------------------------------------------ spi timing --- */
/* The reference firmware runs the bus at 10 MHz. Raise only after the panel
 * is proven to work. */
#define EPD_SPI_HOST         SPI2_HOST
#define EPD_SPI_HZ           (10 * 1000 * 1000)
#define EPD_SPI_MAX_XFER     EPD_PHYS_FRAME_BYTES

/* ---------------------------------------------------------------- colour --- */
/* 2-bit codes are defined by the panel; do not permute without a visual test. */
#define EPD_COLOR_BLACK      0x00u
#define EPD_COLOR_WHITE      0x01u
#define EPD_COLOR_YELLOW     0x02u
#define EPD_COLOR_RED        0x03u

/* ------------------------------------------------------------- pin levels -- */
/* JD79665 BUSY is active low: HIGH == ready, LOW == busy. */
#define EPD_BUSY_READY_LEVEL 1

/* Generous: a full four-colour refresh on this panel takes seconds. */
#define EPD_BUSY_TIMEOUT_MS  45000
/* BUSY must be released after reset; if it is not, the panel or the wiring is
 * suspect and we want to know before pushing a megabit of pixels. */
#define EPD_RESET_BUSY_MS    3000

#endif /* EPD_CONFIG_H */
