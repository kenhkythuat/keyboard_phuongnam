#include "pump_transaction_filter.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "cJSON.h"

#include "device_config.h"
#include "mqtt_manager.h"
#include "pump_transaction_store.h"
#include "time_manager.h"
#include "wifi_manager.h"

#define PUMP_DATA_STABLE_TIME_MS   10000U
#define AMOUNT_TOLERANCE_VND       200.0f
#define FILTER_TASK_STACK_SIZE     4096U
#define FILTER_TASK_PRIORITY       5U
#define FILTER_QUEUE_LENGTH        8U
#define TELEMETRY_JSON_SIZE        384U
#define PENDING_RETRY_MS           1000U

typedef struct {
    uint8_t segments[PUMP_TRANSACTION_DISPLAY_ROWS]
                    [PUMP_TRANSACTION_DISPLAY_COLUMNS];
} pump_display_message_t;

typedef pump_stored_transaction_t pump_transaction_t;

typedef enum {
    TRANSACTION_REPORT_DONE,
    TRANSACTION_REPORT_RETRY,
} transaction_report_result_t;

static const char *TAG = "PUMP_FILTER";

static QueueHandle_t s_display_queue;
static bool s_started;

static bool segment_to_digit(uint8_t segments, uint8_t *digit, bool *blank)
{
    if (digit == NULL || blank == NULL) {
        return false;
    }

    if (segments == 0xFFU || segments == 0x80U) {
        return false;
    }

    *blank = false;

    switch (segments & 0x7FU) {
        case 0x00:
            *digit = 0;
            *blank = true;
            return true;
        case 0x3F: *digit = 0; return true;
        case 0x06: *digit = 1; return true;
        case 0x5B: *digit = 2; return true;
        case 0x4F: *digit = 3; return true;
        case 0x66: *digit = 4; return true;
        case 0x6D: *digit = 5; return true;
        case 0x7D: *digit = 6; return true;
        case 0x07: *digit = 7; return true;
        case 0x7F: *digit = 8; return true;
        case 0x6F: *digit = 9; return true;
        default:   return false;
    }
}

static bool parse_display_row(
    const uint8_t display[PUMP_TRANSACTION_DISPLAY_ROWS]
                         [PUMP_TRANSACTION_DISPLAY_COLUMNS],
    uint8_t row,
    uint32_t *value)
{
    uint32_t parsed_value = 0;
    bool found_digit = false;

    for (uint8_t column = 0;
         column < PUMP_TRANSACTION_DISPLAY_COLUMNS;
         column++) {

        uint8_t digit = 0;
        bool blank = false;
        uint8_t segments = display[row][column];

        if (!segment_to_digit(segments, &digit, &blank)) {
            ESP_LOGE(
                TAG,
                "Ky tu la tai ROW_%u COL_%u: segment=0x%02X, chi chap nhan 0-9",
                (unsigned int)(row + 1U),
                (unsigned int)(column + 1U),
                segments
            );
            return false;
        }

        if (blank) {
            continue;
        }

        found_digit = true;
        parsed_value = (parsed_value * 10U) + digit;
    }

    if (!found_digit) {
        ESP_LOGE(TAG, "ROW_%u khong co chu so", (unsigned int)(row + 1U));
        return false;
    }

    *value = parsed_value;
    return true;
}

static bool parse_transaction(
    const pump_display_message_t *message,
    pump_transaction_t *transaction)
{
    if (message == NULL || transaction == NULL) {
        return false;
    }

    return parse_display_row(message->segments, 0, &transaction->amount_vnd) &&
           parse_display_row(message->segments, 1, &transaction->volume_ml) &&
           parse_display_row(message->segments, 2, &transaction->unit_price);
}

static bool command_code_is_valid(const char *command_code)
{
    if (command_code == NULL) {
        return false;
    }

    size_t length = strlen(command_code);
    if (length == 0U || length > 16U) {
        return false;
    }

    for (size_t index = 0; index < length; index++) {
        char character = command_code[index];
        bool valid_character =
            (character >= '0' && character <= '9') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            character == '#' || character == '$' ||
            character == '_' || character == '-';

        if (!valid_character) {
            return false;
        }
    }

    return true;
}

