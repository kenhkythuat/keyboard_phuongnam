#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_CONFIG_PORTAL_READY,
    WIFI_CONFIG_PORTAL_CONNECTING,
    WIFI_CONFIG_PORTAL_CONNECTION_FAILED,
    WIFI_CONFIG_PORTAL_STORAGE_FAILED,
} wifi_config_portal_status_t;

typedef esp_err_t (*wifi_config_portal_submit_cb_t)(const char *ssid,
                                                    const char *password,
                                                    void *context);

esp_err_t wifi_config_portal_start(wifi_config_portal_submit_cb_t submit_cb,
                                   void *context);
void wifi_config_portal_stop(void);
bool wifi_config_portal_is_running(void);
void wifi_config_portal_set_status(wifi_config_portal_status_t status);

#ifdef __cplusplus
}
#endif
