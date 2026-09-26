#ifndef MANUAL_FONT_BITMAP_H
#define MANUAL_FONT_BITMAP_H

#include <stdint.h>

#define MANUAL_ASCII_WIDTH 16
#define MANUAL_ASCII_HEIGHT 24
#define MANUAL_CJK_WIDTH 32
#define MANUAL_CJK_HEIGHT 32
#define MANUAL_DATE_DIGIT_WIDTH 48
#define MANUAL_DATE_DIGIT_HEIGHT 72

const uint8_t *manual_font_ascii_rows(uint8_t codepoint);
const uint8_t *manual_font_cjk_rows(uint32_t codepoint);
const uint8_t *manual_font_date_digit_rows(uint8_t digit);

#endif
