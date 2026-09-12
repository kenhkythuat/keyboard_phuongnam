#include "mqtt_command_handler.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_log.h"

#include "device_settings.h"
#include "mqtt_manager.h"
#include "pump_data_sniffer.h"
#include "time_manager.h"
#include "virtual_key_output.h"

#define COMMAND_PAYLOAD_MAX_LENGTH  768U
#define COMMAND_QUEUE_LENGTH        8U
#define COMMAND_TASK_STACK_SIZE     5120U
#define COMMAND_TASK_PRIORITY       5U
#define REQUEST_ID_MAX_LENGTH       64U
#define COMMAND_NAME_MAX_LENGTH     48U
#define DEDUP_HISTORY_LENGTH        16U
#define VIRTUAL_KEY_INTERVAL_MS     500U
#define SET_UNIT_PRICE_MAX          999999U
#define SET_UNIT_PRICE_SEQUENCE_MAX_LENGTH 24U
#define SET_UNIT_PRICE_MAX_ATTEMPTS  3U
#define SET_UNIT_PRICE_VERIFY_TIMEOUT_MS 3000U
#define SET_UNIT_PRICE_VERIFY_POLL_MS 50U
#define TOTALIZER_MAX_ATTEMPTS      3U
#define TOTALIZER_VERIFY_TIMEOUT_MS 3000U
#define TOTALIZER_DISPLAY_SETTLE_MS 1000U
#define TOTALIZER_TELEMETRY_JSON_SIZE 256U
#define CALIBRATION_ENTRY_SEQUENCE "C#122973E"
#define CALIBRATION_MAX_ATTEMPTS   3U
#define CALIBRATION_RAW_COMMAND_DELAY_MS 1000U

typedef struct {
    size_t length;
    char payload[COMMAND_PAYLOAD_MAX_LENGTH + 1U];
} command_message_t;

static const char *TAG = "MQTT_COMMAND";
static QueueHandle_t s_command_queue;
static SemaphoreHandle_t s_totalizer_mutex;
static bool s_started;
static bool s_price_edit_locked;
static char s_request_history[DEDUP_HISTORY_LENGTH][REQUEST_ID_MAX_LENGTH + 1U];
static size_t s_request_history_count;
static size_t s_request_history_next;

static esp_err_t reset_total_amount(void);
static esp_err_t reset_total_volume(void);

static bool string_field_is_valid(const cJSON *item, size_t max_length)
{
    return cJSON_IsString(item) && item->valuestring != NULL &&
           item->valuestring[0] != '\0' &&
           strlen(item->valuestring) <= max_length;
}

static bool get_indexed_param_pair(const cJSON *param,
                                   const char *first_base,
                                   const char *second_base,
                                   uint8_t max_slot,
                                   const cJSON **first,
                                   const cJSON **second,
                                   uint8_t *slot)
{
    if (!cJSON_IsObject(param) || cJSON_GetArraySize(param) != 2 ||
        first == NULL || second == NULL || slot == NULL) {
        return false;
    }

    char first_name[32];
    char second_name[32];
    for (uint8_t candidate = 1U; candidate <= max_slot; candidate++) {
        int first_length = snprintf(first_name, sizeof(first_name), "%s_%u",
                                    first_base, (unsigned)candidate);
        int second_length = snprintf(second_name, sizeof(second_name), "%s_%u",
                                     second_base, (unsigned)candidate);
        if (first_length <= 0 || first_length >= (int)sizeof(first_name) ||
            second_length <= 0 || second_length >= (int)sizeof(second_name)) {
            return false;
        }

        const cJSON *first_item =
            cJSON_GetObjectItemCaseSensitive(param, first_name);
        const cJSON *second_item =
            cJSON_GetObjectItemCaseSensitive(param, second_name);
        if (first_item != NULL || second_item != NULL) {
            if (first_item == NULL || second_item == NULL) {
                return false;
            }
            *first = first_item;
            *second = second_item;
            *slot = candidate;
            return true;
        }
    }
    return false;
}

static bool request_was_processed(const char *request_id)
{
    for (size_t index = 0; index < s_request_history_count; index++) {
        if (strcmp(s_request_history[index], request_id) == 0) {
            return true;
        }
    }
    return false;
}

static void remember_request(const char *request_id)
{
    strlcpy(s_request_history[s_request_history_next], request_id,
            sizeof(s_request_history[s_request_history_next]));
    s_request_history_next =
        (s_request_history_next + 1U) % DEDUP_HISTORY_LENGTH;
    if (s_request_history_count < DEDUP_HISTORY_LENGTH) {
        s_request_history_count++;
    }
}

static bool command_name_is_declared(const char *command)
{
    static const char *const commands[] = {
        "set_hash_key_lock",
        "set_unit_price",
        "set_price_edit_lock",
        "get_totalizer",
        "close_shift",
        "set_reset_shortcut",
        "reset_totalizer",
        "send_calibration_command",
        "set_shortcut_mapping",
        "delete_shortcut_mapping",
        "get_reported_state",
    };

    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); index++) {
        if (strcmp(command, commands[index]) == 0) {
            return true;
        }
    }
    return false;
}

static bool segment_to_digit(uint8_t segments, uint8_t *digit, bool *blank)
{
    *blank = false;
    switch (segments & 0x7FU) {
        case 0x00: *digit = 0U; *blank = true; return true;
        case 0x3F: *digit = 0U; return true;
        case 0x06: *digit = 1U; return true;
        case 0x5B: *digit = 2U; return true;
        case 0x4F: *digit = 3U; return true;
        case 0x66: *digit = 4U; return true;
        case 0x6D: *digit = 5U; return true;
        case 0x7D: *digit = 6U; return true;
        case 0x07: *digit = 7U; return true;
        case 0x7F: *digit = 8U; return true;
        case 0x6F: *digit = 9U; return true;
        default: return false;
    }
}

static bool display_has_e0(
    const uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS])
{
    for (size_t column = 0; column < 4U; column++) {
        if ((display[0][column] & 0x7FU) != 0x00U) {
            return false;
        }
    }
    return (display[0][4] & 0x7FU) == 0x79U &&
           (display[0][5] & 0x7FU) == 0x3FU;
}

static bool display_confirms_unit_price(
    const uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS],
    uint32_t expected_unit_price)
{
    if (!display_has_e0(display)) {
        return false;
    }

    uint32_t decoded_price = 0U;
    bool found_digit = false;
    for (size_t column = 0; column < PUMP_DATA_DISPLAY_COLUMNS; column++) {
        uint8_t digit;
        bool blank;
        if (!segment_to_digit(display[2][column], &digit, &blank)) {
            return false;
        }
        if (!blank) {
            found_digit = true;
            decoded_price = decoded_price * 10U + digit;
        }
    }
    return found_digit && decoded_price == expected_unit_price;
}

static bool wait_for_unit_price_confirmation(uint32_t expected_unit_price,
                                             uint32_t baseline_generation)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(SET_UNIT_PRICE_VERIFY_TIMEOUT_MS);

    while ((xTaskGetTickCount() - start) < timeout) {
        uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS];
        uint32_t generation = 0U;
        if (mbi_sniffer_get_display_snapshot(display, &generation) &&
            generation != baseline_generation &&
            display_confirms_unit_price(display, expected_unit_price)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SET_UNIT_PRICE_VERIFY_POLL_MS));
    }
    return false;
}

