#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_manager_start(void);
bool wifi_manager_is_connected(void);
bool wifi_manager_wait_for_connection(uint32_t timeout_ms);
esp_err_t wifi_manager_get_rssi(int8_t *rssi);

#ifdef __cplusplus
}
#endif
