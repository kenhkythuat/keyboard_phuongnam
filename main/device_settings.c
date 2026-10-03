#include "device_settings.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#include "device_config.h"

#define SETTINGS_NAMESPACE       "device_cfg"
#define HASH_KEY_LOCKED_KEY      "hash_lock"
#define PRICE_EDIT_LOCKED_KEY    "price_lock"
#define MODE_CALIBRATION_KEY     "cal_mode"
#define MODE_RESTORE_KEY         "cal_restore"
#define SHORTCUT_MAPPING_KEY     "shortcuts"
#define CALIBRATION_MAPPING_KEY  "calib_map"
#define CORE_CONFIG_KEY          "core_cfg"
#define CORE_CONFIG_MAGIC        0x434F5245UL
#define CORE_CONFIG_VERSION      1U
#define SHORTCUT_STORE_MAGIC     0x53434D50UL
#define SHORTCUT_STORE_VERSION   2U
#define SHORTCUT_STORE_LEGACY_VERSION 1U
#define CALIBRATION_STORE_MAGIC  0x43414C42UL
#define CALIBRATION_STORE_VERSION 2U
#define CALIBRATION_STORE_LEGACY_VERSION 1U
#define CALIBRATION_STORAGE_SLOT_COUNT 10U
#define MODE_RESTORE_MAGIC       0x4D525354UL
#define MODE_RESTORE_VERSION     1U

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

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    device_core_config_t config;
} core_config_store_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    calibration_mode_restore_t restore;
} calibration_mode_restore_store_t;

static const char *TAG = "DEVICE_SETTINGS";
static nvs_handle_t s_nvs_handle;
static bool s_hash_key_locked;
static bool s_price_edit_locked;
static device_core_config_t s_core_config;
static char s_mode_calibration[SHORTCUT_KEY_MAX_LENGTH + 1U];
static bool s_initialized;
static shortcut_store_t s_shortcut_store;
static calibration_store_t s_calibration_store;
static portMUX_TYPE s_shortcut_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_calibration_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_mode_calibration_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_core_config_lock = portMUX_INITIALIZER_UNLOCKED;

_Static_assert(sizeof(NODE_ID) - 1U <= DEVICE_NODE_ID_MAX_LENGTH,
               "Factory NODE_ID is too long");
_Static_assert(sizeof(MQTT_BROKER_HOST) - 1U <=
                   DEVICE_MQTT_HOST_MAX_LENGTH,
               "Factory MQTT host is too long");
_Static_assert(sizeof(MQTT_USERNAME) - 1U <=
                   DEVICE_MQTT_USERNAME_MAX_LENGTH,
               "Factory MQTT username is too long");
_Static_assert(sizeof(MQTT_PASSWORD) - 1U <=
                   DEVICE_MQTT_PASSWORD_MAX_LENGTH,
               "Factory MQTT password is too long");

