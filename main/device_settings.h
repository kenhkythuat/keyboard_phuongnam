#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SHORTCUT_MAPPING_MAX_COUNT          10U
#define SHORTCUT_KEY_MAX_LENGTH             16U
#define SHORTCUT_PHYSICAL_KEY_MAX_LENGTH    32U
#define CALIBRATION_MAPPING_MAX_COUNT       5U
#define CALIBRATION_RAW_COMMAND_MAX_LENGTH  6U
#define DEVICE_NODE_ID_MAX_LENGTH           12U
#define DEVICE_MQTT_HOST_MAX_LENGTH         128U
#define DEVICE_MQTT_USERNAME_MAX_LENGTH     64U
#define DEVICE_MQTT_PASSWORD_MAX_LENGTH     128U

typedef struct {
    char node_id[DEVICE_NODE_ID_MAX_LENGTH + 1U];
    char mqtt_broker_host[DEVICE_MQTT_HOST_MAX_LENGTH + 1U];
    uint16_t mqtt_broker_port;
    char mqtt_username[DEVICE_MQTT_USERNAME_MAX_LENGTH + 1U];
    char mqtt_password[DEVICE_MQTT_PASSWORD_MAX_LENGTH + 1U];
} device_core_config_t;

typedef struct {
    char shortcut_key[SHORTCUT_KEY_MAX_LENGTH + 1U];
    char physical_key[SHORTCUT_PHYSICAL_KEY_MAX_LENGTH + 1U];
} shortcut_mapping_t;

typedef struct {
    char name[SHORTCUT_KEY_MAX_LENGTH + 1U];
    char raw_command[CALIBRATION_RAW_COMMAND_MAX_LENGTH + 1U];
} calibration_mapping_t;

esp_err_t device_settings_init(void);
bool device_settings_core_config_is_valid(
    const device_core_config_t *config);
esp_err_t device_settings_get_core_config(device_core_config_t *config);
esp_err_t device_settings_set_core_config(
    const device_core_config_t *config);
bool device_settings_is_hash_key_locked(void);
esp_err_t device_settings_set_hash_key_locked(bool locked);
bool device_settings_is_price_edit_locked(void);
esp_err_t device_settings_set_price_edit_locked(bool locked);
esp_err_t device_settings_get_mode_calibration(char *mode, size_t size);
esp_err_t device_settings_set_mode_calibration(const char *mode);
bool device_settings_shortcut_mapping_is_valid(const char *shortcut_key,
                                               const char *physical_key);
esp_err_t device_settings_set_shortcut_mapping_at(
    uint8_t slot,
    const char *shortcut_key,
    const char *physical_key,
    uint32_t *revision);
esp_err_t device_settings_get_shortcut_mapping_at(
    uint8_t slot,
    shortcut_mapping_t *mapping,
    uint32_t *revision);
bool device_settings_find_shortcut_suffix(const char *input,
                                          shortcut_mapping_t *mapping);
bool device_settings_calibration_mapping_is_valid(const char *name,
                                                   const char *raw_command);
esp_err_t device_settings_set_calibration_mapping_at(
    uint8_t slot,
    const char *name,
    const char *raw_command);
esp_err_t device_settings_get_calibration_mapping_at(
    uint8_t slot,
    calibration_mapping_t *mapping);
bool device_settings_find_calibration_suffix(
    const char *input,
    calibration_mapping_t *mapping);
