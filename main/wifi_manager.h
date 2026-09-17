#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MANAGER_RESULT_NONE,
    WIFI_MANAGER_RESULT_DONE,
    WIFI_MANAGER_RESULT_FAIL,
} wifi_manager_result_t;

esp_err_t wifi_manager_start(void);
bool wifi_manager_is_connected(void);
bool wifi_manager_is_config_mode(void);
wifi_manager_result_t wifi_manager_get_connection_result(void);
void wifi_manager_acknowledge_connection_result(void);
bool wifi_manager_wait_for_connection(uint32_t timeout_ms);
esp_err_t wifi_manager_get_rssi(int8_t *rssi);

#ifdef __cplusplus
}
#endif
