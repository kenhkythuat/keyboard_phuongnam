#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t mqtt_command_handler_start(void);
esp_err_t read_totalizer(uint64_t *total_amount_vnd,
                         double *total_volume_l);
esp_err_t execute_calibration_command(const char *raw_command);
