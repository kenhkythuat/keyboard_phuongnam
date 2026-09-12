#include "device_settings.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#define SETTINGS_NAMESPACE       "device_cfg"
#define HASH_KEY_LOCKED_KEY      "hash_lock"
#define SHORTCUT_MAPPING_KEY     "shortcuts"
#define CALIBRATION_MAPPING_KEY  "calib_map"
#define SHORTCUT_STORE_MAGIC     0x53434D50UL
#define SHORTCUT_STORE_VERSION   2U
#define SHORTCUT_STORE_LEGACY_VERSION 1U
#define CALIBRATION_STORE_MAGIC  0x43414C42UL
#define CALIBRATION_STORE_VERSION 2U
#define CALIBRATION_STORE_LEGACY_VERSION 1U
#define CALIBRATION_STORAGE_SLOT_COUNT 10U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint32_t revision;
    shortcut_mapping_t mappings[SHORTCUT_MAPPING_MAX_COUNT];
} shortcut_store_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    /* Keep the legacy blob size so existing NVS data can be migrated. */
    calibration_mapping_t mappings[CALIBRATION_STORAGE_SLOT_COUNT];
} calibration_store_t;

static const char *TAG = "DEVICE_SETTINGS";
static nvs_handle_t s_nvs_handle;
static bool s_hash_key_locked;
static bool s_initialized;
static shortcut_store_t s_shortcut_store;
static calibration_store_t s_calibration_store;
static portMUX_TYPE s_shortcut_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_calibration_lock = portMUX_INITIALIZER_UNLOCKED;

static void reset_shortcut_store(shortcut_store_t *store)
{
    memset(store, 0, sizeof(*store));
    store->magic = SHORTCUT_STORE_MAGIC;
    store->version = SHORTCUT_STORE_VERSION;
}

static void reset_calibration_store(calibration_store_t *store)
{
    memset(store, 0, sizeof(*store));
    store->magic = CALIBRATION_STORE_MAGIC;
    store->version = CALIBRATION_STORE_VERSION;
}

static bool shortcut_character_is_valid(char character)
{
    return (character >= '0' && character <= '9') ||
           character == '#' || character == '$' ||
           character == 'C' || character == 'E' ||
           character == 'L' || character == 'P' ||
           character == 'T' || character == 'V';
}

static bool shortcut_string_is_valid(const char *value, size_t max_length)
{
    if (value == NULL) {
        return false;
    }

    size_t length = strnlen(value, max_length + 1U);
    if (length == 0U || length > max_length) {
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        if (!shortcut_character_is_valid(value[index])) {
            return false;
        }
    }
    return true;
}

static bool shortcut_entry_is_empty(const shortcut_mapping_t *entry)
{
    return entry->shortcut_key[0] == '\0' && entry->physical_key[0] == '\0';
}

static bool raw_command_is_valid(const char *raw_command)
{
    if (raw_command == NULL) {
        return false;
    }
    size_t length = strnlen(raw_command,
                            CALIBRATION_RAW_COMMAND_MAX_LENGTH + 1U);
    if (length == 0U || length > CALIBRATION_RAW_COMMAND_MAX_LENGTH) {
        return false;
    }
    for (size_t index = 0U; index < length; index++) {
        if (raw_command[index] < '0' || raw_command[index] > '9') {
            return false;
        }
    }
    return true;
}

static bool calibration_entry_is_empty(const calibration_mapping_t *entry)
{
    return entry->name[0] == '\0' && entry->raw_command[0] == '\0';
}

static bool shortcut_store_is_valid(const shortcut_store_t *store,
                                    uint16_t expected_version)
{
    if (store->magic != SHORTCUT_STORE_MAGIC ||
        store->version != expected_version ||
        store->count > SHORTCUT_MAPPING_MAX_COUNT) {
        return false;
    }

    uint16_t occupied = 0U;
    uint16_t limit = expected_version == SHORTCUT_STORE_LEGACY_VERSION
                         ? store->count : SHORTCUT_MAPPING_MAX_COUNT;
    for (uint16_t index = 0U; index < limit; index++) {
        if (shortcut_entry_is_empty(&store->mappings[index])) {
            if (expected_version == SHORTCUT_STORE_LEGACY_VERSION) {
                return false;
            }
            continue;
        }
        if (store->mappings[index].shortcut_key[0] == '\0' ||
            store->mappings[index].physical_key[0] == '\0') {
            return false;
        }
        if (!shortcut_string_is_valid(store->mappings[index].shortcut_key,
                                      SHORTCUT_KEY_MAX_LENGTH) ||
            !shortcut_string_is_valid(store->mappings[index].physical_key,
                                      SHORTCUT_PHYSICAL_KEY_MAX_LENGTH)) {
            return false;
        }
        occupied++;
        for (uint16_t other = index + 1U; other < limit; other++) {
            if (shortcut_entry_is_empty(&store->mappings[other])) {
                continue;
            }
            if (strcmp(store->mappings[index].shortcut_key,
                       store->mappings[other].shortcut_key) == 0) {
                return false;
            }
        }
    }
    return occupied == store->count;
}

