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
#define SET_UNIT_PRICE_KEY_INTERVAL_MS 500U
#define SET_UNIT_PRICE_MAX          999999U
#define SET_UNIT_PRICE_SEQUENCE_MAX_LENGTH 24U
#define SET_UNIT_PRICE_MAX_ATTEMPTS  3U
#define SET_UNIT_PRICE_VERIFY_TIMEOUT_MS 3000U
#define SET_UNIT_PRICE_VERIFY_POLL_MS 50U

typedef struct {
    size_t length;
    char payload[COMMAND_PAYLOAD_MAX_LENGTH + 1U];
} command_message_t;

static const char *TAG = "MQTT_COMMAND";
static QueueHandle_t s_command_queue;
static bool s_started;
static char s_request_history[DEDUP_HISTORY_LENGTH][REQUEST_ID_MAX_LENGTH + 1U];
static size_t s_request_history_count;
static size_t s_request_history_next;

static bool string_field_is_valid(const cJSON *item, size_t max_length)
{
    return cJSON_IsString(item) && item->valuestring != NULL &&
           item->valuestring[0] != '\0' &&
           strlen(item->valuestring) <= max_length;
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

static bool display_confirms_unit_price(
    const uint8_t display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS],
    uint32_t expected_unit_price)
{
    for (size_t column = 0; column < 4U; column++) {
        if ((display[0][column] & 0x7FU) != 0x00U) {
            return false;
        }
    }
    if ((display[0][4] & 0x7FU) != 0x79U ||
        (display[0][5] & 0x7FU) != 0x3FU) {
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
            entry_sequence, SET_UNIT_PRICE_KEY_INTERVAL_MS);
        if (err != ESP_OK) {
            return err;
        }

        vTaskDelay(pdMS_TO_TICKS(SET_UNIT_PRICE_KEY_INTERVAL_MS));
        uint8_t current_display[PUMP_DATA_DISPLAY_ROWS][PUMP_DATA_DISPLAY_COLUMNS];
        uint32_t baseline_generation = 0U;
        (void)mbi_sniffer_get_display_snapshot(current_display,
                                               &baseline_generation);

        err = virtual_key_output_run_sequence("E", SET_UNIT_PRICE_KEY_INTERVAL_MS);
        if (err != ESP_OK) {
            return err;
        }

        if (wait_for_unit_price_confirmation(unit_price, baseline_generation)) {
            ESP_LOGI(TAG, "Xac nhan man hinh E0 va unit_price=%" PRIu32,
                     unit_price);
            vTaskDelay(pdMS_TO_TICKS(SET_UNIT_PRICE_KEY_INTERVAL_MS));
            return virtual_key_output_run_sequence(
                "C", SET_UNIT_PRICE_KEY_INTERVAL_MS);
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
                             bool hash_key_locked)
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
                                    false, 0U, false, false);
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

    if (envelope_valid &&
        strcmp(command->valuestring, "set_hash_key_lock") == 0) {
        const cJSON *locked_item =
            cJSON_GetObjectItemCaseSensitive(param, "locked");
        if (!cJSON_IsBool(locked_item)) {
            esp_err_t err = publish_ack(
                request_id->valuestring, command->valuestring,
                "error", "invalid_param", false, 0U, false, false);
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
            result, description, false, 0U, true, actual_locked);
        if (ack_err == ESP_OK) {
            remember_request(request_id->valuestring);
        } else {
            ESP_LOGE(TAG, "Publish ACK set_hash_key_lock that bai: %s",
                     esp_err_to_name(ack_err));
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
                "error", "invalid_param", false, 0U, false, false);
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
            "ok", "unit_price_accepted", true, unit_price, false, false);
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
                 (unsigned)SET_UNIT_PRICE_KEY_INTERVAL_MS,
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
                                false, 0U, false, false);
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

    s_command_queue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(command_message_t));
    if (s_command_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(command_task, "mqtt_command", COMMAND_TASK_STACK_SIZE,
                    NULL, COMMAND_TASK_PRIORITY, NULL) != pdPASS) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    mqtt_manager_set_command_callback(command_callback);
    s_started = true;
    ESP_LOGI(TAG, "MQTT command handler started");
    return ESP_OK;
}