static bool node_id_is_valid(const char *node_id)
{
    static const char prefix[] = "node_kbd_";
    if (node_id == NULL ||
        strnlen(node_id, DEVICE_NODE_ID_MAX_LENGTH + 1U) !=
            sizeof(prefix) - 1U + 3U ||
        strncmp(node_id, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }

    const char *number = node_id + sizeof(prefix) - 1U;
    if (number[0] < '0' || number[0] > '9' ||
        number[1] < '0' || number[1] > '9' ||
        number[2] < '0' || number[2] > '9') {
        return false;
    }

    uint16_t value = (uint16_t)(((number[0] - '0') * 100) +
                                ((number[1] - '0') * 10) +
                                (number[2] - '0'));
    return value >= 1U && value <= 999U;
}

static bool nonempty_string_is_valid(const char *value, size_t max_length)
{
    if (value == NULL) {
        return false;
    }
    size_t length = strnlen(value, max_length + 1U);
    return length > 0U && length <= max_length;
}

static bool mqtt_host_is_valid(const char *host)
{
    if (!nonempty_string_is_valid(host, DEVICE_MQTT_HOST_MAX_LENGTH)) {
        return false;
    }
    for (size_t index = 0U; host[index] != '\0'; index++) {
        unsigned char character = (unsigned char)host[index];
        if (character <= 0x20U || character == 0x7FU) {
            return false;
        }
    }
    return true;
}

bool device_settings_core_config_is_valid(
    const device_core_config_t *config)
{
    return config != NULL &&
           node_id_is_valid(config->node_id) &&
           mqtt_host_is_valid(config->mqtt_broker_host) &&
           config->mqtt_broker_port > 0U &&
           nonempty_string_is_valid(config->mqtt_username,
                                    DEVICE_MQTT_USERNAME_MAX_LENGTH) &&
           nonempty_string_is_valid(config->mqtt_password,
                                    DEVICE_MQTT_PASSWORD_MAX_LENGTH);
}

static void load_factory_core_config(device_core_config_t *config)
{
    memset(config, 0, sizeof(*config));
    strlcpy(config->node_id, NODE_ID, sizeof(config->node_id));
    strlcpy(config->mqtt_broker_host, MQTT_BROKER_HOST,
            sizeof(config->mqtt_broker_host));
    config->mqtt_broker_port = MQTT_BROKER_PORT;
    strlcpy(config->mqtt_username, MQTT_USERNAME,
            sizeof(config->mqtt_username));
    strlcpy(config->mqtt_password, MQTT_PASSWORD,
            sizeof(config->mqtt_password));
}

static esp_err_t write_core_config_store(const core_config_store_t *store)
{
    esp_err_t err = nvs_set_blob(s_nvs_handle, CORE_CONFIG_KEY,
                                 store, sizeof(*store));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    if (err != ESP_OK) {
        return err;
    }

    core_config_store_t readback = {0};
    size_t size = sizeof(readback);
    err = nvs_get_blob(s_nvs_handle, CORE_CONFIG_KEY, &readback, &size);
    if (err != ESP_OK || size != sizeof(readback) ||
        memcmp(&readback, store, sizeof(readback)) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

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

    core_config_store_t stored_core = {0};
    size_t core_size = sizeof(stored_core);
    err = nvs_get_blob(s_nvs_handle, CORE_CONFIG_KEY,
                       &stored_core, &core_size);
    bool seed_factory_core = err == ESP_ERR_NVS_NOT_FOUND ||
                             err == ESP_ERR_NVS_INVALID_LENGTH ||
                             core_size != sizeof(stored_core) ||
                             stored_core.magic != CORE_CONFIG_MAGIC ||
                             stored_core.version != CORE_CONFIG_VERSION ||
                             !device_settings_core_config_is_valid(
                                 &stored_core.config);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND &&
        err != ESP_ERR_NVS_INVALID_LENGTH) {
        nvs_close(s_nvs_handle);
        return err;
    }
    if (seed_factory_core) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Core config NVS khong hop le, khoi phuc factory seed");
        }
        memset(&stored_core, 0, sizeof(stored_core));
        stored_core.magic = CORE_CONFIG_MAGIC;
        stored_core.version = CORE_CONFIG_VERSION;
        load_factory_core_config(&stored_core.config);
        if (!device_settings_core_config_is_valid(&stored_core.config)) {
            ESP_LOGE(TAG, "Factory core config khong hop le");
            nvs_close(s_nvs_handle);
            return ESP_ERR_INVALID_ARG;
        }
        err = write_core_config_store(&stored_core);
        if (err != ESP_OK) {
            nvs_close(s_nvs_handle);
            return err;
        }
        ESP_LOGI(TAG, "Da seed core config xuat xuong vao NVS cho node=%s",
                 stored_core.config.node_id);
    }

    uint8_t stored_hash_value = 0U;
    err = nvs_get_u8(s_nvs_handle, HASH_KEY_LOCKED_KEY,
                     &stored_hash_value);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        stored_hash_value = 0U;
        err = nvs_set_u8(s_nvs_handle, HASH_KEY_LOCKED_KEY, 0U);
        if (err == ESP_OK) {
            err = nvs_commit(s_nvs_handle);
        }
    } else if (err != ESP_OK) {
        nvs_close(s_nvs_handle);
        return err;
    }
    if (err != ESP_OK) {
        nvs_close(s_nvs_handle);
        return err;
    }

    uint8_t stored_price_value = 0U;
    err = nvs_get_u8(s_nvs_handle, PRICE_EDIT_LOCKED_KEY,
                     &stored_price_value);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        stored_price_value = 0U;
        err = nvs_set_u8(s_nvs_handle, PRICE_EDIT_LOCKED_KEY, 0U);
        if (err == ESP_OK) {
            err = nvs_commit(s_nvs_handle);
        }
    } else if (err != ESP_OK) {
        nvs_close(s_nvs_handle);
        return err;
    }
    if (err != ESP_OK) {
        nvs_close(s_nvs_handle);
        return err;
    }

    char stored_mode[sizeof(s_mode_calibration)] = DEFAULT_MODE_CALIBRATION;
    size_t stored_mode_size = 0U;
    bool persist_default_mode = false;
    err = nvs_get_str(s_nvs_handle, MODE_CALIBRATION_KEY, NULL,
                      &stored_mode_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        persist_default_mode = true;
    } else if (err != ESP_OK) {
        nvs_close(s_nvs_handle);
        return err;
    } else if (stored_mode_size > sizeof(stored_mode)) {
        ESP_LOGW(TAG, "mode_calibration trong NVS qua dai, dung mac dinh %s",
                 DEFAULT_MODE_CALIBRATION);
        persist_default_mode = true;
    } else {
        err = nvs_get_str(s_nvs_handle, MODE_CALIBRATION_KEY, stored_mode,
                          &stored_mode_size);
        if (err != ESP_OK) {
            nvs_close(s_nvs_handle);
            return err;
        }
        if (!shortcut_string_is_valid(stored_mode,
                                      SHORTCUT_KEY_MAX_LENGTH)) {
            ESP_LOGW(TAG, "mode_calibration trong NVS khong hop le, dung mac dinh %s",
                     DEFAULT_MODE_CALIBRATION);
            strlcpy(stored_mode, DEFAULT_MODE_CALIBRATION,
                    sizeof(stored_mode));
            persist_default_mode = true;
        }
    }
    if (persist_default_mode) {
        strlcpy(stored_mode, DEFAULT_MODE_CALIBRATION,
                sizeof(stored_mode));
        err = nvs_set_str(s_nvs_handle, MODE_CALIBRATION_KEY, stored_mode);
        if (err == ESP_OK) {
            err = nvs_commit(s_nvs_handle);
        }
        if (err != ESP_OK) {
            nvs_close(s_nvs_handle);
            return err;
        }
    }

    shortcut_store_t stored_shortcuts;
    bool shortcuts_need_write = false;
    size_t blob_size = sizeof(stored_shortcuts);
    err = nvs_get_blob(s_nvs_handle, SHORTCUT_MAPPING_KEY,
                       &stored_shortcuts, &blob_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        reset_shortcut_store(&stored_shortcuts);
        err = ESP_OK;
        shortcuts_need_write = true;
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
        shortcuts_need_write = true;
        ESP_LOGI(TAG, "Chuyen shortcut NVS cu sang slot 1..%u",
                 (unsigned)stored_shortcuts.count);
    } else if (blob_size != sizeof(stored_shortcuts) ||
               !shortcut_store_is_valid(&stored_shortcuts,
                                        SHORTCUT_STORE_VERSION)) {
        ESP_LOGW(TAG, "Shortcut mapping NVS khong hop le, dung bang rong");
        reset_shortcut_store(&stored_shortcuts);
        err = ESP_OK;
        shortcuts_need_write = true;
    }

    calibration_store_t stored_calibrations;
    bool calibrations_need_write = false;
    blob_size = sizeof(stored_calibrations);
    err = nvs_get_blob(s_nvs_handle, CALIBRATION_MAPPING_KEY,
                       &stored_calibrations, &blob_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        reset_calibration_store(&stored_calibrations);
        err = ESP_OK;
        calibrations_need_write = true;
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
        calibrations_need_write = true;
        ESP_LOGI(TAG, "Chuyen calibration NVS cu sang slot 1..%u",
                 (unsigned)retained);
    } else if (blob_size != sizeof(stored_calibrations) ||
               !calibration_store_is_valid(&stored_calibrations,
                                           CALIBRATION_STORE_VERSION)) {
        ESP_LOGW(TAG, "Calibration mapping NVS khong hop le, dung bang rong");
        reset_calibration_store(&stored_calibrations);
        err = ESP_OK;
        calibrations_need_write = true;
    }

    if (shortcuts_need_write) {
        err = nvs_set_blob(s_nvs_handle, SHORTCUT_MAPPING_KEY,
                           &stored_shortcuts, sizeof(stored_shortcuts));
    }
    if (err == ESP_OK && calibrations_need_write) {
        err = nvs_set_blob(s_nvs_handle, CALIBRATION_MAPPING_KEY,
                           &stored_calibrations,
                           sizeof(stored_calibrations));
    }
    if (err == ESP_OK &&
        (shortcuts_need_write || calibrations_need_write)) {
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

    portENTER_CRITICAL(&s_core_config_lock);
    s_core_config = stored_core.config;
    portEXIT_CRITICAL(&s_core_config_lock);

    __atomic_store_n(&s_hash_key_locked, stored_hash_value != 0U,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_price_edit_locked, stored_price_value != 0U,
                     __ATOMIC_RELEASE);
    portENTER_CRITICAL(&s_mode_calibration_lock);
    strlcpy(s_mode_calibration, stored_mode, sizeof(s_mode_calibration));
    portEXIT_CRITICAL(&s_mode_calibration_lock);
    s_initialized = true;
    ESP_LOGI(TAG, "Khoi phuc core config node=%s broker=%s:%u tu NVS",
             stored_core.config.node_id,
             stored_core.config.mqtt_broker_host,
             (unsigned)stored_core.config.mqtt_broker_port);
    ESP_LOGI(TAG, "Khoi phuc hash_key_locked=%s tu Flash",
             stored_hash_value != 0U ? "true" : "false");
    ESP_LOGI(TAG, "Khoi phuc price_edit_locked=%s tu Flash",
             stored_price_value != 0U ? "true" : "false");
    ESP_LOGI(TAG, "Khoi phuc %u shortcut mapping, revision=%" PRIu32,
             (unsigned)stored_shortcuts.count, stored_shortcuts.revision);
    ESP_LOGI(TAG, "Khoi phuc %u calibration mapping tu Flash",
             (unsigned)stored_calibrations.count);
    ESP_LOGI(TAG, "Khoi phuc mode_calibration=%s tu Flash/mac dinh",
             stored_mode);
    return ESP_OK;
}

