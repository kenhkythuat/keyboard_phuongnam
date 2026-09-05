#include "pump_transaction_filter.h"

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "time_manager.h"

#define PUMP_DATA_STABLE_TIME_MS   10000U
#define UNIT_PRICE_TOLERANCE_VND   100.0f
#define FILTER_TASK_STACK_SIZE     4096U
#define FILTER_TASK_PRIORITY       5U
#define FILTER_QUEUE_LENGTH        8U

typedef struct {
    uint8_t segments[PUMP_TRANSACTION_DISPLAY_ROWS]
                    [PUMP_TRANSACTION_DISPLAY_COLUMNS];
} pump_display_message_t;

typedef struct {
    uint32_t amount_vnd;
    uint32_t volume_ml;
    uint32_t unit_price;
} pump_transaction_t;

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

static void report_completed_transaction(const pump_transaction_t *transaction)
{
    if (transaction->volume_ml == 0U) {
        ESP_LOGE(TAG, "Khong the kiem tra giao dich: volume_ml=0");
        return;
    }

    float temp_price =
        ((float)transaction->amount_vnd * 1000.0f) /
        (float)transaction->volume_ml;

    float difference = temp_price - (float)transaction->unit_price;
    if (difference < 0.0f) {
        difference = -difference;
    }

    if (difference > UNIT_PRICE_TOLERANCE_VND) {
        ESP_LOGW(
            TAG,
            "DU LIEU BOM KHONG HOP LE: unit_price=%" PRIu32
            " amount_vnd=%" PRIu32 " volume_ml=%" PRIu32
            " temp_price=%.2f sai_so=%.2f (cho phep +/-%.0f)",
            transaction->unit_price,
            transaction->amount_vnd,
            transaction->volume_ml,
            (double)temp_price,
            (double)difference,
            (double)UNIT_PRICE_TOLERANCE_VND
        );
        return;
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
        return;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "GIAO DICH BOM HOP LE - SAN SANG GUI MQTT");
    ESP_LOGI(TAG, "unit_price=%" PRIu32, transaction->unit_price);
    ESP_LOGI(TAG, "amount_vnd=%" PRIu32, transaction->amount_vnd);
    ESP_LOGI(TAG, "volume_ml=%" PRIu32, transaction->volume_ml);
    ESP_LOGI(TAG, "ts=%" PRId64, time_snapshot.ts);
    ESP_LOGI(TAG, "time_device=%s", time_snapshot.time_device);
    ESP_LOGI(
        TAG,
        "temp_price=%.2f sai_so=%.2f (cho phep +/-%.0f)",
        (double)temp_price,
        (double)difference,
        (double)UNIT_PRICE_TOLERANCE_VND
    );
    ESP_LOGI(TAG, "========================================");
}

static void pump_transaction_filter_task(void *argument)
{
    (void)argument;

    pump_display_message_t message;
    pump_transaction_t latest_transaction = {0};
    bool transaction_active = false;
    bool has_sale_data = false;

    while (1) {
        TickType_t wait_ticks =
            transaction_active && has_sale_data
                ? pdMS_TO_TICKS(PUMP_DATA_STABLE_TIME_MS)
                : portMAX_DELAY;

        if (xQueueReceive(s_display_queue, &message, wait_ticks) == pdTRUE) {
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

        ESP_LOGI(
            TAG,
            "Du lieu bom da dung thay doi trong %u ms",
            (unsigned int)PUMP_DATA_STABLE_TIME_MS
        );
        report_completed_transaction(&latest_transaction);

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
