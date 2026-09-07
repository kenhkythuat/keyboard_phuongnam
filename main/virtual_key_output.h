#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t virtual_key_output_run_sequence(const char *sequence,
                                          uint32_t interval_ms);
