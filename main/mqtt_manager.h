#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef void (*mqtt_command_callback_t)(const char *payload, size_t length);

esp_err_t mqtt_manager_start(void);
bool mqtt_manager_is_connected(void);
void mqtt_manager_set_command_callback(mqtt_command_callback_t callback);

esp_err_t mqtt_manager_publish_telemetry(const char *json_payload);
esp_err_t mqtt_manager_publish_ack(const char *json_payload);
esp_err_t mqtt_manager_publish_event(const char *json_payload);
