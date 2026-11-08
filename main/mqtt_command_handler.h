#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define TOTALIZER_LED_COLUMNS 6U
#define BOOT_CALIBRATION_VALUE_MAX_LENGTH 6U

typedef struct {
    uint64_t total_amount_vnd;
    double total_volume_l;
    uint8_t amount_segments[TOTALIZER_LED_COLUMNS];
} totalizer_result_t;

typedef struct {
    bool calibration_observed;
    bool calibration_mismatch;
    char actual_command_value[BOOT_CALIBRATION_VALUE_MAX_LENGTH + 1U];
    bool unit_price_observed;
    uint32_t actual_unit_price;
} boot_calibration_check_t;

esp_err_t mqtt_command_handler_start(void);
esp_err_t read_totalizer(uint64_t *total_amount_vnd,
                         double *total_volume_l);
esp_err_t read_totalizer_result(totalizer_result_t *result);
esp_err_t execute_calibration_command(const char *raw_command);
esp_err_t mqtt_command_handler_check_boot_calibration(
    const char *expected_raw_command,
    boot_calibration_check_t *check);
esp_err_t mqtt_command_handler_execute_local_calibration(
    const char *mode_calibration,
    const char *raw_command);
void mqtt_command_handler_notify_transaction_complete(
    const char *command_code);
