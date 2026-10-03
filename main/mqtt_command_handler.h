#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t mqtt_command_handler_start(void);
esp_err_t read_totalizer(uint64_t *total_amount_vnd,
                         double *total_volume_l);
esp_err_t execute_calibration_command(const char *raw_command);
esp_err_t mqtt_command_handler_execute_local_calibration(
    const char *mode_calibration,
    const char *raw_command);
void mqtt_command_handler_notify_transaction_complete(
    const char *command_code);
