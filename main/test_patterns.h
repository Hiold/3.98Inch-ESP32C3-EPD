#ifndef TEST_PATTERNS_H
#define TEST_PATTERNS_H

#include "epd_panel.h"

/* Each routine draws into the framebuffer and then refreshes the panel. */
esp_err_t test_pattern_solid(uint8_t color);
esp_err_t test_pattern_info(void);
esp_err_t test_pattern_info_b(void);
esp_err_t test_pattern_bands_h(void);
esp_err_t test_pattern_checker(int cell);
esp_err_t test_pattern_stripes(void);
esp_err_t test_pattern_stripes_v(void);
esp_err_t test_pattern_axis_probe_h(void);
esp_err_t test_pattern_axis_probe_v(void);
esp_err_t test_pattern_ref_image(void);

/* Band probe: A draws blocks in the suspect rows, B blanks the page. */
esp_err_t test_pattern_probe_a(void);
esp_err_t test_pattern_probe_b(void);
void test_probe_sequence(unsigned dwell_ms);

/* Serial diagnostics. */
void test_dump_fb(void);
void test_report(void);

/* Dispatch a single character test selector. */
esp_err_t test_run(const char *cmd);

/* Handle one whitespace separated console token; false if unrecognised. */
bool test_console_command(const char *cmd, size_t len);

#endif /* TEST_PATTERNS_H */