static bool decode_unsigned_row(const uint8_t row[PUMP_DATA_DISPLAY_COLUMNS],
                                uint64_t *value)
{
    uint64_t decoded = 0U;
    bool found_digit = false;
    for (size_t column = 0; column < PUMP_DATA_DISPLAY_COLUMNS; column++) {
        uint8_t digit;
        bool blank;
        if (!segment_to_digit(row[column], &digit, &blank)) {
            return false;
        }
        if (!blank) {
            found_digit = true;
            decoded = decoded * 10U + digit;
        }
    }
    if (!found_digit) {
        return false;
    }
    *value = decoded;
    return true;
}

static bool decode_total_amount_display(
    const uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS],
    uint64_t *total_amount_vnd)
{
    for (size_t column = 0; column < 3U; column++) {
        if ((display[0][column] & 0x7FU) != 0x00U) {
            return false;
        }
    }
    if ((display[0][3] & 0x7FU) != 0x73U ||
        (display[0][4] & 0x7FU) != 0x5BU ||
        (display[0][5] & 0x7FU) != 0x3FU) {
        return false;
    }
    return decode_unsigned_row(display[1], total_amount_vnd);
}

static bool decode_reset_total_volume_display(
    const uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS],
    uint64_t *displayed_total_volume)
{
    for (size_t column = 0; column < 3U; column++) {
        if ((display[0][column] & 0x7FU) != 0x00U) {
            return false;
        }
    }
    if ((display[0][3] & 0x7FU) != 0x73U ||
        (display[0][4] & 0x7FU) != 0x3FU ||
        (display[0][5] & 0x7FU) != 0x7FU) {
        return false;
    }
    return decode_unsigned_row(display[1], displayed_total_volume);
}

static bool decode_total_volume_display(
    const uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS],
    double *total_volume_l)
{
    uint64_t integer_part;
    if (!decode_unsigned_row(display[0], &integer_part)) {
        return false;
    }

    bool decimal_marker_found = false;
    bool fractional_digit_found = false;
    uint64_t fractional_part = 0U;
    uint64_t fractional_scale = 1U;

    for (size_t column = 0; column < PUMP_DATA_DISPLAY_COLUMNS; column++) {
        uint8_t segments = display[1][column];
        if ((segments & 0x80U) != 0U) {
            if (decimal_marker_found) {
                break;
            }
            decimal_marker_found = true;
        }

        if (!decimal_marker_found) {
            continue;
        }

        uint8_t digit;
        bool blank;
        if (!segment_to_digit(segments, &digit, &blank)) {
            return false;
        }
        if (!blank) {
            fractional_digit_found = true;
            fractional_part = fractional_part * 10U + digit;
            fractional_scale *= 10U;
        }
    }

    if (!decimal_marker_found || !fractional_digit_found) {
        return false;
    }

    *total_volume_l = (double)integer_part +
                      ((double)fractional_part / (double)fractional_scale);
    return true;
}

static bool wait_for_total_amount(uint32_t baseline_generation,
                                  uint64_t *total_amount_vnd)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(TOTALIZER_VERIFY_TIMEOUT_MS);
    while ((xTaskGetTickCount() - start) < timeout) {
        uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS];
        uint32_t generation = 0U;
        if (mbi_sniffer_get_display_snapshot(display, &generation) &&
            generation != baseline_generation &&
            decode_total_amount_display(display, total_amount_vnd)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SET_UNIT_PRICE_VERIFY_POLL_MS));
    }
    return false;
}

static bool wait_for_total_volume(uint32_t baseline_generation,
                                  double *total_volume_l)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(TOTALIZER_VERIFY_TIMEOUT_MS);
    while ((xTaskGetTickCount() - start) < timeout) {
        uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS];
        uint32_t generation = 0U;
        if (mbi_sniffer_get_display_snapshot(display, &generation) &&
            generation != baseline_generation &&
            decode_total_volume_display(display, total_volume_l)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SET_UNIT_PRICE_VERIFY_POLL_MS));
    }
    return false;
}

static bool wait_for_e0(uint32_t baseline_generation)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(TOTALIZER_VERIFY_TIMEOUT_MS);
    while ((xTaskGetTickCount() - start) < timeout) {
        uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS];
        uint32_t generation = 0U;
        if (mbi_sniffer_get_display_snapshot(display, &generation) &&
            generation != baseline_generation &&
            display_has_e0(display)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SET_UNIT_PRICE_VERIFY_POLL_MS));
    }
    return false;
}

static bool display_has_p06(
    const uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS])
{
    for (size_t column = 0U; column < 3U; column++) {
        if ((display[0][column] & 0x7FU) != 0x00U) {
            return false;
        }
    }
    return (display[0][3] & 0x7FU) == 0x73U &&
           (display[0][4] & 0x7FU) == 0x3FU &&
           (display[0][5] & 0x7FU) == 0x7DU;
}

static bool display_row_matches_raw_command(
    const uint8_t row[PUMP_DATA_DISPLAY_COLUMNS],
    const char *raw_command)
{
    size_t length = strlen(raw_command);
    if (length == 0U || length > PUMP_DATA_DISPLAY_COLUMNS) {
        return false;
    }

    size_t first_digit = PUMP_DATA_DISPLAY_COLUMNS - length;
    for (size_t column = 0U; column < PUMP_DATA_DISPLAY_COLUMNS; column++) {
        if ((row[column] & 0x80U) != 0U) {
            return false;
        }
        if (column < first_digit) {
            if ((row[column] & 0x7FU) != 0x00U) {
                return false;
            }
            continue;
        }

        uint8_t digit;
        bool blank;
        if (!segment_to_digit(row[column], &digit, &blank) || blank ||
            digit != (uint8_t)(raw_command[column - first_digit] - '0')) {
            return false;
        }
    }
    return true;
}

static bool wait_for_calibration_display(uint32_t baseline_generation,
                                         const char *raw_command,
                                         bool require_p06,
                                         bool require_e0)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(TOTALIZER_VERIFY_TIMEOUT_MS);
    while ((xTaskGetTickCount() - start) < timeout) {
        uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS];
        uint32_t generation = 0U;
        if (mbi_sniffer_get_display_snapshot(display, &generation) &&
            generation != baseline_generation &&
            (!require_p06 || display_has_p06(display)) &&
            (!require_e0 || display_has_e0(display)) &&
            (raw_command == NULL ||
             display_row_matches_raw_command(display[1], raw_command))) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SET_UNIT_PRICE_VERIFY_POLL_MS));
    }
    return false;
}

static bool wait_for_reset_total_volume_screen(uint32_t baseline_generation)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(TOTALIZER_VERIFY_TIMEOUT_MS);
    while ((xTaskGetTickCount() - start) < timeout) {
        uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS];
        uint32_t generation = 0U;
        uint64_t displayed_total_volume = 0U;
        if (mbi_sniffer_get_display_snapshot(display, &generation) &&
            generation != baseline_generation &&
            decode_reset_total_volume_display(display,
                                              &displayed_total_volume)) {
            ESP_LOGI(TAG, "Da vao P08, tong lit hien thi=%" PRIu64,
                     displayed_total_volume);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SET_UNIT_PRICE_VERIFY_POLL_MS));
    }
    return false;
}