esp_err_t device_settings_get_core_config(device_core_config_t *config)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_core_config_lock);
    *config = s_core_config;
    portEXIT_CRITICAL(&s_core_config_lock);
    return ESP_OK;
}

esp_err_t device_settings_set_core_config(
    const device_core_config_t *config)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!device_settings_core_config_is_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }

    device_core_config_t previous;
    portENTER_CRITICAL(&s_core_config_lock);
    previous = s_core_config;
    portEXIT_CRITICAL(&s_core_config_lock);
    if (memcmp(&previous, config, sizeof(previous)) == 0) {
        return ESP_OK;
    }

    core_config_store_t updated = {
        .magic = CORE_CONFIG_MAGIC,
        .version = CORE_CONFIG_VERSION,
        .config = *config,
    };
    esp_err_t err = write_core_config_store(&updated);
    if (err != ESP_OK) {
        core_config_store_t restore = {
            .magic = CORE_CONFIG_MAGIC,
            .version = CORE_CONFIG_VERSION,
            .config = previous,
        };
        (void)write_core_config_store(&restore);
        return err;
    }

    portENTER_CRITICAL(&s_core_config_lock);
    s_core_config = updated.config;
    portEXIT_CRITICAL(&s_core_config_lock);
    ESP_LOGI(TAG, "Da luu core config node=%s broker=%s:%u vao NVS; "
                  "reboot de MQTT ap dung",
             updated.config.node_id,
             updated.config.mqtt_broker_host,
             (unsigned)updated.config.mqtt_broker_port);
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

    uint8_t readback = 0U;
    err = nvs_get_u8(s_nvs_handle, HASH_KEY_LOCKED_KEY, &readback);
    if (err != ESP_OK || readback != (locked ? 1U : 0U)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    __atomic_store_n(&s_hash_key_locked, locked, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "Da luu hash_key_locked=%s vao Flash",
             locked ? "true" : "false");
    return ESP_OK;
}

