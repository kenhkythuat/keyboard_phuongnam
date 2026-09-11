#pragma once

#include <stdint.h>

#include "esp_err.h"

#define CONTROL_DISPLAY_ROWS       3U
#define CONTROL_DISPLAY_COLUMNS    6U

void control_display_led_init(void);
void control_display_led_clear(void);
void control_display_led_set_segments(
    const uint8_t segments[CONTROL_DISPLAY_ROWS][CONTROL_DISPLAY_COLUMNS]);
esp_err_t control_display_led_begin_virtual(
    const uint8_t segments[CONTROL_DISPLAY_ROWS][CONTROL_DISPLAY_COLUMNS]);
esp_err_t control_display_led_set_virtual_segments(
    const uint8_t segments[CONTROL_DISPLAY_ROWS][CONTROL_DISPLAY_COLUMNS]);
void control_display_led_end_virtual(void);
