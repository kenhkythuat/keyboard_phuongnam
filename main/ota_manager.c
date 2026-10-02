#include "ota_manager.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "device_config.h"
#include "device_settings.h"
#include "wifi_manager.h"

#define OTA_TASK_STACK_SIZE          10240U
#define OTA_TASK_PRIORITY            5U
#define OTA_REQUEST_QUEUE_LENGTH     1U
#define OTA_HTTP_TIMEOUT_MS          15000U
#define OTA_MANIFEST_MAX_LENGTH      1024U
#define OTA_MANIFEST_LOG_PREVIEW     160U
#define OTA_URL_MAX_LENGTH           384U
#define OTA_SELF_TEST_DELAY_MS       10000U
#define OTA_REBOOT_DELAY_MS          2000U
#define OTA_VERSION_PART_COUNT       4U

typedef struct {
    char data[OTA_MANIFEST_MAX_LENGTH + 1U];
    size_t length;
    bool overflow;
} ota_http_response_t;

typedef struct {
    char version[sizeof(((esp_app_desc_t *)0)->version)];
    char firmware_url[OTA_URL_MAX_LENGTH];
} ota_manifest_t;

static const char *TAG = "OTA_MANAGER";
static QueueHandle_t s_request_queue;
static bool s_started;
static bool s_busy;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    ota_http_response_t *response = event->user_data;
    if (response == NULL ||
        response->length + (size_t)event->data_len >
            OTA_MANIFEST_MAX_LENGTH) {
        if (response != NULL) {
            response->overflow = true;
        }
        return ESP_FAIL;
    }

    memcpy(response->data + response->length, event->data,
           (size_t)event->data_len);
    response->length += (size_t)event->data_len;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static bool parse_version(const char *text,
                          uint32_t parts[OTA_VERSION_PART_COUNT])
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    memset(parts, 0, sizeof(uint32_t) * OTA_VERSION_PART_COUNT);
    size_t part_index = 0U;
    const char *cursor = text;
    while (*cursor != '\0') {
        if (part_index >= OTA_VERSION_PART_COUNT ||
            *cursor < '0' || *cursor > '9') {
            return false;
        }

        uint32_t value = 0U;
        while (*cursor >= '0' && *cursor <= '9') {
            uint32_t digit = (uint32_t)(*cursor - '0');
            if (value > (UINT32_MAX - digit) / 10U) {
                return false;
            }
            value = value * 10U + digit;
            cursor++;
        }
        parts[part_index++] = value;

        if (*cursor == '\0') {
            break;
        }
        if (*cursor != '.' || cursor[1] == '\0') {
            return false;
        }
        cursor++;
    }
    return part_index >= 2U;
}

static int compare_versions(const char *left, const char *right,
                            bool *valid)
{
    uint32_t left_parts[OTA_VERSION_PART_COUNT];
    uint32_t right_parts[OTA_VERSION_PART_COUNT];
    *valid = parse_version(left, left_parts) &&
             parse_version(right, right_parts);
    if (!*valid) {
        return 0;
    }

    for (size_t index = 0U; index < OTA_VERSION_PART_COUNT; index++) {
        if (left_parts[index] < right_parts[index]) {
            return -1;
        }
        if (left_parts[index] > right_parts[index]) {
            return 1;
        }
    }
    return 0;
}

static bool firmware_url_is_allowed(const char *url)
{
    return url != NULL && strcmp(url, OTA_FIRMWARE_URL) == 0;
}

