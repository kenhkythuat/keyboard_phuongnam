#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t device_settings_init(void);
bool device_settings_is_hash_key_locked(void);
esp_err_t device_settings_set_hash_key_locked(bool locked);