bool device_settings_is_price_edit_locked(void)
{
    return s_initialized &&
           __atomic_load_n(&s_price_edit_locked, __ATOMIC_ACQUIRE);
}

esp_err_t device_settings_set_price_edit_locked(bool locked)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_set_u8(s_nvs_handle, PRICE_EDIT_LOCKED_KEY,
                               locked ? 1U : 0U);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t readback = 0U;
    err = nvs_get_u8(s_nvs_handle, PRICE_EDIT_LOCKED_KEY, &readback);
    if (err != ESP_OK || readback != (locked ? 1U : 0U)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    __atomic_store_n(&s_price_edit_locked, locked, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "Da luu price_edit_locked=%s vao Flash",
             locked ? "true" : "false");
    return ESP_OK;
}

esp_err_t device_settings_get_mode_calibration(char *mode, size_t size)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode == NULL || size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_mode_calibration_lock);
    size_t required_size = strlen(s_mode_calibration) + 1U;
    if (required_size <= size) {
        memcpy(mode, s_mode_calibration, required_size);
    }
    portEXIT_CRITICAL(&s_mode_calibration_lock);

    return required_size <= size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t device_settings_set_mode_calibration(const char *mode)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!shortcut_string_is_valid(mode, SHORTCUT_KEY_MAX_LENGTH)) {
        return ESP_ERR_INVALID_ARG;
    }

    char previous[sizeof(s_mode_calibration)];
    portENTER_CRITICAL(&s_mode_calibration_lock);
    strlcpy(previous, s_mode_calibration, sizeof(previous));
    portEXIT_CRITICAL(&s_mode_calibration_lock);
    if (strcmp(previous, mode) == 0) {
        return ESP_OK;
    }

    esp_err_t err = nvs_set_str(s_nvs_handle, MODE_CALIBRATION_KEY, mode);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    if (err != ESP_OK) {
        return err;
    }

    char readback[sizeof(s_mode_calibration)];
    size_t readback_size = sizeof(readback);
    err = nvs_get_str(s_nvs_handle, MODE_CALIBRATION_KEY,
                      readback, &readback_size);
    if (err != ESP_OK || strcmp(readback, mode) != 0) {
        (void)nvs_set_str(s_nvs_handle, MODE_CALIBRATION_KEY, previous);
        (void)nvs_commit(s_nvs_handle);
        return ESP_ERR_INVALID_RESPONSE;
    }

    portENTER_CRITICAL(&s_mode_calibration_lock);
    strlcpy(s_mode_calibration, readback, sizeof(s_mode_calibration));
    portEXIT_CRITICAL(&s_mode_calibration_lock);
    ESP_LOGI(TAG, "Da luu mode_calibration=%s vao Flash", readback);
    return ESP_OK;
}