static bool wait_for_display_row_zero(uint32_t baseline_generation,
                                      size_t row)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(TOTALIZER_VERIFY_TIMEOUT_MS);
    while ((xTaskGetTickCount() - start) < timeout) {
        uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS];
        uint32_t generation = 0U;
        uint64_t value = 0U;
        if (mbi_sniffer_get_display_snapshot(display, &generation) &&
            generation != baseline_generation &&
            row < PUMP_DATA_DISPLAY_ROWS &&
            decode_unsigned_row(display[row], &value) && value == 0U) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SET_UNIT_PRICE_VERIFY_POLL_MS));
    }
    return false;
}

static uint32_t get_display_generation(void)
{
    uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS];
    uint32_t generation = 0U;
    (void)mbi_sniffer_get_display_snapshot(display, &generation);
    return generation;
}

static esp_err_t execute_calibration_locked(const char *raw_command)
{
    for (uint32_t attempt = 1U; attempt <= CALIBRATION_MAX_ATTEMPTS;
         attempt++) {
        ESP_LOGI(TAG, "Calibration %s lan %u/%u", raw_command,
                 (unsigned)attempt, (unsigned)CALIBRATION_MAX_ATTEMPTS);

        uint32_t baseline_generation = get_display_generation();
        esp_err_t err = virtual_key_output_run_sequence(
            CALIBRATION_ENTRY_SEQUENCE, VIRTUAL_KEY_INTERVAL_MS);
        if (err == ESP_OK &&
            wait_for_calibration_display(baseline_generation, NULL,
                                         true, false)) {
            ESP_LOGI(TAG, "Calibration da xac nhan man hinh P06, cho %u ms "
                          "truoc khi nhap raw_command",
                     (unsigned)CALIBRATION_RAW_COMMAND_DELAY_MS);

            vTaskDelay(pdMS_TO_TICKS(CALIBRATION_RAW_COMMAND_DELAY_MS));
            baseline_generation = get_display_generation();
            err = virtual_key_output_run_sequence(raw_command,
                                                  VIRTUAL_KEY_INTERVAL_MS);
            if (err == ESP_OK &&
                wait_for_calibration_display(baseline_generation,
                                             raw_command, false, false)) {
                ESP_LOGI(TAG, "Calibration da xac nhan command echo=%s",
                         raw_command);

                baseline_generation = get_display_generation();
                err = virtual_key_output_run_sequence(
                    "E", VIRTUAL_KEY_INTERVAL_MS);
                if (err == ESP_OK &&
                    wait_for_calibration_display(baseline_generation,
                                                 raw_command, false, true)) {
                    ESP_LOGI(TAG, "Calibration thanh cong: E0, echo=%s",
                             raw_command);
                    vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
                    err = virtual_key_output_run_sequence(
                        "C", VIRTUAL_KEY_INTERVAL_MS);
                    if (err == ESP_OK) {
                        return ESP_OK;
                    }
                }
            }
        }

        ESP_LOGW(TAG, "Calibration %s khong xac nhan duoc o lan %u",
                 raw_command, (unsigned)attempt);
        (void)virtual_key_output_run_sequence("C", VIRTUAL_KEY_INTERVAL_MS);
        vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t execute_calibration_command(const char *raw_command)
{
    if (!device_settings_calibration_mapping_is_valid("P", raw_command)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_totalizer_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_totalizer_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = execute_calibration_locked(raw_command);
    xSemaphoreGive(s_totalizer_mutex);
    return err;
}

static esp_err_t read_total_amount(uint64_t *total_amount_vnd)
{
    for (uint32_t attempt = 1U; attempt <= TOTALIZER_MAX_ATTEMPTS; attempt++) {
        ESP_LOGI(TAG, "Doc tong tien lan %u/%u",
                 (unsigned)attempt, (unsigned)TOTALIZER_MAX_ATTEMPTS);
        esp_err_t err = virtual_key_output_run_sequence(
            "C#7733", VIRTUAL_KEY_INTERVAL_MS);
        if (err != ESP_OK) {
            return err;
        }

        vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
        uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS];
        uint32_t baseline_generation = 0U;
        (void)mbi_sniffer_get_display_snapshot(display, &baseline_generation);
        err = virtual_key_output_run_sequence("E", VIRTUAL_KEY_INTERVAL_MS);
        if (err != ESP_OK) {
            return err;
        }

        if (wait_for_total_amount(baseline_generation, total_amount_vnd)) {
            ESP_LOGI(TAG, "Doc tong tien thanh cong: %" PRIu64 " VND",
                     *total_amount_vnd);
            vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
            return virtual_key_output_run_sequence("C", VIRTUAL_KEY_INTERVAL_MS);
        }

        ESP_LOGW(TAG, "Khong tim thay P20/tong tien hop le lan %u",
                 (unsigned)attempt);
        (void)virtual_key_output_run_sequence("C", VIRTUAL_KEY_INTERVAL_MS);
        vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t read_total_volume(double *total_volume_l)
{
    for (uint32_t attempt = 1U; attempt <= TOTALIZER_MAX_ATTEMPTS; attempt++) {
        ESP_LOGI(TAG, "Doc tong lit lan %u/%u",
                 (unsigned)attempt, (unsigned)TOTALIZER_MAX_ATTEMPTS);

        uint32_t baseline_generation = get_display_generation();
        esp_err_t err = virtual_key_output_run_sequence(
            "C#44504ET", VIRTUAL_KEY_INTERVAL_MS);
        if (err != ESP_OK) {
            return err;
        }

        vTaskDelay(pdMS_TO_TICKS(TOTALIZER_DISPLAY_SETTLE_MS));

        if (wait_for_total_volume(baseline_generation, total_volume_l)) {
            ESP_LOGI(TAG, "Doc tong lit thanh cong: %.3f L", *total_volume_l);
            vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
            return virtual_key_output_run_sequence("C", VIRTUAL_KEY_INTERVAL_MS);
        }

        ESP_LOGW(TAG, "Khong decode duoc tong lit lan %u", (unsigned)attempt);
        (void)virtual_key_output_run_sequence("C", VIRTUAL_KEY_INTERVAL_MS);
        vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t read_totalizer(uint64_t *total_amount_vnd,
                         double *total_volume_l)
{
    if (total_amount_vnd == NULL || total_volume_l == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *total_amount_vnd = 0U;
    *total_volume_l = 0.0;

    if (s_totalizer_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_totalizer_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = read_total_amount(total_amount_vnd);
    if (err == ESP_OK) {
        err = read_total_volume(total_volume_l);
    }
    if (err != ESP_OK) {
        *total_amount_vnd = 0U;
        *total_volume_l = 0.0;
    }
    xSemaphoreGive(s_totalizer_mutex);
    return err;
}

static esp_err_t reset_totalizer(void)
{
    if (s_totalizer_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_totalizer_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = reset_total_amount();
    if (err == ESP_OK) {
        err = reset_total_volume();
    }
    xSemaphoreGive(s_totalizer_mutex);
    return err;
}

static esp_err_t reset_total_amount(void)
{
    for (uint32_t attempt = 1U; attempt <= TOTALIZER_MAX_ATTEMPTS; attempt++) {
        ESP_LOGI(TAG, "Reset tong tien lan %u/%u",
                 (unsigned)attempt, (unsigned)TOTALIZER_MAX_ATTEMPTS);

        esp_err_t err = virtual_key_output_run_sequence(
            "C#7733", VIRTUAL_KEY_INTERVAL_MS);
        if (err != ESP_OK) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));

        uint32_t baseline_generation = get_display_generation();
        err = virtual_key_output_run_sequence("E", VIRTUAL_KEY_INTERVAL_MS);
        if (err != ESP_OK) {
            return err;
        }

        uint64_t current_total = 0U;
        if (!wait_for_total_amount(baseline_generation, &current_total)) {
            ESP_LOGW(TAG, "Reset tong tien: khong tim thay P20 lan %u",
                     (unsigned)attempt);
            (void)virtual_key_output_run_sequence("C", VIRTUAL_KEY_INTERVAL_MS);
            vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
            continue;
        }

        ESP_LOGI(TAG, "Da vao P20, tong tien hien tai=%" PRIu64, current_total);
        vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
        baseline_generation = get_display_generation();
        err = virtual_key_output_run_sequence("000", VIRTUAL_KEY_INTERVAL_MS);
        if (err != ESP_OK) {
            return err;
        }

        if (wait_for_e0(baseline_generation)) {
            ESP_LOGI(TAG, "Reset tong tien da xac nhan E0");
            vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
            return virtual_key_output_run_sequence("C", VIRTUAL_KEY_INTERVAL_MS);
        }

        ESP_LOGW(TAG, "Reset tong tien: khong thay E0 lan %u",
                 (unsigned)attempt);
        (void)virtual_key_output_run_sequence("C", VIRTUAL_KEY_INTERVAL_MS);
        vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t reset_total_volume(void)
{
    for (uint32_t attempt = 1U; attempt <= TOTALIZER_MAX_ATTEMPTS; attempt++) {
        ESP_LOGI(TAG, "Reset tong lit lan %u/%u",
                 (unsigned)attempt, (unsigned)TOTALIZER_MAX_ATTEMPTS);

        uint32_t baseline_generation = get_display_generation();
        esp_err_t err = virtual_key_output_run_sequence(
            "C#845443E", VIRTUAL_KEY_INTERVAL_MS);
        if (err != ESP_OK) {
            return err;
        }

        if (!wait_for_reset_total_volume_screen(baseline_generation)) {
            ESP_LOGW(TAG, "Reset tong lit: khong thay P08 lan %u",
                     (unsigned)attempt);
            (void)virtual_key_output_run_sequence("C", VIRTUAL_KEY_INTERVAL_MS);
            vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
        baseline_generation = get_display_generation();
        err = virtual_key_output_run_sequence("0", VIRTUAL_KEY_INTERVAL_MS);
        if (err != ESP_OK) {
            return err;
        }

        if (!wait_for_display_row_zero(baseline_generation, 1U)) {
            ESP_LOGW(TAG, "Reset tong lit: hang 2 chua ve 0 lan %u",
                     (unsigned)attempt);
            (void)virtual_key_output_run_sequence("C", VIRTUAL_KEY_INTERVAL_MS);
            vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
            continue;
        }

        ESP_LOGI(TAG, "Reset tong lit: hang 2 da ve 0");
        vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
        baseline_generation = get_display_generation();
        err = virtual_key_output_run_sequence("E", VIRTUAL_KEY_INTERVAL_MS);
        if (err != ESP_OK) {
            return err;
        }

        if (wait_for_e0(baseline_generation)) {
            ESP_LOGI(TAG, "Reset tong lit da xac nhan E0");
            vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
            return virtual_key_output_run_sequence("C", VIRTUAL_KEY_INTERVAL_MS);
        }

        ESP_LOGW(TAG, "Reset tong lit: khong thay E0 sau phim E lan %u",
                 (unsigned)attempt);
        (void)virtual_key_output_run_sequence("C", VIRTUAL_KEY_INTERVAL_MS);
        vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t publish_totalizer_telemetry(uint64_t total_amount_vnd,
                                             double total_volume_l)
{
    time_manager_snapshot_t snapshot;
    esp_err_t err = time_manager_get_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    bool built =
        cJSON_AddNumberToObject(root, "ts", (double)snapshot.ts) != NULL &&
        cJSON_AddStringToObject(root, "version", "1.3") != NULL;
    bool data_attached = built && cJSON_AddItemToObject(root, "data", data);
    built = data_attached &&
        cJSON_AddNumberToObject(data, "total_amount_vnd",
                               (double)total_amount_vnd) != NULL &&
        cJSON_AddNumberToObject(data, "total_volume_l", total_volume_l) != NULL &&
        cJSON_AddStringToObject(data, "time_device",
                               snapshot.time_device) != NULL;
    if (!built) {
        if (!data_attached) {
            cJSON_Delete(data);
        }
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char payload[TOTALIZER_TELEMETRY_JSON_SIZE];
    if (!cJSON_PrintPreallocated(root, payload, sizeof(payload), false)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON_Delete(root);
    return mqtt_manager_publish_telemetry(payload);
}

static esp_err_t execute_set_unit_price(uint32_t unit_price)
{
    char entry_sequence[SET_UNIT_PRICE_SEQUENCE_MAX_LENGTH];
    int written = snprintf(entry_sequence, sizeof(entry_sequence),
                           "#44504EP%" PRIu32, unit_price);
    if (written < 0 || (size_t)written >= sizeof(entry_sequence)) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (uint32_t attempt = 1U; attempt <= SET_UNIT_PRICE_MAX_ATTEMPTS; attempt++) {
        ESP_LOGI(TAG, "set_unit_price lan %u/%u: sequence=%sE",
                 (unsigned)attempt,
                 (unsigned)SET_UNIT_PRICE_MAX_ATTEMPTS,
                 entry_sequence);

        esp_err_t err = virtual_key_output_run_sequence(
            entry_sequence, VIRTUAL_KEY_INTERVAL_MS);
        if (err != ESP_OK) {
            return err;
        }

        vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
        uint8_t current_display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS];
        uint32_t baseline_generation = 0U;
        (void)mbi_sniffer_get_display_snapshot(current_display,
                                               &baseline_generation);

        err = virtual_key_output_run_sequence("E", VIRTUAL_KEY_INTERVAL_MS);
        if (err != ESP_OK) {
            return err;
        }

        if (wait_for_unit_price_confirmation(unit_price, baseline_generation)) {
            ESP_LOGI(TAG, "Xac nhan man hinh E0 va unit_price=%" PRIu32,
                     unit_price);
            vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEY_INTERVAL_MS));
            return virtual_key_output_run_sequence(
                "C", VIRTUAL_KEY_INTERVAL_MS);
        }

        ESP_LOGW(TAG, "Khong xac nhan duoc E0/unit_price=%" PRIu32
                      " o lan %u",
                 unit_price, (unsigned)attempt);
    }

    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t publish_ack(const char *request_id,
                             const char *command,
                             const char *result,
                             const char *description,
                             bool report_unit_price,
                             uint32_t unit_price,
                             bool report_hash_key_locked,
                             bool hash_key_locked,
                             bool report_totalizer,
                             uint64_t total_amount_vnd,
                             double total_volume_l)
{
    time_manager_snapshot_t snapshot;
    esp_err_t err = time_manager_get_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *reported = cJSON_CreateObject();
    if (root == NULL || reported == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(reported);
        return ESP_ERR_NO_MEM;
    }

    if (report_unit_price &&
        cJSON_AddNumberToObject(reported, "unit_price", unit_price) == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(reported);
        return ESP_ERR_NO_MEM;
    }

    if (report_hash_key_locked &&
        cJSON_AddBoolToObject(reported, "hash_key_locked",
                              hash_key_locked) == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(reported);
        return ESP_ERR_NO_MEM;
    }

    if (report_totalizer &&
        (cJSON_AddNumberToObject(reported, "total_amount_vnd",
                                 (double)total_amount_vnd) == NULL ||
         cJSON_AddNumberToObject(reported, "total_volume_l",
                                 total_volume_l) == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(reported);
        return ESP_ERR_NO_MEM;
    }

    bool built =
        cJSON_AddNumberToObject(root, "ts", (double)snapshot.ts) != NULL &&
        cJSON_AddStringToObject(root, "version", "1.3") != NULL &&
        cJSON_AddStringToObject(root, "request_id", request_id) != NULL &&
        cJSON_AddStringToObject(root, "ack_to", command) != NULL &&
        cJSON_AddStringToObject(root, "result", result) != NULL &&
        cJSON_AddStringToObject(root, "description", description) != NULL;

    bool reported_attached = built &&
                             cJSON_AddItemToObject(root, "reported", reported);
    built = reported_attached &&
            cJSON_AddStringToObject(root, "time_device",
                                    snapshot.time_device) != NULL;

    if (!built) {
        if (!reported_attached) {
            cJSON_Delete(reported);
        }
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = mqtt_manager_publish_ack(payload);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ACK queued: request_id=%s ack_to=%s result=%s description=%s",
                 request_id, command, result, description);
    }
    cJSON_free(payload);
    return err;
}

static esp_err_t publish_shortcut_mapping_ack(
    const char *request_id,
    const shortcut_mapping_t *mapping,
    uint8_t slot,
    uint32_t revision)
{
    time_manager_snapshot_t snapshot;
    esp_err_t err = time_manager_get_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *reported = cJSON_CreateObject();
    cJSON *mapping_json = cJSON_CreateObject();
    if (root == NULL || reported == NULL || mapping_json == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(reported);
        cJSON_Delete(mapping_json);
        return ESP_ERR_NO_MEM;
    }

    bool mapping_built =
        cJSON_AddStringToObject(mapping_json, "physical_key",
                               mapping->physical_key) != NULL &&
        cJSON_AddStringToObject(mapping_json, "shortcut_key",
                               mapping->shortcut_key) != NULL;
    bool mapping_attached = mapping_built &&
                            cJSON_AddItemToObject(reported, "mapping",
                                                  mapping_json);
    bool reported_built = mapping_attached &&
                          cJSON_AddNumberToObject(reported, "revision",
                                                 revision) != NULL;
    bool root_built = reported_built &&
        cJSON_AddNumberToObject(root, "ts", (double)snapshot.ts) != NULL &&
        cJSON_AddStringToObject(root, "version", "1.3") != NULL &&
        cJSON_AddStringToObject(root, "request_id", request_id) != NULL &&
        cJSON_AddStringToObject(root, "ack_to",
                               "set_shortcut_mapping") != NULL &&
        cJSON_AddStringToObject(root, "result", "ok") != NULL &&
        cJSON_AddStringToObject(root, "description",
                               "shortcut_mapping_updated") != NULL;
    bool reported_attached = root_built &&
                             cJSON_AddItemToObject(root, "reported", reported);
    bool built = reported_attached &&
                 cJSON_AddStringToObject(root, "time_device",
                                         snapshot.time_device) != NULL;
    if (!built) {
        if (!reported_attached) {
            cJSON_Delete(reported);
        }
        if (!mapping_attached) {
            cJSON_Delete(mapping_json);
        }
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = mqtt_manager_publish_ack(payload);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ACK shortcut queued: result=ok "
                      "description=shortcut_mapping_updated "
                      "slot=%u %s -> %s revision=%" PRIu32,
                 (unsigned)slot, mapping->shortcut_key,
                 mapping->physical_key, revision);
    }
    cJSON_free(payload);
    return err;
}

static esp_err_t publish_price_edit_lock_ack(const char *request_id,
                                             bool price_edit_locked)
{
    time_manager_snapshot_t snapshot;
    esp_err_t err = time_manager_get_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *reported = cJSON_CreateObject();
    if (root == NULL || reported == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(reported);
        return ESP_ERR_NO_MEM;
    }

    bool reported_built = cJSON_AddBoolToObject(
        reported, "price_edit_locked", price_edit_locked) != NULL;
    bool root_built = reported_built &&
        cJSON_AddNumberToObject(root, "ts", (double)snapshot.ts) != NULL &&
        cJSON_AddStringToObject(root, "version", "1.3") != NULL &&
        cJSON_AddStringToObject(root, "request_id", request_id) != NULL &&
        cJSON_AddStringToObject(root, "ack_to",
                               "set_price_edit_lock") != NULL &&
        cJSON_AddStringToObject(root, "result", "ok") != NULL &&
        cJSON_AddStringToObject(root, "description",
                               "price_edit_lock_updated") != NULL;
    bool reported_attached = root_built &&
                             cJSON_AddItemToObject(root, "reported", reported);
    bool built = reported_attached &&
                 cJSON_AddStringToObject(root, "time_device",
                                         snapshot.time_device) != NULL;
    if (!built) {
        if (!reported_attached) {
            cJSON_Delete(reported);
        }
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = mqtt_manager_publish_ack(payload);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ACK price edit lock queued: result=ok "
                      "description=price_edit_lock_updated "
                      "price_edit_locked=%s",
                 price_edit_locked ? "true" : "false");
    }
    cJSON_free(payload);
    return err;
}

static esp_err_t publish_calibration_ack(const char *request_id,
                                         const calibration_mapping_t *mapping,
                                         uint8_t slot)
{
    time_manager_snapshot_t snapshot;
    esp_err_t err = time_manager_get_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *reported = cJSON_CreateObject();
    if (root == NULL || reported == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(reported);
        return ESP_ERR_NO_MEM;
    }

    bool reported_built =
        cJSON_AddStringToObject(reported, "name", mapping->name) != NULL &&
        cJSON_AddStringToObject(reported, "command_echo",
                                mapping->raw_command) != NULL;
    bool root_built = reported_built &&
        cJSON_AddNumberToObject(root, "ts", (double)snapshot.ts) != NULL &&
        cJSON_AddStringToObject(root, "version", "1.3") != NULL &&
        cJSON_AddStringToObject(root, "request_id", request_id) != NULL &&
        cJSON_AddStringToObject(root, "ack_to",
                               "send_calibration_command") != NULL &&
        cJSON_AddStringToObject(root, "result", "ok") != NULL &&
        cJSON_AddStringToObject(root, "description",
                               "calibration_command_applied") != NULL;
    bool reported_attached = root_built &&
                             cJSON_AddItemToObject(root, "reported", reported);
    bool built = reported_attached &&
                 cJSON_AddStringToObject(root, "time_device",
                                         snapshot.time_device) != NULL;
    if (!built) {
        if (!reported_attached) {
            cJSON_Delete(reported);
        }
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = mqtt_manager_publish_ack(payload);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ACK calibration queued: slot=%u name=%s command_echo=%s",
                 (unsigned)slot, mapping->name, mapping->raw_command);
    }
    cJSON_free(payload);
    return err;
}

static void process_command(const command_message_t *message)
{
    cJSON *root = cJSON_ParseWithLength(message->payload, message->length);
    if (root == NULL || !cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "Command JSON khong hop le");
        cJSON_Delete(root);
        return;
    }

    const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!string_field_is_valid(request_id, REQUEST_ID_MAX_LENGTH) ||
        !string_field_is_valid(command, COMMAND_NAME_MAX_LENGTH)) {
        ESP_LOGE(TAG, "Command thieu request_id/cmd hop le, khong the tao ACK");
        cJSON_Delete(root);
        return;
    }

    if (request_was_processed(request_id->valuestring)) {
        esp_err_t err = publish_ack(request_id->valuestring, command->valuestring,
                                    "rejected", "duplicate_request",
                                    false, 0U, false, false,
                                    false, 0U, 0.0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Gui duplicate ACK that bai: %s", esp_err_to_name(err));
        }
        cJSON_Delete(root);
        return;
    }

    const cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(root, "ts");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *param = cJSON_GetObjectItemCaseSensitive(root, "param");
    bool timestamp_valid = cJSON_IsNumber(timestamp) &&
                           timestamp->valuedouble >= 0.0 &&
                           floor(timestamp->valuedouble) == timestamp->valuedouble;
    bool envelope_valid = timestamp_valid &&
                          cJSON_IsString(version) &&
                          strcmp(version->valuestring, "1.3") == 0 &&
                          cJSON_IsObject(param);
    bool reset_totalizer_envelope_valid =
        (timestamp == NULL || timestamp_valid) &&
        cJSON_IsString(version) &&
        strcmp(version->valuestring, "1.3") == 0 &&
        cJSON_IsObject(param);

    if (strcmp(command->valuestring, "set_shortcut_mapping") == 0) {
        const cJSON *physical_key = NULL;
        const cJSON *shortcut_key = NULL;
        uint8_t slot = 0U;
        bool param_valid = envelope_valid && get_indexed_param_pair(
            param, "physical_key", "shortcut_key",
            SHORTCUT_MAPPING_MAX_COUNT, &physical_key, &shortcut_key, &slot) &&
            string_field_is_valid(physical_key,
                                  SHORTCUT_PHYSICAL_KEY_MAX_LENGTH) &&
            string_field_is_valid(shortcut_key, SHORTCUT_KEY_MAX_LENGTH) &&
            device_settings_shortcut_mapping_is_valid(
                shortcut_key->valuestring, physical_key->valuestring);
        if (!param_valid) {
            esp_err_t ack_err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", "invalid_param", false, 0U, false, false,
                false, 0U, 0.0);
            if (ack_err == ESP_OK) {
                remember_request(request_id->valuestring);
            }
            cJSON_Delete(root);
            return;
        }

        uint32_t revision = 0U;
        esp_err_t err = device_settings_set_shortcut_mapping_at(
            slot, shortcut_key->valuestring, physical_key->valuestring,
            &revision);
        shortcut_mapping_t readback = {0};
        uint32_t readback_revision = 0U;
        if (err == ESP_OK) {
            err = device_settings_get_shortcut_mapping_at(
                slot, &readback, &readback_revision);
            if (err == ESP_OK &&
                (strcmp(readback.shortcut_key,
                        shortcut_key->valuestring) != 0 ||
                 strcmp(readback.physical_key,
                        physical_key->valuestring) != 0 ||
                 readback_revision != revision)) {
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }

        if (err != ESP_OK) {
            const char *description;
            if (err == ESP_ERR_NO_MEM) {
                description = "mapping_limit_reached";
            } else if (err == ESP_ERR_INVALID_RESPONSE ||
                       err == ESP_ERR_NOT_FOUND) {
                description = "reported_mismatch";
            } else if (err == ESP_ERR_INVALID_ARG) {
                description = "invalid_param";
            } else if (err == ESP_ERR_INVALID_STATE) {
                description = "internal_error";
            } else {
                description = "storage_error";
            }
            ESP_LOGE(TAG, "Cap nhat shortcut mapping that bai: %s",
                     esp_err_to_name(err));
            esp_err_t ack_err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", description, false, 0U, false, false,
                false, 0U, 0.0);
            if (ack_err == ESP_OK) {
                remember_request(request_id->valuestring);
            }
            cJSON_Delete(root);
            return;
        }

        err = publish_shortcut_mapping_ack(request_id->valuestring,
                                           &readback, slot,
                                           readback_revision);
        if (err == ESP_OK) {
            remember_request(request_id->valuestring);
        } else {
            ESP_LOGE(TAG, "Publish ACK shortcut mapping that bai: %s",
                     esp_err_to_name(err));
        }
        cJSON_Delete(root);
        return;
    }

    if (strcmp(command->valuestring, "reset_totalizer") == 0) {
        const cJSON *confirm = cJSON_IsObject(param)
                                   ? cJSON_GetObjectItemCaseSensitive(param,
                                                                      "confirm")
                                   : NULL;
        bool param_valid = reset_totalizer_envelope_valid &&
                           cJSON_IsTrue(confirm) &&
                           cJSON_GetArraySize(param) == 1;
        if (!param_valid) {
            esp_err_t err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", "invalid_param",
                false, 0U, false, false,
                false, 0U, 0.0);
            if (err == ESP_OK) {
                remember_request(request_id->valuestring);
            } else {
                ESP_LOGE(TAG, "Publish ACK reset_totalizer invalid_param that bai: %s",
                         esp_err_to_name(err));
            }
            cJSON_Delete(root);
            return;
        }

        esp_err_t err = reset_totalizer();

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "RESET_TOTALIZER THAT BAI: %s", esp_err_to_name(err));
            esp_err_t ack_err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", "verification_failed",
                false, 0U, false, false,
                false, 0U, 0.0);
            if (ack_err == ESP_OK) {
                remember_request(request_id->valuestring);
            } else {
                ESP_LOGE(TAG, "Publish ACK loi reset_totalizer that bai: %s",
                         esp_err_to_name(ack_err));
            }
            cJSON_Delete(root);
            return;
        }

        err = publish_ack(
            request_id->valuestring, command->valuestring,
            "ok", "totalizer_reset",
            false, 0U, false, false,
            true, 0U, 0.0);
        if (err == ESP_OK) {
            remember_request(request_id->valuestring);
            ESP_LOGI(TAG, "Reset totalizer hoan tat va da ACK");
        } else {
            ESP_LOGE(TAG, "Publish ACK reset_totalizer that bai: %s",
                     esp_err_to_name(err));
        }
        cJSON_Delete(root);
        return;
    }

    if (envelope_valid &&
        strcmp(command->valuestring, "set_price_edit_lock") == 0) {
        const cJSON *locked_item =
            cJSON_GetObjectItemCaseSensitive(param, "locked");
        if (!cJSON_IsBool(locked_item) || cJSON_GetArraySize(param) != 1) {
            esp_err_t err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", "invalid_param", false, 0U, false, false,
                false, 0U, 0.0);
            if (err == ESP_OK) {
                remember_request(request_id->valuestring);
            } else {
                ESP_LOGE(TAG, "Publish ACK set_price_edit_lock invalid_param "
                              "that bai: %s",
                         esp_err_to_name(err));
            }
            cJSON_Delete(root);
            return;
        }

        bool requested_locked = cJSON_IsTrue(locked_item);
        __atomic_store_n(&s_price_edit_locked, requested_locked,
                         __ATOMIC_RELEASE);
        bool actual_locked = __atomic_load_n(&s_price_edit_locked,
                                             __ATOMIC_ACQUIRE);
        if (actual_locked != requested_locked) {
            esp_err_t err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", "verification_failed", false, 0U, false, false,
                false, 0U, 0.0);
            if (err == ESP_OK) {
                remember_request(request_id->valuestring);
            }
            cJSON_Delete(root);
            return;
        }

        esp_err_t err = publish_price_edit_lock_ack(
            request_id->valuestring, actual_locked);
        if (err == ESP_OK) {
            remember_request(request_id->valuestring);
        } else {
            ESP_LOGE(TAG, "Publish ACK set_price_edit_lock that bai: %s",
                     esp_err_to_name(err));
        }
        cJSON_Delete(root);
        return;
    }

    if (envelope_valid &&
        strcmp(command->valuestring, "set_hash_key_lock") == 0) {
        const cJSON *locked_item =
            cJSON_GetObjectItemCaseSensitive(param, "locked");
        if (!cJSON_IsBool(locked_item)) {
            esp_err_t err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", "invalid_param", false, 0U, false, false,
                false, 0U, 0.0);
            if (err == ESP_OK) {
                remember_request(request_id->valuestring);
            } else {
                ESP_LOGE(TAG, "Publish ACK that bai: %s", esp_err_to_name(err));
            }
            cJSON_Delete(root);
            return;
        }

        bool requested_locked = cJSON_IsTrue(locked_item);
        esp_err_t settings_err =
            device_settings_set_hash_key_locked(requested_locked);
        bool actual_locked = device_settings_is_hash_key_locked();

        const char *result;
        const char *description;
        if (settings_err != ESP_OK) {
            result = "error";
            description = "storage_error";
        } else if (actual_locked != requested_locked) {
            result = "error";
            description = "verification_failed";
        } else {
            result = "ok";
            description = "hash_key_lock_updated";
        }

        esp_err_t ack_err = publish_ack(
            request_id->valuestring, command->valuestring,
            result, description, false, 0U, true, actual_locked,
            false, 0U, 0.0);
        if (ack_err == ESP_OK) {
            remember_request(request_id->valuestring);
        } else {
            ESP_LOGE(TAG, "Publish ACK set_hash_key_lock that bai: %s",
                     esp_err_to_name(ack_err));
        }
        cJSON_Delete(root);
        return;
    }

    if (envelope_valid && strcmp(command->valuestring, "close_shift") == 0) {
        if (cJSON_GetArraySize(param) != 0) {
            esp_err_t err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", "invalid_param", false, 0U, false, false,
                false, 0U, 0.0);
            if (err == ESP_OK) {
                remember_request(request_id->valuestring);
            } else {
                ESP_LOGE(TAG, "Publish ACK close_shift invalid_param that bai: %s",
                         esp_err_to_name(err));
            }
            cJSON_Delete(root);
            return;
        }

        uint64_t total_amount_vnd = 0U;
        double total_volume_l = 0.0;
        esp_err_t err = read_totalizer(&total_amount_vnd, &total_volume_l);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CLOSE_SHIFT doc Totalizer that bai: %s",
                     esp_err_to_name(err));
            esp_err_t ack_err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", "verification_failed", false, 0U, false, false,
                false, 0U, 0.0);
            if (ack_err == ESP_OK) {
                remember_request(request_id->valuestring);
            } else {
                ESP_LOGE(TAG, "Publish ACK loi close_shift that bai: %s",
                         esp_err_to_name(ack_err));
            }
            cJSON_Delete(root);
            return;
        }

        err = publish_ack(
            request_id->valuestring, command->valuestring,
            "ok", "shift_closed",
            false, 0U, false, false,
            true, total_amount_vnd, total_volume_l);
        if (err == ESP_OK) {
            remember_request(request_id->valuestring);
            ESP_LOGI(TAG, "Close shift ACK queued: amount=%" PRIu64
                          " VND volume=%.3f L",
                     total_amount_vnd, total_volume_l);
        } else {
            ESP_LOGE(TAG, "Publish ACK close_shift that bai: %s",
                     esp_err_to_name(err));
        }
        cJSON_Delete(root);
        return;
    }

    if (envelope_valid && strcmp(command->valuestring, "get_totalizer") == 0) {
        if (cJSON_GetArraySize(param) != 0) {
            esp_err_t err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", "invalid_param", false, 0U, false, false,
                false, 0U, 0.0);
            if (err == ESP_OK) {
                remember_request(request_id->valuestring);
            }
            cJSON_Delete(root);
            return;
        }

        uint64_t total_amount_vnd = 0U;
        double total_volume_l = 0.0;
        esp_err_t err = read_totalizer(&total_amount_vnd, &total_volume_l);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GET_TOTALIZER THAT BAI: %s", esp_err_to_name(err));
            esp_err_t ack_err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", "verification_failed", false, 0U, false, false,
                false, 0U, 0.0);
            if (ack_err != ESP_OK) {
                ESP_LOGE(TAG, "Publish ACK loi get_totalizer that bai: %s",
                         esp_err_to_name(ack_err));
            } else {
                remember_request(request_id->valuestring);
            }
            cJSON_Delete(root);
            return;
        }

        err = publish_ack(
            request_id->valuestring, command->valuestring,
            "ok", "totalizer_read_success",
            false, 0U, false, false,
            true, total_amount_vnd, total_volume_l);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Publish ACK totalizer that bai: %s",
                     esp_err_to_name(err));
            cJSON_Delete(root);
            return;
        }
        remember_request(request_id->valuestring);

        err = publish_totalizer_telemetry(total_amount_vnd, total_volume_l);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Publish totalizer telemetry that bai: %s",
                     esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Totalizer telemetry queued: amount=%" PRIu64
                          " VND volume=%.3f L",
                     total_amount_vnd, total_volume_l);
        }
        cJSON_Delete(root);
        return;
    }

    if (envelope_valid && strcmp(command->valuestring, "set_unit_price") == 0) {
        const cJSON *unit_price_item =
            cJSON_GetObjectItemCaseSensitive(param, "unit_price");
        bool unit_price_valid = cJSON_IsNumber(unit_price_item) &&
                                unit_price_item->valuedouble >= 1.0 &&
                                unit_price_item->valuedouble <= SET_UNIT_PRICE_MAX &&
                                floor(unit_price_item->valuedouble) ==
                                    unit_price_item->valuedouble;
        if (!unit_price_valid) {
            esp_err_t err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", "invalid_param", false, 0U, false, false,
                false, 0U, 0.0);
            if (err == ESP_OK) {
                remember_request(request_id->valuestring);
            } else {
                ESP_LOGE(TAG, "Publish ACK that bai: %s", esp_err_to_name(err));
            }
            cJSON_Delete(root);
            return;
        }

        uint32_t unit_price = (uint32_t)unit_price_item->valuedouble;
        esp_err_t err = publish_ack(
            request_id->valuestring, command->valuestring,
            "ok", "unit_price_accepted", true, unit_price, false, false,
            false, 0U, 0.0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Publish ACK set_unit_price that bai: %s",
                     esp_err_to_name(err));
            cJSON_Delete(root);
            return;
        }

        remember_request(request_id->valuestring);

        ESP_LOGI(TAG, "Thuc thi set_unit_price=%" PRIu32
                      ", interval=%u ms, max_attempts=%u",
                 unit_price,
                 (unsigned)VIRTUAL_KEY_INTERVAL_MS,
                 (unsigned)SET_UNIT_PRICE_MAX_ATTEMPTS);
        err = execute_set_unit_price(unit_price);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SET_UNIT_PRICE THAT BAI sau %u lan: %s",
                     (unsigned)SET_UNIT_PRICE_MAX_ATTEMPTS,
                     esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Da hoan thanh chuoi phim set_unit_price");
        }
        cJSON_Delete(root);
        return;
    }

    if (envelope_valid &&
        strcmp(command->valuestring, "send_calibration_command") == 0) {
        const cJSON *name = NULL;
        const cJSON *raw_command = NULL;
        uint8_t slot = 0U;
        bool param_valid = get_indexed_param_pair(
            param, "name", "raw_command", CALIBRATION_MAPPING_MAX_COUNT,
            &name, &raw_command, &slot) &&
            string_field_is_valid(name, SHORTCUT_KEY_MAX_LENGTH) &&
            string_field_is_valid(raw_command,
                                  CALIBRATION_RAW_COMMAND_MAX_LENGTH) &&
            device_settings_calibration_mapping_is_valid(
                name->valuestring, raw_command->valuestring);
        if (!param_valid) {
            esp_err_t err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", "invalid_param", false, 0U, false, false,
                false, 0U, 0.0);
            if (err == ESP_OK) {
                remember_request(request_id->valuestring);
            } else {
                ESP_LOGE(TAG, "Publish ACK calibration invalid_param that bai: %s",
                         esp_err_to_name(err));
            }
            cJSON_Delete(root);
            return;
        }

        esp_err_t err = execute_calibration_command(raw_command->valuestring);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Calibration MQTT that bai sau %u lan: %s",
                     (unsigned)CALIBRATION_MAX_ATTEMPTS,
                     esp_err_to_name(err));
            esp_err_t ack_err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", "verification_failed", false, 0U, false, false,
                false, 0U, 0.0);
            if (ack_err == ESP_OK) {
                remember_request(request_id->valuestring);
            }
            cJSON_Delete(root);
            return;
        }

        err = device_settings_set_calibration_mapping_at(
            slot, name->valuestring, raw_command->valuestring);
        calibration_mapping_t readback = {0};
        if (err == ESP_OK) {
            err = device_settings_get_calibration_mapping_at(slot, &readback);
            if (err == ESP_OK &&
                (strcmp(readback.name, name->valuestring) != 0 ||
                 strcmp(readback.raw_command,
                        raw_command->valuestring) != 0)) {
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
        if (err != ESP_OK) {
            const char *description;
            if (err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_NOT_FOUND) {
                description = "verification_failed";
            } else if (err == ESP_ERR_INVALID_ARG) {
                description = "invalid_param";
            } else {
                description = "storage_error";
            }
            ESP_LOGE(TAG, "Luu calibration mapping that bai: %s",
                     esp_err_to_name(err));
            esp_err_t ack_err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", description, false, 0U, false, false,
                false, 0U, 0.0);
            if (ack_err == ESP_OK) {
                remember_request(request_id->valuestring);
            }
            cJSON_Delete(root);
            return;
        }

        err = publish_calibration_ack(request_id->valuestring, &readback,
                                      slot);
        if (err == ESP_OK) {
            remember_request(request_id->valuestring);
        } else {
            ESP_LOGE(TAG, "Publish ACK calibration that bai: %s",
                     esp_err_to_name(err));
        }
        cJSON_Delete(root);
        return;
    }

    const char *result = envelope_valid ? "rejected" : "error";
    const char *description = envelope_valid ? "unsupported_command" :
                                               "invalid_param";
    if (envelope_valid) {
        ESP_LOGW(TAG, "%s command chua co backend/readback: %s",
                 command_name_is_declared(command->valuestring) ? "Declared" : "Unknown",
                 command->valuestring);
    }
    esp_err_t err = publish_ack(request_id->valuestring, command->valuestring,
                                result, description,
                                false, 0U, false, false,
                                false, 0U, 0.0);
    if (err == ESP_OK) {
        remember_request(request_id->valuestring);
    } else {
        ESP_LOGE(TAG, "Publish ACK that bai: %s", esp_err_to_name(err));
    }
    cJSON_Delete(root);
}