static bool calibration_store_is_valid(const calibration_store_t *store,
                                       uint16_t expected_version)
{
    if (store->magic != CALIBRATION_STORE_MAGIC ||
        store->version != expected_version ||
        store->count > (expected_version == CALIBRATION_STORE_LEGACY_VERSION
                            ? CALIBRATION_STORAGE_SLOT_COUNT
                            : CALIBRATION_MAPPING_MAX_COUNT)) {
        return false;
    }

    uint16_t occupied = 0U;
    uint16_t limit = expected_version == CALIBRATION_STORE_LEGACY_VERSION
                         ? store->count : CALIBRATION_MAPPING_MAX_COUNT;
    for (uint16_t index = 0U; index < limit; index++) {
        if (calibration_entry_is_empty(&store->mappings[index])) {
            if (expected_version == CALIBRATION_STORE_LEGACY_VERSION) {
                return false;
            }
            continue;
        }
        if (store->mappings[index].name[0] == '\0' ||
            store->mappings[index].raw_command[0] == '\0') {
            return false;
        }
        if (!shortcut_string_is_valid(store->mappings[index].name,
                                      SHORTCUT_KEY_MAX_LENGTH) ||
            !raw_command_is_valid(store->mappings[index].raw_command)) {
            return false;
        }
        occupied++;
        for (uint16_t other = index + 1U; other < limit; other++) {
            if (calibration_entry_is_empty(&store->mappings[other])) {
                continue;
            }
            if (strcmp(store->mappings[index].name,
                       store->mappings[other].name) == 0) {
                return false;
            }
        }
    }
    if (expected_version == CALIBRATION_STORE_VERSION) {
        for (uint16_t index = CALIBRATION_MAPPING_MAX_COUNT;
             index < CALIBRATION_STORAGE_SLOT_COUNT; index++) {
            if (!calibration_entry_is_empty(&store->mappings[index])) {
                return false;
            }
        }
    }
    return occupied == store->count;
}

