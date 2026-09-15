#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define PUMP_TRANSACTION_COMMAND_CODE_MAX_LENGTH 16U

typedef struct {
    uint32_t amount_vnd;
    uint32_t volume_ml;
    uint32_t unit_price;
    char command_code[PUMP_TRANSACTION_COMMAND_CODE_MAX_LENGTH + 1U];
} pump_stored_transaction_t;

esp_err_t pump_transaction_store_init(void);
esp_err_t pump_transaction_store_append(
    const pump_stored_transaction_t *transaction);
esp_err_t pump_transaction_store_peek(pump_stored_transaction_t *transaction);
esp_err_t pump_transaction_store_pop(void);
size_t pump_transaction_store_count(void);
