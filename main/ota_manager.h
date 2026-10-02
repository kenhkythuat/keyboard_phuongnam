#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t ota_manager_start(void);
esp_err_t ota_manager_request_update(void);
bool ota_manager_is_busy(void);
const char *ota_manager_get_current_version(void);
