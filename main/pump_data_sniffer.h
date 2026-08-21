#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PUMP_DATA_DISPLAY_ROWS      3U
#define PUMP_DATA_DISPLAY_COLUMNS   6U

void pump_data_sniffer_start(void);

bool mbi_sniffer_get_display(
    uint8_t output[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS]);