static esp_err_t fetch_manifest(ota_manifest_t *manifest)
{
    ota_http_response_t response = {0};
    esp_http_client_config_t config = {
        .url = OTA_MANIFEST_URL,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .keep_alive_enable = true,
        .max_redirection_count = 5,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Manifest HTTP request failed: err=%s status=%d length=%u",
                 esp_err_to_name(err), status_code, (unsigned)response.length);
        return err;
    }
    if (status_code != 200 || response.overflow || response.length == 0U) {
        ESP_LOGE(TAG, "Manifest HTTP status=%d length=%u overflow=%s",
                 status_code, (unsigned)response.length,
                 response.overflow ? "true" : "false");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *root = cJSON_ParseWithLength(response.data, response.length);
    if (root == NULL || !cJSON_IsObject(root)) {
        size_t preview_length = response.length < OTA_MANIFEST_LOG_PREVIEW
                                    ? response.length
                                    : OTA_MANIFEST_LOG_PREVIEW;
        ESP_LOGE(TAG, "Manifest JSON khong hop le, body=%.*s",
                 (int)preview_length, response.data);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *version =
        cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *firmware_url =
        cJSON_GetObjectItemCaseSensitive(root, "firmware_url");
    if (!cJSON_IsString(version) || version->valuestring == NULL) {
        ESP_LOGE(TAG, "Manifest thieu field version hop le");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strnlen(version->valuestring, sizeof(manifest->version)) >=
        sizeof(manifest->version)) {
        ESP_LOGE(TAG, "Manifest version qua dai");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!cJSON_IsString(firmware_url) || firmware_url->valuestring == NULL) {
        ESP_LOGE(TAG, "Manifest thieu field firmware_url hop le");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!firmware_url_is_allowed(firmware_url->valuestring)) {
        ESP_LOGE(TAG, "Manifest firmware_url khong khop");
        ESP_LOGE(TAG, "  expected: %s", OTA_FIRMWARE_URL);
        ESP_LOGE(TAG, "  received: %s", firmware_url->valuestring);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    strlcpy(manifest->version, version->valuestring,
            sizeof(manifest->version));
    strlcpy(manifest->firmware_url, firmware_url->valuestring,
            sizeof(manifest->firmware_url));
    cJSON_Delete(root);

    uint32_t parsed[OTA_VERSION_PART_COUNT];
    if (!parse_version(manifest->version, parsed)) {
        ESP_LOGE(TAG, "Manifest version khong hop le: %s",
                 manifest->version);
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

static esp_err_t download_and_install(const ota_manifest_t *manifest)
{
    const esp_app_desc_t *running_app = esp_app_get_description();
    const esp_partition_t *update_partition =
        esp_ota_get_next_update_partition(NULL);
    if (running_app == NULL || update_partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Tai OTA firmware: %s", manifest->firmware_url);
    esp_http_client_config_t http_config = {
        .url = manifest->firmware_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .keep_alive_enable = true,
        .max_redirection_count = 5,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        return err;
    }

    esp_app_desc_t new_app = {0};
    err = esp_https_ota_get_img_desc(handle, &new_app);
    if (err != ESP_OK) {
        esp_https_ota_abort(handle);
        return err;
    }
    if (strcmp(new_app.project_name, running_app->project_name) != 0 ||
        strcmp(new_app.version, manifest->version) != 0) {
        ESP_LOGE(TAG, "Image descriptor khong khop: project=%s version=%s",
                 new_app.project_name, new_app.version);
        esp_https_ota_abort(handle);
        return ESP_ERR_INVALID_VERSION;
    }

    ESP_LOGI(TAG, "Dang tai firmware %s vao partition %s",
             manifest->version, update_partition->label);
    int last_reported_kb = -1;
    do {
        err = esp_https_ota_perform(handle);
        int downloaded_kb = esp_https_ota_get_image_len_read(handle) / 1024;
        if (downloaded_kb / 64 != last_reported_kb / 64) {
            last_reported_kb = downloaded_kb;
            ESP_LOGI(TAG, "OTA downloaded=%d KiB", downloaded_kb);
        }
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "OTA firmware %s da ghi va xac minh thanh cong",
             manifest->version);
    return ESP_OK;
}

static esp_err_t perform_update(bool *reboot_required)
{
    *reboot_required = false;
    if (!wifi_manager_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    ota_manifest_t manifest = {0};
    ESP_LOGI(TAG, "Kiem tra OTA manifest: %s", OTA_MANIFEST_URL);
    esp_err_t err = fetch_manifest(&manifest);
    if (err != ESP_OK) {
        return err;
    }

    const char *current_version = ota_manager_get_current_version();
    bool versions_valid = false;
    int comparison = compare_versions(current_version, manifest.version,
                                      &versions_valid);
    if (!versions_valid) {
        ESP_LOGE(TAG, "Version khong hop le: current=%s remote=%s",
                 current_version, manifest.version);
        return ESP_ERR_INVALID_VERSION;
    }

    ESP_LOGI(TAG, "OTA version current=%s remote=%s",
             current_version, manifest.version);
    if (comparison >= 0) {
        ESP_LOGI(TAG, "Firmware hien tai da moi nhat, khong can OTA");
        return ESP_OK;
    }

    err = download_and_install(&manifest);
    if (err == ESP_OK) {
        *reboot_required = true;
    }
    return err;
}

static void validate_pending_app(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running == NULL ||
        esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }

    ESP_LOGW(TAG, "Firmware moi dang PENDING_VERIFY, bat dau self-test");
    vTaskDelay(pdMS_TO_TICKS(OTA_SELF_TEST_DELAY_MS));

    device_core_config_t core_config;
    bool healthy = device_settings_get_core_config(&core_config) == ESP_OK &&
                   device_settings_core_config_is_valid(&core_config);
    memset(&core_config, 0, sizeof(core_config));
    if (healthy) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Self-test OTA thanh cong, firmware da duoc xac nhan");
            return;
        }
        ESP_LOGE(TAG, "Khong the xac nhan firmware OTA: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGE(TAG, "Self-test OTA that bai: core config NVS khong hop le");
    }

    ESP_LOGE(TAG, "Danh dau firmware khong hop le va rollback");
    esp_ota_mark_app_invalid_rollback_and_reboot();
#endif
}

static void ota_task(void *argument)
{
    (void)argument;
    validate_pending_app();

    uint8_t request;
    while (true) {
        if (xQueueReceive(s_request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        __atomic_store_n(&s_busy, true, __ATOMIC_RELEASE);
        bool reboot_required = false;
        esp_err_t err = perform_update(&reboot_required);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA that bai: %s", esp_err_to_name(err));
        }
        __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);

        if (err == ESP_OK && reboot_required) {
            ESP_LOGI(TAG, "Khoi dong lai de chay firmware moi sau %u ms",
                     (unsigned)OTA_REBOOT_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
            esp_restart();
        }
    }
}

esp_err_t ota_manager_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_request_queue = xQueueCreate(OTA_REQUEST_QUEUE_LENGTH, sizeof(uint8_t));
    if (s_request_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(ota_task, "ota_manager", OTA_TASK_STACK_SIZE, NULL,
                    OTA_TASK_PRIORITY, NULL) != pdPASS) {
        vQueueDelete(s_request_queue);
        s_request_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "OTA manager started, current_version=%s",
             ota_manager_get_current_version());
    return ESP_OK;
}

esp_err_t ota_manager_request_update(void)
{
    if (!s_started || s_request_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ota_manager_is_busy() || uxQueueMessagesWaiting(s_request_queue) > 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t request = 1U;
    return xQueueSend(s_request_queue, &request, 0) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

bool ota_manager_is_busy(void)
{
    return __atomic_load_n(&s_busy, __ATOMIC_ACQUIRE);
}

const char *ota_manager_get_current_version(void)
{
    const esp_app_desc_t *description = esp_app_get_description();
    return description != NULL ? description->version : "0.0.0";
}
