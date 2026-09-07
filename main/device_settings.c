#include "device_settings.h"

#include "esp_log.h"
#include "nvs.h"

#define SETTINGS_NAMESPACE       "device_cfg"
#define HASH_KEY_LOCKED_KEY      "hash_lock"

static const char *TAG = "DEVICE_SETTINGS";
static nvs_handle_t s_nvs_handle;
static bool s_hash_key_locked;
static bool s_initialized;

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

    __atomic_store_n(&s_hash_key_locked, stored_value != 0U, __ATOMIC_RELEASE);
    s_initialized = true;
    ESP_LOGI(TAG, "Khoi phuc hash_key_locked=%s tu Flash",
             stored_value != 0U ? "true" : "false");
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