esp_err_t device_settings_set_calibration_mode_restore(
    const calibration_mode_restore_t *restore)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (restore == NULL ||
        !shortcut_string_is_valid(restore->temporary_mode,
                                  SHORTCUT_KEY_MAX_LENGTH) ||
        !shortcut_string_is_valid(restore->previous_mode,
                                  SHORTCUT_KEY_MAX_LENGTH) ||
        strcmp(restore->temporary_mode, restore->previous_mode) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    calibration_mode_restore_store_t stored = {
        .magic = MODE_RESTORE_MAGIC,
        .version = MODE_RESTORE_VERSION,
        .restore = *restore,
    };
    esp_err_t err = nvs_set_blob(s_nvs_handle, MODE_RESTORE_KEY,
                                 &stored, sizeof(stored));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    if (err != ESP_OK) {
        return err;
    }

    calibration_mode_restore_store_t readback = {0};
    size_t size = sizeof(readback);
    err = nvs_get_blob(s_nvs_handle, MODE_RESTORE_KEY, &readback, &size);
    if (err != ESP_OK || size != sizeof(readback) ||
        memcmp(&readback, &stored, sizeof(stored)) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "Da luu mode tam=%s, se khoi phuc mode=%s",
             restore->temporary_mode, restore->previous_mode);
    return ESP_OK;
}

esp_err_t device_settings_get_calibration_mode_restore(
    calibration_mode_restore_t *restore)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (restore == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    calibration_mode_restore_store_t stored = {0};
    size_t size = sizeof(stored);
    esp_err_t err = nvs_get_blob(s_nvs_handle, MODE_RESTORE_KEY,
                                 &stored, &size);
    if (err != ESP_OK) {
        return err;
    }
    if (size != sizeof(stored) || stored.magic != MODE_RESTORE_MAGIC ||
        stored.version != MODE_RESTORE_VERSION ||
        !shortcut_string_is_valid(stored.restore.temporary_mode,
                                  SHORTCUT_KEY_MAX_LENGTH) ||
        !shortcut_string_is_valid(stored.restore.previous_mode,
                                  SHORTCUT_KEY_MAX_LENGTH) ||
        strcmp(stored.restore.temporary_mode,
               stored.restore.previous_mode) == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *restore = stored.restore;
    return ESP_OK;
}

esp_err_t device_settings_clear_calibration_mode_restore(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_erase_key(s_nvs_handle, MODE_RESTORE_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Da xoa trang thai khoi phuc mode tam");
    }
    return err;
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