esp_err_t device_settings_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t stored_value = 0U;
    err = nvs_get_u8(s_nvs_handle, HASH_KEY_LOCKED_KEY, &stored_value);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        stored_value = 0U;
        err = ESP_OK;
    } else if (err != ESP_OK) {
        nvs_close(s_nvs_handle);
        return err;
    }

    shortcut_store_t stored_shortcuts;
    bool shortcuts_migrated = false;
    size_t blob_size = sizeof(stored_shortcuts);
    err = nvs_get_blob(s_nvs_handle, SHORTCUT_MAPPING_KEY,
                       &stored_shortcuts, &blob_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        reset_shortcut_store(&stored_shortcuts);
        err = ESP_OK;
    } else if (err != ESP_OK) {
        nvs_close(s_nvs_handle);
        return err;
    } else if (blob_size == sizeof(stored_shortcuts) &&
               shortcut_store_is_valid(&stored_shortcuts,
                                       SHORTCUT_STORE_LEGACY_VERSION)) {
        for (uint16_t index = stored_shortcuts.count;
             index < SHORTCUT_MAPPING_MAX_COUNT; index++) {
            memset(&stored_shortcuts.mappings[index], 0,
                   sizeof(stored_shortcuts.mappings[index]));
        }
        stored_shortcuts.version = SHORTCUT_STORE_VERSION;
        shortcuts_migrated = true;
        ESP_LOGI(TAG, "Chuyen shortcut NVS cu sang slot 1..%u",
                 (unsigned)stored_shortcuts.count);
    } else if (blob_size != sizeof(stored_shortcuts) ||
               !shortcut_store_is_valid(&stored_shortcuts,
                                        SHORTCUT_STORE_VERSION)) {
        ESP_LOGW(TAG, "Shortcut mapping NVS khong hop le, dung bang rong");
        reset_shortcut_store(&stored_shortcuts);
        err = ESP_OK;
    }

    calibration_store_t stored_calibrations;
    bool calibrations_migrated = false;
    blob_size = sizeof(stored_calibrations);
    err = nvs_get_blob(s_nvs_handle, CALIBRATION_MAPPING_KEY,
                       &stored_calibrations, &blob_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        reset_calibration_store(&stored_calibrations);
        err = ESP_OK;
    } else if (err != ESP_OK) {
        nvs_close(s_nvs_handle);
        return err;
    } else if (blob_size == sizeof(stored_calibrations) &&
               calibration_store_is_valid(
                   &stored_calibrations,
                   CALIBRATION_STORE_LEGACY_VERSION)) {
        uint16_t retained = stored_calibrations.count;
        if (retained > CALIBRATION_MAPPING_MAX_COUNT) {
            retained = CALIBRATION_MAPPING_MAX_COUNT;
            ESP_LOGW(TAG, "Calibration NVS cu co qua 5 mapping, chi giu 5 slot dau");
        }
        for (uint16_t index = retained;
             index < CALIBRATION_STORAGE_SLOT_COUNT; index++) {
            memset(&stored_calibrations.mappings[index], 0,
                   sizeof(stored_calibrations.mappings[index]));
        }
        stored_calibrations.count = retained;
        stored_calibrations.version = CALIBRATION_STORE_VERSION;
        calibrations_migrated = true;
        ESP_LOGI(TAG, "Chuyen calibration NVS cu sang slot 1..%u",
                 (unsigned)retained);
    } else if (blob_size != sizeof(stored_calibrations) ||
               !calibration_store_is_valid(&stored_calibrations,
                                           CALIBRATION_STORE_VERSION)) {
        ESP_LOGW(TAG, "Calibration mapping NVS khong hop le, dung bang rong");
        reset_calibration_store(&stored_calibrations);
        err = ESP_OK;
    }

    if (shortcuts_migrated) {
        err = nvs_set_blob(s_nvs_handle, SHORTCUT_MAPPING_KEY,
                           &stored_shortcuts, sizeof(stored_shortcuts));
    }
    if (err == ESP_OK && calibrations_migrated) {
        err = nvs_set_blob(s_nvs_handle, CALIBRATION_MAPPING_KEY,
                           &stored_calibrations,
                           sizeof(stored_calibrations));
    }
    if (err == ESP_OK && (shortcuts_migrated || calibrations_migrated)) {
        err = nvs_commit(s_nvs_handle);
    }
    if (err != ESP_OK) {
        nvs_close(s_nvs_handle);
        return err;
    }

    portENTER_CRITICAL(&s_shortcut_lock);
    s_shortcut_store = stored_shortcuts;
    portEXIT_CRITICAL(&s_shortcut_lock);

    portENTER_CRITICAL(&s_calibration_lock);
    s_calibration_store = stored_calibrations;
    portEXIT_CRITICAL(&s_calibration_lock);

    __atomic_store_n(&s_hash_key_locked, stored_value != 0U, __ATOMIC_RELEASE);
    s_initialized = true;
    ESP_LOGI(TAG, "Khoi phuc hash_key_locked=%s tu Flash",
             stored_value != 0U ? "true" : "false");
    ESP_LOGI(TAG, "Khoi phuc %u shortcut mapping, revision=%" PRIu32,
             (unsigned)stored_shortcuts.count, stored_shortcuts.revision);
    ESP_LOGI(TAG, "Khoi phuc %u calibration mapping tu Flash",
             (unsigned)stored_calibrations.count);
    return ESP_OK;
}

bool device_settings_is_hash_key_locked(void)
{
    return s_initialized &&
           __atomic_load_n(&s_hash_key_locked, __ATOMIC_ACQUIRE);
}

esp_err_t device_settings_set_hash_key_locked(bool locked)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_set_u8(s_nvs_handle, HASH_KEY_LOCKED_KEY,
                               locked ? 1U : 0U);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    __atomic_store_n(&s_hash_key_locked, locked, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "Da luu hash_key_locked=%s vao Flash",
             locked ? "true" : "false");
    return ESP_OK;
}

bool device_settings_shortcut_mapping_is_valid(const char *shortcut_key,
                                               const char *physical_key)
{
    return shortcut_string_is_valid(shortcut_key,
                                    SHORTCUT_KEY_MAX_LENGTH) &&
           shortcut_string_is_valid(physical_key,
                                    SHORTCUT_PHYSICAL_KEY_MAX_LENGTH);
}