static esp_err_t publish_transaction_telemetry(
    const pump_transaction_t *transaction,
    const time_manager_snapshot_t *time_snapshot,
    int8_t rssi,
    bool is_buffered)
{
    if (transaction == NULL || time_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    if (cJSON_AddNumberToObject(root, "ts", (double)time_snapshot->ts) == NULL ||
        cJSON_AddStringToObject(root, "version", "1.3") == NULL) {

        cJSON_Delete(root);
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    if (!cJSON_AddItemToObject(root, "data", data)) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    if (cJSON_AddNumberToObject(data, "unit_price", transaction->unit_price) == NULL ||
        cJSON_AddNumberToObject(data, "amount_vnd", transaction->amount_vnd) == NULL ||
        cJSON_AddNumberToObject(data, "volume_ml", transaction->volume_ml) == NULL ||
        cJSON_AddStringToObject(data, "command_code", TRANSACTION_COMMAND_CODE) == NULL ||
        cJSON_AddStringToObject(data, "time_device", time_snapshot->time_device) == NULL ||
        cJSON_AddBoolToObject(data, "is_buffered", is_buffered) == NULL ||
        cJSON_AddNumberToObject(data, "RSSI", rssi) == NULL) {

        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char json_payload[TELEMETRY_JSON_SIZE];
    if (!cJSON_PrintPreallocated(
            root,
            json_payload,
            sizeof(json_payload),
            false)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON_Delete(root);
    return mqtt_manager_publish_telemetry(json_payload);
}

static transaction_report_result_t report_completed_transaction(
    const pump_transaction_t *transaction,
    bool is_buffered)
{
    if (transaction == NULL ||
        transaction->unit_price == 0U ||
        transaction->amount_vnd == 0U ||
        transaction->volume_ml == 0U) {
        ESP_LOGE(TAG, "Du lieu giao dich khong hop le hoac bang 0");
        return TRANSACTION_REPORT_DONE;
    }

    float expected_amount_vnd =
        ((float)transaction->unit_price *
         (float)transaction->volume_ml) /
        1000.0f;

    float difference_vnd = fabsf(
        (float)transaction->amount_vnd - expected_amount_vnd
    );

    if (difference_vnd > AMOUNT_TOLERANCE_VND) {
        ESP_LOGW(
            TAG,
            "DU LIEU BOM KHONG HOP LE: unit_price=%" PRIu32
            " amount_vnd=%" PRIu32 " volume_ml=%" PRIu32
            " expected_amount_vnd=%.2f sai_so_vnd=%.2f (cho phep +/-%.0f)",
            transaction->unit_price,
            transaction->amount_vnd,
            transaction->volume_ml,
            (double)expected_amount_vnd,
            (double)difference_vnd,
            (double)AMOUNT_TOLERANCE_VND
        );
        return TRANSACTION_REPORT_DONE;
    }

    if (!command_code_is_valid(TRANSACTION_COMMAND_CODE)) {
        ESP_LOGE(TAG, "command_code khong hop le");
        return TRANSACTION_REPORT_DONE;
    }

    time_manager_snapshot_t time_snapshot;
    esp_err_t time_result = time_manager_get_snapshot(&time_snapshot);
    if (time_result != ESP_OK) {
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "GIAO DICH BOM HOP LE - THOI GIAN CHUA DONG BO");
        ESP_LOGI(TAG, "unit_price=%" PRIu32, transaction->unit_price);
        ESP_LOGI(TAG, "amount_vnd=%" PRIu32, transaction->amount_vnd);
        ESP_LOGI(TAG, "volume_ml=%" PRIu32, transaction->volume_ml);
        ESP_LOGE(
            TAG,
            "Khong tao ts/time_device: %s",
            esp_err_to_name(time_result)
        );
        ESP_LOGI(TAG, "========================================");
        return TRANSACTION_REPORT_RETRY;
    }

    int8_t rssi;
    esp_err_t rssi_result = wifi_manager_get_rssi(&rssi);
    if (rssi_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Giao dich hop le nhung khong lay duoc RSSI: %s",
            esp_err_to_name(rssi_result)
        );
        return TRANSACTION_REPORT_RETRY;
    }

    if (!mqtt_manager_is_ready()) {
        ESP_LOGW(TAG, "Giao dich hop le nhung MQTT chua ready, khong publish");
        return TRANSACTION_REPORT_RETRY;
    }

    esp_err_t publish_result = publish_transaction_telemetry(
        transaction,
        &time_snapshot,
        rssi,
        is_buffered
    );
    if (publish_result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Publish MQTT telemetry that bai: %s",
            esp_err_to_name(publish_result)
        );
        return TRANSACTION_REPORT_RETRY;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "GIAO DICH BOM HOP LE - DA QUEUE MQTT TELEMETRY");
    ESP_LOGI(TAG, "unit_price=%" PRIu32, transaction->unit_price);
    ESP_LOGI(TAG, "amount_vnd=%" PRIu32, transaction->amount_vnd);
    ESP_LOGI(TAG, "volume_ml=%" PRIu32, transaction->volume_ml);
    ESP_LOGI(TAG, "ts=%" PRId64, time_snapshot.ts);
    ESP_LOGI(TAG, "time_device=%s", time_snapshot.time_device);
    ESP_LOGI(TAG, "command_code=%s", TRANSACTION_COMMAND_CODE);
    ESP_LOGI(TAG, "is_buffered=%s", is_buffered ? "true" : "false");
    ESP_LOGI(TAG, "RSSI=%d dBm", (int)rssi);
    ESP_LOGI(
        TAG,
        "expected_amount_vnd=%.2f sai_so_vnd=%.2f (cho phep +/-%.0f)",
        (double)expected_amount_vnd,
        (double)difference_vnd,
        (double)AMOUNT_TOLERANCE_VND
    );
    ESP_LOGI(TAG, "========================================");
    return TRANSACTION_REPORT_DONE;
}

static void pump_transaction_filter_task(void *argument)
{
    (void)argument;

    pump_display_message_t message;
    pump_transaction_t latest_transaction = {0};
    bool transaction_active = false;
    bool has_sale_data = false;
    TickType_t last_display_tick = 0;
    const TickType_t stable_ticks = pdMS_TO_TICKS(PUMP_DATA_STABLE_TIME_MS);
    const TickType_t pending_retry_ticks = pdMS_TO_TICKS(PENDING_RETRY_MS);

    while (1) {
        while (pump_transaction_store_count() > 0U &&
               time_manager_is_valid()) {
            pump_transaction_t buffered_transaction;
            esp_err_t store_result = pump_transaction_store_peek(
                &buffered_transaction);
            if (store_result != ESP_OK) {
                ESP_LOGE(TAG, "Khong doc duoc giao dich Flash: %s",
                         esp_err_to_name(store_result));
                break;
            }

            if (report_completed_transaction(&buffered_transaction, true) !=
                TRANSACTION_REPORT_DONE) {
                break;
            }

            store_result = pump_transaction_store_pop();
            if (store_result != ESP_OK) {
                ESP_LOGE(TAG, "Khong xoa duoc giao dich da gui khoi Flash: %s",
                         esp_err_to_name(store_result));
                break;
            }

            ESP_LOGI(TAG, "Da gui giao dich Flash, con lai=%u",
                     (unsigned)pump_transaction_store_count());
        }

        TickType_t wait_ticks = portMAX_DELAY;
        if (transaction_active && has_sale_data) {
            TickType_t elapsed = xTaskGetTickCount() - last_display_tick;
            wait_ticks = elapsed >= stable_ticks ? 0 : stable_ticks - elapsed;
        }

        if (pump_transaction_store_count() > 0U &&
            (wait_ticks == portMAX_DELAY || pending_retry_ticks < wait_ticks)) {
            wait_ticks = pending_retry_ticks;
        }

        if (xQueueReceive(s_display_queue, &message, wait_ticks) == pdTRUE) {
            last_display_tick = xTaskGetTickCount();
            pump_transaction_t transaction = {0};

            if (!parse_transaction(&message, &transaction)) {
                if (transaction_active) {
                    has_sale_data = false;
                }
                continue;
            }

            bool is_transaction_start =
                transaction.amount_vnd == 0U &&
                transaction.volume_ml == 0U &&
                transaction.unit_price > 0U;

            if (is_transaction_start) {
                transaction_active = true;
                has_sale_data = false;
                latest_transaction = transaction;

                ESP_LOGI(
                    TAG,
                    "Bat dau luot bom, unit_price=%" PRIu32,
                    transaction.unit_price
                );
                continue;
            }

            if (!transaction_active) {
                continue;
            }

            latest_transaction = transaction;
            has_sale_data =
                transaction.amount_vnd > 0U &&
                transaction.volume_ml > 0U;
            continue;
        }

        if (!transaction_active || !has_sale_data ||
            (xTaskGetTickCount() - last_display_tick) < stable_ticks) {
            continue;
        }

        ESP_LOGI(
            TAG,
            "Du lieu bom da dung thay doi trong %u ms",
            (unsigned int)PUMP_DATA_STABLE_TIME_MS
        );
        transaction_report_result_t report_result =
            report_completed_transaction(&latest_transaction, false);
        if (report_result == TRANSACTION_REPORT_RETRY) {
            esp_err_t store_result = pump_transaction_store_append(
                &latest_transaction);
            if (store_result == ESP_OK) {
                ESP_LOGI(
                    TAG,
                    "Da luu giao dich vao Flash, dang cho gui=%u",
                    (unsigned)pump_transaction_store_count()
                );
            } else {
                ESP_LOGE(TAG, "Khong luu duoc giao dich vao Flash: %s",
                         esp_err_to_name(store_result));
            }
        }

        transaction_active = false;
        has_sale_data = false;
        memset(&latest_transaction, 0, sizeof(latest_transaction));
    }
}

esp_err_t pump_transaction_filter_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t store_result = pump_transaction_store_init();
    if (store_result != ESP_OK) {
        return store_result;
    }

    s_display_queue = xQueueCreate(
        FILTER_QUEUE_LENGTH,
        sizeof(pump_display_message_t)
    );
    if (s_display_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_result = xTaskCreate(
        pump_transaction_filter_task,
        "pump_filter",
        FILTER_TASK_STACK_SIZE,
        NULL,
        FILTER_TASK_PRIORITY,
        NULL
    );

    if (task_result != pdPASS) {
        vQueueDelete(s_display_queue);
        s_display_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(
        TAG,
        "Pump transaction filter enabled, stable_time=%u ms",
        (unsigned int)PUMP_DATA_STABLE_TIME_MS
    );
    return ESP_OK;
}

bool pump_transaction_filter_submit(
    const uint8_t display[PUMP_TRANSACTION_DISPLAY_ROWS]
                         [PUMP_TRANSACTION_DISPLAY_COLUMNS])
{
    if (!s_started || s_display_queue == NULL || display == NULL) {
        return false;
    }

    pump_display_message_t message;
    memcpy(message.segments, display, sizeof(message.segments));

    return xQueueSend(s_display_queue, &message, 0) == pdPASS;
}
