#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define PUMP_TRANSACTION_DISPLAY_ROWS      3U
#define PUMP_TRANSACTION_DISPLAY_COLUMNS   6U

esp_err_t pump_transaction_filter_start(void);

bool pump_transaction_filter_submit(
    const uint8_t display[PUMP_TRANSACTION_DISPLAY_ROWS]
                         [PUMP_TRANSACTION_DISPLAY_COLUMNS]);