esp_err_t device_settings_set_shortcut_mapping_at(
    uint8_t slot,
    const char *shortcut_key,
    const char *physical_key,
    uint32_t *revision)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (slot == 0U || slot > SHORTCUT_MAPPING_MAX_COUNT ||
        !device_settings_shortcut_mapping_is_valid(shortcut_key,
                                                   physical_key)) {
        return ESP_ERR_INVALID_ARG;
    }

    shortcut_store_t updated;
    portENTER_CRITICAL(&s_shortcut_lock);
    updated = s_shortcut_store;
    portEXIT_CRITICAL(&s_shortcut_lock);

    uint16_t index = (uint16_t)(slot - 1U);
    for (uint16_t other = 0U; other < SHORTCUT_MAPPING_MAX_COUNT; other++) {
        if (other != index &&
            !shortcut_entry_is_empty(&updated.mappings[other]) &&
            strcmp(updated.mappings[other].shortcut_key, shortcut_key) == 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (!shortcut_entry_is_empty(&updated.mappings[index]) &&
        strcmp(updated.mappings[index].shortcut_key, shortcut_key) == 0 &&
        strcmp(updated.mappings[index].physical_key, physical_key) == 0) {
        if (revision != NULL) {
            *revision = updated.revision;
        }
        return ESP_OK;
    }
    if (shortcut_entry_is_empty(&updated.mappings[index])) {
        updated.count++;
    }

    shortcut_mapping_t *entry = &updated.mappings[index];
    memset(entry, 0, sizeof(*entry));
    strlcpy(entry->shortcut_key, shortcut_key, sizeof(entry->shortcut_key));
    strlcpy(entry->physical_key, physical_key, sizeof(entry->physical_key));
    updated.revision++;
    if (updated.revision == 0U) {
        updated.revision = 1U;
    }

    esp_err_t err = nvs_set_blob(s_nvs_handle, SHORTCUT_MAPPING_KEY,
                                 &updated, sizeof(updated));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    if (err != ESP_OK) {
        return err;
    }

    shortcut_store_t readback;
    size_t blob_size = sizeof(readback);
    err = nvs_get_blob(s_nvs_handle, SHORTCUT_MAPPING_KEY,
                       &readback, &blob_size);
    if (err != ESP_OK || blob_size != sizeof(readback) ||
        memcmp(&readback, &updated, sizeof(updated)) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    portENTER_CRITICAL(&s_shortcut_lock);
    s_shortcut_store = readback;
    portEXIT_CRITICAL(&s_shortcut_lock);

    if (revision != NULL) {
        *revision = readback.revision;
    }
    ESP_LOGI(TAG, "Da luu shortcut slot %u: %s -> %s, revision=%" PRIu32,
             (unsigned)slot, shortcut_key, physical_key, readback.revision);
    return ESP_OK;
}

esp_err_t device_settings_get_shortcut_mapping_at(
    uint8_t slot,
    shortcut_mapping_t *mapping,
    uint32_t *revision)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (slot == 0U || slot > SHORTCUT_MAPPING_MAX_COUNT || mapping == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    shortcut_store_t current;
    portENTER_CRITICAL(&s_shortcut_lock);
    current = s_shortcut_store;
    portEXIT_CRITICAL(&s_shortcut_lock);

    uint16_t index = (uint16_t)(slot - 1U);
    if (shortcut_entry_is_empty(&current.mappings[index])) {
        return ESP_ERR_NOT_FOUND;
    }
    *mapping = current.mappings[index];
    if (revision != NULL) {
        *revision = current.revision;
    }
    return ESP_OK;
}

bool device_settings_find_shortcut_suffix(const char *input,
                                          shortcut_mapping_t *mapping)
{
    if (!s_initialized || input == NULL || mapping == NULL) {
        return false;
    }

    shortcut_store_t current;
    portENTER_CRITICAL(&s_shortcut_lock);
    current = s_shortcut_store;
    portEXIT_CRITICAL(&s_shortcut_lock);

    size_t input_length = strlen(input);
    int best_index = -1;
    size_t best_length = 0U;
    for (uint16_t index = 0U; index < SHORTCUT_MAPPING_MAX_COUNT; index++) {
        if (shortcut_entry_is_empty(&current.mappings[index])) {
            continue;
        }
        size_t shortcut_length = strlen(current.mappings[index].shortcut_key);
        if (shortcut_length <= input_length && shortcut_length > best_length &&
            strcmp(input + input_length - shortcut_length,
                   current.mappings[index].shortcut_key) == 0) {
            best_index = (int)index;
            best_length = shortcut_length;
        }
    }
    if (best_index < 0) {
        return false;
    }
    *mapping = current.mappings[best_index];
    return true;
}

bool device_settings_calibration_mapping_is_valid(const char *name,
                                                   const char *raw_command)
{
    return shortcut_string_is_valid(name, SHORTCUT_KEY_MAX_LENGTH) &&
           raw_command_is_valid(raw_command);
}

esp_err_t device_settings_set_calibration_mapping_at(uint8_t slot,
                                                     const char *name,
                                                     const char *raw_command)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (slot == 0U || slot > CALIBRATION_MAPPING_MAX_COUNT ||
        !device_settings_calibration_mapping_is_valid(name, raw_command)) {
        return ESP_ERR_INVALID_ARG;
    }

    calibration_store_t updated;
    portENTER_CRITICAL(&s_calibration_lock);
    updated = s_calibration_store;
    portEXIT_CRITICAL(&s_calibration_lock);

    uint16_t index = (uint16_t)(slot - 1U);
    for (uint16_t other = 0U; other < CALIBRATION_MAPPING_MAX_COUNT; other++) {
        if (other != index &&
            !calibration_entry_is_empty(&updated.mappings[other]) &&
            strcmp(updated.mappings[other].name, name) == 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (!calibration_entry_is_empty(&updated.mappings[index]) &&
        strcmp(updated.mappings[index].name, name) == 0 &&
        strcmp(updated.mappings[index].raw_command, raw_command) == 0) {
        return ESP_OK;
    }
    if (calibration_entry_is_empty(&updated.mappings[index])) {
        updated.count++;
    }

    calibration_mapping_t *entry = &updated.mappings[index];
    memset(entry, 0, sizeof(*entry));
    strlcpy(entry->name, name, sizeof(entry->name));
    strlcpy(entry->raw_command, raw_command, sizeof(entry->raw_command));

    esp_err_t err = nvs_set_blob(s_nvs_handle, CALIBRATION_MAPPING_KEY,
                                 &updated, sizeof(updated));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    if (err != ESP_OK) {
        return err;
    }

    calibration_store_t readback;
    size_t blob_size = sizeof(readback);
    err = nvs_get_blob(s_nvs_handle, CALIBRATION_MAPPING_KEY,
                       &readback, &blob_size);
    if (err != ESP_OK || blob_size != sizeof(readback) ||
        memcmp(&readback, &updated, sizeof(updated)) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    portENTER_CRITICAL(&s_calibration_lock);
    s_calibration_store = readback;
    portEXIT_CRITICAL(&s_calibration_lock);
    ESP_LOGI(TAG, "Da luu calibration slot %u: %s -> %s",
             (unsigned)slot, name, raw_command);
    return ESP_OK;
}

esp_err_t device_settings_get_calibration_mapping_at(
    uint8_t slot,
    calibration_mapping_t *mapping)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (slot == 0U || slot > CALIBRATION_MAPPING_MAX_COUNT ||
        mapping == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    calibration_store_t current;
    portENTER_CRITICAL(&s_calibration_lock);
    current = s_calibration_store;
    portEXIT_CRITICAL(&s_calibration_lock);

    uint16_t index = (uint16_t)(slot - 1U);
    if (calibration_entry_is_empty(&current.mappings[index])) {
        return ESP_ERR_NOT_FOUND;
    }
    *mapping = current.mappings[index];
    return ESP_OK;
}

bool device_settings_find_calibration_suffix(
    const char *input,
    calibration_mapping_t *mapping)
{
    if (!s_initialized || input == NULL || mapping == NULL) {
        return false;
    }

    calibration_store_t current;
    portENTER_CRITICAL(&s_calibration_lock);
    current = s_calibration_store;
    portEXIT_CRITICAL(&s_calibration_lock);

    size_t input_length = strlen(input);
    int best_index = -1;
    size_t best_length = 0U;
    for (uint16_t index = 0U; index < CALIBRATION_MAPPING_MAX_COUNT; index++) {
        if (calibration_entry_is_empty(&current.mappings[index])) {
            continue;
        }
        size_t name_length = strlen(current.mappings[index].name);
        if (name_length <= input_length && name_length > best_length &&
            strcmp(input + input_length - name_length,
                   current.mappings[index].name) == 0) {
            best_index = (int)index;
            best_length = name_length;
        }
    }
    if (best_index < 0) {
        return false;
    }
    *mapping = current.mappings[best_index];
    return true;
}
