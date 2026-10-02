#include "telemetry_heartbeat.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_log.h"

#include "firmware_version.h"
#include "mqtt_manager.h"
#include "pump_transaction_filter.h"
#include "time_manager.h"
#include "wifi_manager.h"

#define TELEMETRY_HEARTBEAT_PERIOD_MS       60000U
#define TELEMETRY_HEARTBEAT_TASK_STACK_SIZE 3072U
#define TELEMETRY_HEARTBEAT_TASK_PRIORITY   3U
#define TELEMETRY_HEARTBEAT_VALUE           1
#define WIFI_RSSI_MIN_DBM                   (-127)
#define WIFI_RSSI_MAX_DBM                   0

static const char *TAG = "TELEMETRY_HEARTBEAT";
static bool s_started;

static esp_err_t publish_heartbeat(void)
{
    const char *firmware_version = firmware_version_get();

    int64_t timestamp = time_manager_get_timestamp();
    if (timestamp <= 0) {
        ESP_LOGW(TAG, "Bo qua heartbeat: thoi gian chua hop le");
        return ESP_ERR_INVALID_STATE;
    }

    int8_t rssi = 0;
    esp_err_t err = wifi_manager_get_rssi(&rssi);
    if (err != ESP_OK || rssi < WIFI_RSSI_MIN_DBM ||
        rssi > WIFI_RSSI_MAX_DBM) {
        ESP_LOGW(TAG, "Bo qua heartbeat: RSSI khong hop le (%s, %d dBm)",
                 esp_err_to_name(err), (int)rssi);
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }

    if (!mqtt_manager_is_ready()) {
        ESP_LOGW(TAG, "Bo qua heartbeat: MQTT chua ready");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t unit_price = 0U;
    err = pump_transaction_filter_get_unit_price(&unit_price);
    if (err != ESP_OK || unit_price == 0U) {
        ESP_LOGW(TAG, "Bo qua heartbeat: unit_price chua hop le (%s)",
                 esp_err_to_name(err));
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    bool data_built =
        cJSON_AddNumberToObject(data, "unit_price", unit_price) != NULL;
    bool root_built = data_built &&
        cJSON_AddNumberToObject(root, "ts", (double)timestamp) != NULL &&
        cJSON_AddStringToObject(root, "version",
                                firmware_version) != NULL &&
        cJSON_AddNumberToObject(root, "keep_alive",
                               TELEMETRY_HEARTBEAT_VALUE) != NULL &&
        cJSON_AddNumberToObject(root, "RSSI", rssi) != NULL;
    bool data_attached = root_built &&
                         cJSON_AddItemToObject(root, "data", data);
    if (!data_attached) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = mqtt_manager_publish_telemetry(payload);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Heartbeat queued: ts=%lld version=%s keep_alive=%d "
                      "RSSI=%d dBm unit_price=%lu",
                 (long long)timestamp, firmware_version,
                 TELEMETRY_HEARTBEAT_VALUE, (int)rssi,
                 (unsigned long)unit_price);
    } else {
        ESP_LOGW(TAG, "Khong queue duoc heartbeat: %s",
                 esp_err_to_name(err));
    }

    cJSON_free(payload);
    return err;
}

static void telemetry_heartbeat_task(void *argument)
{
    (void)argument;

    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(TELEMETRY_HEARTBEAT_PERIOD_MS);

    while (true) {
        vTaskDelayUntil(&last_wake_time, period);
        (void)publish_heartbeat();
    }
}

esp_err_t telemetry_heartbeat_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    BaseType_t result = xTaskCreate(
        telemetry_heartbeat_task,
        "telemetry_heartbeat",
        TELEMETRY_HEARTBEAT_TASK_STACK_SIZE,
        NULL,
        TELEMETRY_HEARTBEAT_TASK_PRIORITY,
        NULL
    );
    if (result != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "Heartbeat telemetry moi %u giay",
             (unsigned)(TELEMETRY_HEARTBEAT_PERIOD_MS / 1000U));
    return ESP_OK;
}