static void command_callback(const char *payload, size_t length)
{
    if (payload == NULL || length == 0U ||
        length > COMMAND_PAYLOAD_MAX_LENGTH) {
        ESP_LOGE(TAG, "Command payload length khong hop le: %u", (unsigned)length);
        return;
    }

    command_message_t message = {.length = length};
    memcpy(message.payload, payload, length);
    message.payload[length] = '\0';
    if (xQueueSend(s_command_queue, &message, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Command queue day, bo qua payload moi");
    }
}

static void command_task(void *argument)
{
    (void)argument;
    command_message_t message;
    while (1) {
        if (xQueueReceive(s_command_queue, &message, portMAX_DELAY) == pdTRUE) {
            while (!time_manager_is_valid() || !mqtt_manager_is_ready()) {
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            process_command(&message);
        }
    }
}

esp_err_t mqtt_command_handler_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_totalizer_mutex = xSemaphoreCreateMutex();
    if (s_totalizer_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_command_queue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(command_message_t));
    if (s_command_queue == NULL) {
        vSemaphoreDelete(s_totalizer_mutex);
        s_totalizer_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(command_task, "mqtt_command", COMMAND_TASK_STACK_SIZE,
                    NULL, COMMAND_TASK_PRIORITY, NULL) != pdPASS) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        vSemaphoreDelete(s_totalizer_mutex);
        s_totalizer_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    mqtt_manager_set_command_callback(command_callback);
    s_started = true;
    ESP_LOGI(TAG, "MQTT command handler started");
    return ESP_OK;
}
