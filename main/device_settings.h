#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SHORTCUT_MAPPING_MAX_COUNT          10U
#define SHORTCUT_KEY_MAX_LENGTH             16U
#define SHORTCUT_PHYSICAL_KEY_MAX_LENGTH    32U

typedef struct {
    char shortcut_key[SHORTCUT_KEY_MAX_LENGTH + 1U];
    char physical_key[SHORTCUT_PHYSICAL_KEY_MAX_LENGTH + 1U];
} shortcut_mapping_t;

esp_err_t device_settings_init(void);
bool device_settings_is_hash_key_locked(void);
esp_err_t device_settings_set_hash_key_locked(bool locked);
bool device_settings_shortcut_mapping_is_valid(const char *shortcut_key,
                                               const char *physical_key);
esp_err_t device_settings_set_shortcut_mapping(
    const char *shortcut_key,
    const char *physical_key,
    uint32_t *revision);
esp_err_t device_settings_get_shortcut_mapping(
    const char *shortcut_key,
    shortcut_mapping_t *mapping,
    uint32_t *revision);
bool device_settings_find_shortcut_suffix(const char *input,
                                          shortcut_mapping_t *mapping);
