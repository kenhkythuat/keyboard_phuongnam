#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "device_config.h"
#include "device_settings.h"
#include "control_display_led.h"
#include "pump_data_sniffer.h"
#include "mqtt_command_handler.h"
#include "mqtt_manager.h"
#include "ota_manager.h"
#include "telemetry_heartbeat.h"
#include "time_manager.h"
#include "virtual_key_output.h"
#include "wifi_manager.h"

static const char *TAG = "KEY_OUTPUT";

#define ENABLE_KEYPAD               1
#define COMBINES_KEYPAD_FUNTIONS    1
#define ENABLE_PUMP_DATA_SNIFFER    1
#define ENABLE_AUTO_KEY_SEQUENCE    0

#define KEY_OUTPUT_PIN_COUNT        5

#define ENABLE_VIRTUAL_LED_GPIO     GPIO_NUM_39

#define AUTO_KEY_SEQUENCE              "#122973E2522EC"
#define AUTO_KEY_HOLD_TIME_MS          50
#define AUTO_KEY_DELAY_BETWEEN_MS      50
#define AUTO_KEY_REPEAT_DELAY_MS       2000
#define SHORTCUT_OUTPUT_INTERVAL_MS    500U
#define SHORTCUT_INPUT_TIMEOUT_MS      2000U
#define SHORTCUT_OUTPUT_QUEUE_LENGTH   4U
#define TOTALIZER_VIEW_POLL_MS         50U
#define TOTALIZER_VIEW_TASK_STACK_SIZE 4096U
#define TOTALIZER_VIEW_MAX_SEGMENTS    CONTROL_DISPLAY_COLUMNS
#define WIFI_CONFIG_LED_POLL_MS        50U
#define WIFI_CONFIG_LED_TASK_STACK_SIZE 3072U

#define PIN_D0          GPIO_NUM_16
#define PIN_D1          GPIO_NUM_8
#define PIN_D2          GPIO_NUM_18
#define PIN_D3          GPIO_NUM_17
#define PIN_D4          GPIO_NUM_7

#define KEY_1           1
#define KEY_2           2
#define KEY_3           3
#define KEY_4           4
#define KEY_5           5
#define KEY_6           6
#define KEY_7           7
#define KEY_8           8
#define KEY_9           9
#define KEY_0           10
#define KEY_F4          11
#define KEY_HASH        12
#define KEY_F5          13
#define KEY_DOLLAR      14
#define KEY_F6          15
#define KEY_L           16
#define KEY_V           17
#define KEY_F3          18
#define KEY_P           19
#define KEY_F2          20
#define KEY_E           21
#define KEY_F1          22
#define KEY_T           23
#define KEY_C           24
#define KEY_NONE        0

static const gpio_num_t key_pins[KEY_OUTPUT_PIN_COUNT] = {
    PIN_D0,
    PIN_D1,
    PIN_D2,
    PIN_D3,
    PIN_D4
};

typedef enum {
    SHORTCUT_OUTPUT_SEQUENCE,
    SHORTCUT_OUTPUT_CALIBRATION,
} shortcut_output_type_t;

typedef struct {
    shortcut_output_type_t type;
    char sequence[SHORTCUT_PHYSICAL_KEY_MAX_LENGTH + 1U];
    char mode_calibration[SHORTCUT_KEY_MAX_LENGTH + 1U];
} shortcut_output_message_t;

typedef enum {
    WIFI_STATUS_LED_NONE,
    WIFI_STATUS_LED_CONFIG,
    WIFI_STATUS_LED_DONE,
    WIFI_STATUS_LED_FAIL,
} wifi_status_led_view_t;

static SemaphoreHandle_t s_key_output_mutex;
static QueueHandle_t s_shortcut_output_queue;
static bool s_virtual_output_active;
static TaskHandle_t s_totalizer_view_task_handle;
static bool s_totalizer_view_active;
static bool s_totalizer_view_exit_requested;
static wifi_status_led_view_t s_wifi_status_led_view;

static void virtual_led_enable_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << ENABLE_VIRTUAL_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(ENABLE_VIRTUAL_LED_GPIO, 0));

    ESP_LOGI(
        TAG,
        "ENABLE_VIRTUAL_LED IO39 = 0, LED do mach ngoai dieu khien"
    );
}

static const char *key_get_name(uint8_t key_code)
{
    switch (key_code) {
        case KEY_1:      return "1";
        case KEY_2:      return "2";
        case KEY_3:      return "3";
        case KEY_4:      return "4";
        case KEY_5:      return "5";
        case KEY_6:      return "6";
        case KEY_7:      return "7";
        case KEY_8:      return "8";
        case KEY_9:      return "9";
        case KEY_0:      return "0";
        case KEY_F4:     return "F4";
        case KEY_HASH:   return "#";
        case KEY_F5:     return "F5";
        case KEY_DOLLAR: return "$";
        case KEY_F6:     return "F6";
        case KEY_L:      return "L";
        case KEY_V:      return "V";
        case KEY_F3:     return "F3";
        case KEY_P:      return "P";
        case KEY_F2:     return "F2";
        case KEY_E:      return "E";
        case KEY_F1:     return "F1";
        case KEY_T:      return "T";
        case KEY_C:      return "C";
        default:         return "NONE";
    }
}

static void key_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask =
            (1ULL << PIN_D0) |
            (1ULL << PIN_D1) |
            (1ULL << PIN_D2) |
            (1ULL << PIN_D3) |
            (1ULL << PIN_D4),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    s_key_output_mutex = xSemaphoreCreateMutex();
    if (s_key_output_mutex == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
}

static void key_output_code(uint8_t key_code)
{
    for (uint8_t bit = 0; bit < KEY_OUTPUT_PIN_COUNT; bit++) {
        const uint8_t level = (key_code >> bit) & 0x01;
        ESP_ERROR_CHECK(gpio_set_level(key_pins[bit], level));
    }
}

static void key_release(void)
{
    key_output_code(KEY_NONE);
}

static void key_print_status(uint8_t key_code)
{
    ESP_LOGI(
        TAG,
        "Xuat phim %-2s | code=%02u | D4 D3 D2 D1 D0 = %u %u %u %u %u",
        key_get_name(key_code),
        key_code,
        (key_code >> 4) & 0x01,
        (key_code >> 3) & 0x01,
        (key_code >> 2) & 0x01,
        (key_code >> 1) & 0x01,
        (key_code >> 0) & 0x01
    );
}

static void key_set_active(uint8_t key_code)
{
    static uint8_t current_key = KEY_NONE;

    if (key_code == current_key) {
        return;
    }

    current_key = key_code;

    if (key_code == KEY_NONE) {
        key_release();
        ESP_LOGI(TAG, "Nha phim ao | D4 D3 D2 D1 D0 = 0 0 0 0 0");
        return;
    }

    key_output_code(key_code);
    key_print_status(key_code);
}

static uint8_t key_code_from_char(char character)
{
    switch (character) {
        case '0': return KEY_0;
        case '1': return KEY_1;
        case '2': return KEY_2;
        case '3': return KEY_3;
        case '4': return KEY_4;
        case '5': return KEY_5;
        case '6': return KEY_6;
        case '7': return KEY_7;
        case '8': return KEY_8;
        case '9': return KEY_9;
        case '#': return KEY_HASH;
        case '$': return KEY_DOLLAR;
        case 'C':
        case 'c': return KEY_C;
        case 'E':
        case 'e': return KEY_E;
        case 'L':
        case 'l': return KEY_L;
        case 'P':
        case 'p': return KEY_P;
        case 'T':
        case 't': return KEY_T;
        case 'V':
        case 'v': return KEY_V;
        default:  return KEY_NONE;
    }
}

static esp_err_t key_press(uint8_t key_code, uint32_t hold_time_ms)
{
    if (key_code < KEY_1 || key_code > KEY_C) {
        ESP_LOGE(TAG, "Ma phim khong hop le: %u", key_code);
        key_set_active(KEY_NONE);
        return ESP_ERR_INVALID_ARG;
    }

    key_set_active(key_code);
    vTaskDelay(pdMS_TO_TICKS(hold_time_ms));
    key_set_active(KEY_NONE);

    return ESP_OK;
}

esp_err_t virtual_key_output_run_sequence(const char *sequence,
                                          uint32_t interval_ms)
{
    if (sequence == NULL || sequence[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_key_output_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_key_output_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    __atomic_store_n(&s_virtual_output_active, true, __ATOMIC_RELEASE);
    esp_err_t result = ESP_OK;

    for (size_t index = 0; sequence[index] != '\0'; index++) {
        uint8_t key_code = key_code_from_char(sequence[index]);
        if (key_code == KEY_NONE) {
            ESP_LOGE(TAG, "Ky tu phim ao khong ho tro: %c", sequence[index]);
            result = ESP_ERR_NOT_SUPPORTED;
            break;
        }

        esp_err_t err = key_press(key_code, AUTO_KEY_HOLD_TIME_MS);
        if (err != ESP_OK) {
            result = err;
            break;
        }

        if (sequence[index + 1U] != '\0') {
            vTaskDelay(pdMS_TO_TICKS(interval_ms));
        }
    }

    key_set_active(KEY_NONE);
    __atomic_store_n(&s_virtual_output_active, false, __ATOMIC_RELEASE);
    xSemaphoreGive(s_key_output_mutex);
    return result;
}

bool virtual_key_output_is_active(void)
{
    return __atomic_load_n(&s_virtual_output_active, __ATOMIC_ACQUIRE);
}

static void auto_key_sequence_task(void *argument)
{
    (void)argument;

    const char *sequence = AUTO_KEY_SEQUENCE;

    ESP_LOGI(
        TAG,
        "Auto key sequence enabled: %s, hold=%ums, delay=%ums",
        sequence,
        (unsigned int)AUTO_KEY_HOLD_TIME_MS,
        (unsigned int)AUTO_KEY_DELAY_BETWEEN_MS
    );

    while (1) {
        for (size_t index = 0; sequence[index] != '\0'; index++) {
            uint8_t key_code = key_code_from_char(sequence[index]);

            if (key_code == KEY_NONE) {
                ESP_LOGW(
                    TAG,
                    "Bo qua ky tu khong ho tro trong chuoi: %c",
                    sequence[index]
                );
                continue;
            }

            ESP_LOGI(
                TAG,
                "Auto nhan ky tu %c -> phim %s",
                sequence[index],
                key_get_name(key_code)
            );

            key_press(key_code, AUTO_KEY_HOLD_TIME_MS);
            vTaskDelay(pdMS_TO_TICKS(AUTO_KEY_DELAY_BETWEEN_MS));
        }

        ESP_LOGI(TAG, "Hoan thanh auto key sequence");
        vTaskDelay(pdMS_TO_TICKS(AUTO_KEY_REPEAT_DELAY_MS));
    }
}

static void auto_key_sequence_start(void)
{
    BaseType_t task_result = xTaskCreate(
        auto_key_sequence_task,
        "auto_key_sequence",
        4096,
        NULL,
        4,
        NULL
    );

    if (task_result != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
}

#if ENABLE_KEYPAD

#define KEYPAD_ROW_COUNT        5
#define KEYPAD_COL_COUNT        5
#define KEYPAD_SCAN_DELAY_MS    1
#define KEYPAD_SCAN_PERIOD_MS   20

#define ROW_1_GPIO      GPIO_NUM_19
#define ROW_2_GPIO      GPIO_NUM_20
#define ROW_3_GPIO      GPIO_NUM_46
#define ROW_4_GPIO      GPIO_NUM_9
#define ROW_5_GPIO      GPIO_NUM_10

#define COL_1_GPIO      GPIO_NUM_21
#define COL_2_GPIO      GPIO_NUM_14
#define COL_3_GPIO      GPIO_NUM_13
#define COL_4_GPIO      GPIO_NUM_12
#define COL_5_GPIO      GPIO_NUM_11

static const gpio_num_t keypad_rows[KEYPAD_ROW_COUNT] = {
    ROW_1_GPIO,
    ROW_2_GPIO,
    ROW_3_GPIO,
    ROW_4_GPIO,
    ROW_5_GPIO
};

static const gpio_num_t keypad_cols[KEYPAD_COL_COUNT] = {
    COL_1_GPIO,
    COL_2_GPIO,
    COL_3_GPIO,
    COL_4_GPIO,
    COL_5_GPIO
};

static const uint8_t keypad_key_map[KEYPAD_ROW_COUNT][KEYPAD_COL_COUNT] = {
    {KEY_HASH,   KEY_P,  KEY_7,  KEY_8,      KEY_9},
    {KEY_E,      KEY_V,  KEY_4,  KEY_5,      KEY_6},
    {KEY_C,      KEY_T,  KEY_1,  KEY_2,      KEY_3},
    {KEY_F4,     KEY_F5, KEY_F6, KEY_DOLLAR, KEY_0},
    {KEY_F1,     KEY_F2, KEY_F3, KEY_L,      KEY_NONE}
};

static void keypad_set_all_rows_low(void)
{
    for (size_t row = 0; row < KEYPAD_ROW_COUNT; row++) {
        ESP_ERROR_CHECK(gpio_set_level(keypad_rows[row], 0));
    }
}

static void keypad_gpio_init(void)
{
    gpio_config_t row_config = {
        .pin_bit_mask =
            (1ULL << ROW_1_GPIO) |
            (1ULL << ROW_2_GPIO) |
            (1ULL << ROW_3_GPIO) |
            (1ULL << ROW_4_GPIO) |
            (1ULL << ROW_5_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&row_config));
    keypad_set_all_rows_low();

    gpio_config_t col_config = {
        .pin_bit_mask =
            (1ULL << COL_1_GPIO) |
            (1ULL << COL_2_GPIO) |
            (1ULL << COL_3_GPIO) |
            (1ULL << COL_4_GPIO) |
            (1ULL << COL_5_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&col_config));

    ESP_LOGI(
        TAG,
        "Keypad 5x5 enabled: rows IO19 IO20 IO46 IO9 IO10, "
        "cols IO21 IO14 IO13 IO12 IO11"
    );
}

static uint32_t keypad_scan_matrix(void)
{
    uint32_t pressed_mask = 0;

    for (size_t row = 0; row < KEYPAD_ROW_COUNT; row++) {
        keypad_set_all_rows_low();
        ESP_ERROR_CHECK(gpio_set_level(keypad_rows[row], 1));
        vTaskDelay(pdMS_TO_TICKS(KEYPAD_SCAN_DELAY_MS));

        for (size_t col = 0; col < KEYPAD_COL_COUNT; col++) {
            if (gpio_get_level(keypad_cols[col]) == 1) {
                pressed_mask |=
                    1UL << ((row * KEYPAD_COL_COUNT) + col);
            }
        }
    }

    keypad_set_all_rows_low();

    return pressed_mask;
}

static uint8_t keypad_get_first_key(uint32_t pressed_mask)
{
    for (size_t row = 0; row < KEYPAD_ROW_COUNT; row++) {
        for (size_t col = 0; col < KEYPAD_COL_COUNT; col++) {
            uint32_t key_bit =
                1UL << ((row * KEYPAD_COL_COUNT) + col);

            if ((pressed_mask & key_bit) != 0) {
                return keypad_key_map[row][col];
            }
        }
    }

    return KEY_NONE;
}

static char keypad_key_to_character(uint8_t key_code)
{
    switch (key_code) {
        case KEY_0:      return '0';
        case KEY_1:      return '1';
        case KEY_2:      return '2';
        case KEY_3:      return '3';
        case KEY_4:      return '4';
        case KEY_5:      return '5';
        case KEY_6:      return '6';
        case KEY_7:      return '7';
        case KEY_8:      return '8';
        case KEY_9:      return '9';
        case KEY_HASH:   return '#';
        case KEY_DOLLAR: return '$';
        case KEY_C:      return 'C';
        case KEY_E:      return 'E';
        case KEY_L:      return 'L';
        case KEY_P:      return 'P';
        case KEY_T:      return 'T';
        case KEY_V:      return 'V';
        default:         return '\0';
    }
}

static bool keypad_find_shortcut(uint8_t key_code,
                                 shortcut_mapping_t *mapping,
                                 calibration_mapping_t *calibration_mapping,
                                 bool *calibration_requested,
                                 bool *totalizer_view_requested)
{
    static char input[SHORTCUT_KEY_MAX_LENGTH + 1U];
    static size_t input_length;
    static TickType_t last_key_tick;

    *calibration_requested = false;
    *totalizer_view_requested = false;

    char character = keypad_key_to_character(key_code);
    if (character == '\0') {
        input_length = 0U;
        input[0] = '\0';
        return false;
    }

    TickType_t now = xTaskGetTickCount();
    if (input_length > 0U &&
        (now - last_key_tick) > pdMS_TO_TICKS(SHORTCUT_INPUT_TIMEOUT_MS)) {
        input_length = 0U;
    }
    last_key_tick = now;

    if (input_length == SHORTCUT_KEY_MAX_LENGTH) {
        memmove(input, input + 1U, SHORTCUT_KEY_MAX_LENGTH - 1U);
        input_length--;
    }
    input[input_length++] = character;
    input[input_length] = '\0';

    if (input_length >= 3U &&
        strcmp(input + input_length - 3U, "P88") == 0) {
        input_length = 0U;
        input[0] = '\0';
        *totalizer_view_requested = true;
        ESP_LOGI(TAG, "Da decode du chuoi vat ly P88");
        return false;
    }

    if (input[input_length - 1U] == 'P' ||
        (input_length >= 2U &&
         strcmp(input + input_length - 2U, "P8") == 0)) {
        return false;
    }

    if (device_settings_find_calibration_suffix(input,
                                                calibration_mapping)) {
        ESP_LOGI(TAG, "Nhan calibration shortcut vat ly %s -> %s",
                 calibration_mapping->name,
                 calibration_mapping->raw_command);
        input_length = 0U;
        input[0] = '\0';
        *calibration_requested = true;
        return false;
    }

    if (!device_settings_find_shortcut_suffix(input, mapping)) {
        return false;
    }

    ESP_LOGI(TAG, "Nhan shortcut vat ly %s -> %s",
             mapping->shortcut_key, mapping->physical_key);
    input_length = 0U;
    input[0] = '\0';
    return true;
}

static bool shortcut_output_queue(const shortcut_mapping_t *mapping)
{
    if (s_shortcut_output_queue == NULL || mapping == NULL) {
        return false;
    }

    shortcut_output_message_t message = {0};
    message.type = SHORTCUT_OUTPUT_SEQUENCE;
    strlcpy(message.sequence, mapping->physical_key,
            sizeof(message.sequence));
    if (xQueueSend(s_shortcut_output_queue, &message, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Shortcut output queue day, bo qua %s",
                 mapping->shortcut_key);
        return false;
    }
    return true;
}

static bool calibration_output_queue(
    const calibration_mapping_t *mapping)
{
    if (s_shortcut_output_queue == NULL || mapping == NULL) {
        return false;
    }

    shortcut_output_message_t message = {
        .type = SHORTCUT_OUTPUT_CALIBRATION,
    };
    strlcpy(message.sequence, mapping->raw_command,
            sizeof(message.sequence));
    strlcpy(message.mode_calibration, mapping->name,
            sizeof(message.mode_calibration));
    if (xQueueSend(s_shortcut_output_queue, &message, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Shortcut output queue day, bo qua calibration %s",
                 mapping->name);
        return false;
    }
    return true;
}

static void shortcut_output_task(void *argument)
{
    (void)argument;
    shortcut_output_message_t message;

    while (1) {
        if (xQueueReceive(s_shortcut_output_queue, &message,
                          portMAX_DELAY) == pdTRUE) {
            esp_err_t err;
            if (message.type == SHORTCUT_OUTPUT_CALIBRATION) {
                ESP_LOGI(TAG, "Bat dau calibration local: %s",
                         message.sequence);
                err = mqtt_command_handler_execute_local_calibration(
                    message.mode_calibration, message.sequence);
            } else {
                ESP_LOGI(TAG, "Bat dau phat shortcut: %s", message.sequence);
                err = virtual_key_output_run_sequence(
                    message.sequence, SHORTCUT_OUTPUT_INTERVAL_MS);
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Xu ly shortcut that bai: %s",
                         esp_err_to_name(err));
            } else if (message.type == SHORTCUT_OUTPUT_CALIBRATION) {
                ESP_LOGI(TAG, "Calibration local hoan tat: %s",
                         message.sequence);
            }
        }
    }
}

static esp_err_t shortcut_output_start(void)
{
    s_shortcut_output_queue = xQueueCreate(
        SHORTCUT_OUTPUT_QUEUE_LENGTH, sizeof(shortcut_output_message_t));
    if (s_shortcut_output_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(shortcut_output_task, "shortcut_output", 4096,
                    NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_shortcut_output_queue);
        s_shortcut_output_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static uint8_t display_digit_segments(char digit)
{
    static const uint8_t segments[] = {
        0x3FU, 0x06U, 0x5BU, 0x4FU, 0x66U,
        0x6DU, 0x7DU, 0x07U, 0x7FU, 0x6FU
    };
    return segments[(size_t)(digit - '0')];
}

static bool numeric_text_to_segments(const char *text,
                                     uint8_t *segments,
                                     size_t *segment_count)
{
    size_t count = 0U;
    for (size_t index = 0U; text[index] != '\0'; index++) {
        if (text[index] >= '0' && text[index] <= '9') {
            if (count >= TOTALIZER_VIEW_MAX_SEGMENTS) {
                return false;
            }
            segments[count++] = display_digit_segments(text[index]);
        } else if (text[index] == '.' && count > 0U &&
                   (segments[count - 1U] & 0x80U) == 0U) {
            segments[count - 1U] |= 0x80U;
        } else {
            return false;
        }
    }
    if (count == 0U) {
        return false;
    }
    *segment_count = count;
    return true;
}

static bool format_amount_vnd(uint64_t amount_vnd, char *output,
                              size_t output_size)
{
    char digits[32];
    int digit_count = snprintf(digits, sizeof(digits), "%" PRIu64, amount_vnd);
    if (digit_count <= 0 || (size_t)digit_count >= sizeof(digits)) {
        return false;
    }

    size_t separator_count = ((size_t)digit_count - 1U) / 3U;
    if ((size_t)digit_count + separator_count + 1U > output_size) {
        return false;
    }

    size_t output_index = 0U;
    for (size_t index = 0U; index < (size_t)digit_count; index++) {
        output[output_index++] = digits[index];
        size_t remaining_digits = (size_t)digit_count - index - 1U;
        if (remaining_digits > 0U && (remaining_digits % 3U) == 0U) {
            output[output_index++] = '.';
        }
    }
    output[output_index] = '\0';
    return true;
}

static bool render_totalizer_display(
    const uint8_t *amount_segments,
    size_t amount_count,
    const uint8_t *volume_segments,
    size_t volume_count,
    uint8_t output[CONTROL_DISPLAY_ROWS][CONTROL_DISPLAY_COLUMNS])
{
    if (amount_count > CONTROL_DISPLAY_COLUMNS ||
        volume_count > CONTROL_DISPLAY_COLUMNS) {
        return false;
    }

    memset(output, 0, CONTROL_DISPLAY_ROWS * CONTROL_DISPLAY_COLUMNS);
    memcpy(&output[0][CONTROL_DISPLAY_COLUMNS - amount_count],
           amount_segments, amount_count);
    memcpy(&output[2][CONTROL_DISPLAY_COLUMNS - volume_count],
           volume_segments, volume_count);
    return true;
}

static bool wifi_status_led_is_requested(void)
{
    wifi_manager_result_t result = wifi_manager_get_connection_result();
    return wifi_manager_is_config_mode() ||
           result == WIFI_MANAGER_RESULT_DONE ||
           result == WIFI_MANAGER_RESULT_FAIL;
}

static bool totalizer_view_trigger(void)
{
    bool expected = false;
    if (s_totalizer_view_task_handle == NULL ||
        wifi_status_led_is_requested() ||
        !__atomic_compare_exchange_n(&s_totalizer_view_active, &expected, true,
                                     false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        return false;
    }

    __atomic_store_n(&s_totalizer_view_exit_requested, false,
                     __ATOMIC_RELEASE);
    xTaskNotifyGive(s_totalizer_view_task_handle);
    return true;
}

static void totalizer_view_request_exit(void)
{
    __atomic_store_n(&s_totalizer_view_exit_requested, true,
                     __ATOMIC_RELEASE);
}

static void totalizer_view_task(void *argument)
{
    (void)argument;

    while (1) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint64_t total_amount_vnd = 0U;
        double total_volume_l = 0.0;
        esp_err_t err = read_totalizer(&total_amount_vnd, &total_volume_l);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "P88 doc Totalizer that bai: %s",
                     esp_err_to_name(err));
            __atomic_store_n(&s_totalizer_view_active, false,
                             __ATOMIC_RELEASE);
            continue;
        }
        if (__atomic_load_n(&s_totalizer_view_exit_requested,
                            __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&s_totalizer_view_active, false,
                             __ATOMIC_RELEASE);
            continue;
        }

        char amount_text[32];
        char volume_text[32];
        int volume_length = snprintf(volume_text, sizeof(volume_text),
                                     "%.3f", total_volume_l);
        if (!format_amount_vnd(total_amount_vnd, amount_text,
                               sizeof(amount_text)) ||
            volume_length <= 0 || (size_t)volume_length >= sizeof(volume_text) ||
            !isfinite(total_volume_l)) {
            ESP_LOGE(TAG, "P88 khong format duoc du lieu Totalizer");
            __atomic_store_n(&s_totalizer_view_active, false,
                             __ATOMIC_RELEASE);
            continue;
        }

        char *decimal = strchr(volume_text, '.');
        char *end = volume_text + strlen(volume_text) - 1U;
        while (decimal != NULL && end > decimal + 1U && *end == '0') {
            *end-- = '\0';
        }

        uint8_t amount_segments[TOTALIZER_VIEW_MAX_SEGMENTS];
        uint8_t volume_segments[TOTALIZER_VIEW_MAX_SEGMENTS];
        size_t amount_count = 0U;
        size_t volume_count = 0U;
        if (!numeric_text_to_segments(amount_text, amount_segments,
                                      &amount_count) ||
            !numeric_text_to_segments(volume_text, volume_segments,
                                      &volume_count)) {
            ESP_LOGE(TAG, "P88 du lieu Totalizer vuot kha nang hien thi");
            __atomic_store_n(&s_totalizer_view_active, false,
                             __ATOMIC_RELEASE);
            continue;
        }

        uint8_t display[CONTROL_DISPLAY_ROWS][CONTROL_DISPLAY_COLUMNS];
        if (!render_totalizer_display(amount_segments, amount_count,
                                      volume_segments, volume_count,
                                      display)) {
            ESP_LOGE(TAG, "P88 moi gia tri chi duoc toi da %u LED",
                     (unsigned)CONTROL_DISPLAY_COLUMNS);
            __atomic_store_n(&s_totalizer_view_active, false,
                             __ATOMIC_RELEASE);
            continue;
        }
        if (wifi_status_led_is_requested() ||
            __atomic_load_n(&s_totalizer_view_exit_requested,
                            __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&s_totalizer_view_active, false,
                             __ATOMIC_RELEASE);
            continue;
        }
        err = control_display_led_begin_virtual(display);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Khong bat duoc Virtual LED: %s",
                     esp_err_to_name(err));
            __atomic_store_n(&s_totalizer_view_active, false,
                             __ATOMIC_RELEASE);
            continue;
        }

        err = gpio_set_level(ENABLE_VIRTUAL_LED_GPIO, 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Khong bat duoc IO39: %s", esp_err_to_name(err));
            control_display_led_end_virtual();
            __atomic_store_n(&s_totalizer_view_active, false,
                             __ATOMIC_RELEASE);
            continue;
        }
        ESP_LOGI(TAG, "P88 hien thi Totalizer: amount=%" PRIu64
                      " volume=%.3f, IO39=1",
                 total_amount_vnd, total_volume_l);

        while (!__atomic_load_n(&s_totalizer_view_exit_requested,
                                __ATOMIC_ACQUIRE)) {
            vTaskDelay(pdMS_TO_TICKS(TOTALIZER_VIEW_POLL_MS));
        }

        bool wifi_status_requested = wifi_status_led_is_requested();
        if (!wifi_status_requested) {
            err = gpio_set_level(ENABLE_VIRTUAL_LED_GPIO, 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Khong tra duoc IO39 ve 0: %s",
                         esp_err_to_name(err));
            }
            control_display_led_end_virtual();
        }
        __atomic_store_n(&s_totalizer_view_exit_requested, false,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&s_totalizer_view_active, false,
                         __ATOMIC_RELEASE);
        if (wifi_status_requested) {
            ESP_LOGI(TAG, "Thoat P88, giu quyen LED cho trang thai Wi-Fi");
        } else {
            ESP_LOGI(TAG, "Thoat P88, IO39=0, tra man hinh ve mach ngoai");
        }
    }
}

static esp_err_t totalizer_view_start(void)
{
    if (xTaskCreate(totalizer_view_task, "totalizer_view",
                    TOTALIZER_VIEW_TASK_STACK_SIZE, NULL, 4,
                    &s_totalizer_view_task_handle) != pdPASS) {
        s_totalizer_view_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void wifi_status_led_task(void *argument)
{
    (void)argument;

    static const uint8_t config_display
        [CONTROL_DISPLAY_ROWS][CONTROL_DISPLAY_COLUMNS] = {
        /* C, O, n, F, I, G */
        {0x39U, 0x3FU, 0x54U, 0x71U, 0x06U, 0x6FU},
        {0U, 0U, 0U, 0U, 0U, 0U},
        {0U, 0U, 0U, 0U, 0U, 0U},
    };
    static const uint8_t done_display
        [CONTROL_DISPLAY_ROWS][CONTROL_DISPLAY_COLUMNS] = {
        /* d, o, n, E, centered on row 1. */
        {0U, 0x5EU, 0x5CU, 0x54U, 0x79U, 0U},
        {0U, 0U, 0U, 0U, 0U, 0U},
        {0U, 0U, 0U, 0U, 0U, 0U},
    };
    static const uint8_t fail_display
        [CONTROL_DISPLAY_ROWS][CONTROL_DISPLAY_COLUMNS] = {
        /* F, A, i, L, centered on row 1. */
        {0U, 0x71U, 0x77U, 0x30U, 0x38U, 0U},
        {0U, 0U, 0U, 0U, 0U, 0U},
        {0U, 0U, 0U, 0U, 0U, 0U},
    };

    TickType_t result_display_started = 0;

    while (true) {
        bool config_mode = wifi_manager_is_config_mode();
        wifi_manager_result_t connection_result =
            wifi_manager_get_connection_result();
        wifi_status_led_view_t desired_view = WIFI_STATUS_LED_NONE;
        const uint8_t (*desired_segments)[CONTROL_DISPLAY_COLUMNS] = NULL;

        if (connection_result == WIFI_MANAGER_RESULT_DONE) {
            desired_view = WIFI_STATUS_LED_DONE;
            desired_segments = done_display;
        } else if (connection_result == WIFI_MANAGER_RESULT_FAIL) {
            desired_view = WIFI_STATUS_LED_FAIL;
            desired_segments = fail_display;
        } else if (config_mode) {
            desired_view = WIFI_STATUS_LED_CONFIG;
            desired_segments = config_display;
        }

        wifi_status_led_view_t current_view = __atomic_load_n(
            &s_wifi_status_led_view, __ATOMIC_ACQUIRE);
        if (desired_view != current_view &&
            desired_view != WIFI_STATUS_LED_NONE) {
            if (__atomic_load_n(&s_totalizer_view_active,
                                __ATOMIC_ACQUIRE)) {
                totalizer_view_request_exit();
            }

            esp_err_t err = current_view == WIFI_STATUS_LED_NONE
                                ? control_display_led_begin_virtual(
                                      desired_segments)
                                : control_display_led_set_virtual_segments(
                                      desired_segments);
            if (err == ESP_OK) {
                err = gpio_set_level(ENABLE_VIRTUAL_LED_GPIO, 1);
            }
            if (err == ESP_OK) {
                __atomic_store_n(&s_wifi_status_led_view, desired_view,
                                 __ATOMIC_RELEASE);
                if (desired_view == WIFI_STATUS_LED_CONFIG) {
                    ESP_LOGI(TAG, "WiFi Config Mode: hien thi CONFIG, IO39=1");
                } else {
                    result_display_started = xTaskGetTickCount();
                    if (desired_view == WIFI_STATUS_LED_DONE) {
                        ESP_LOGI(TAG, "Kiem tra Wi-Fi: hien thi donE, IO39=1");
                    } else {
                        ESP_LOGI(TAG, "Kiem tra Wi-Fi: hien thi FAiL, IO39=1");
                    }
                }
            } else {
                if (current_view == WIFI_STATUS_LED_NONE) {
                    control_display_led_end_virtual();
                }
                ESP_LOGE(TAG, "Khong hien thi duoc trang thai Wi-Fi: %s",
                         esp_err_to_name(err));
            }
        } else if (desired_view != WIFI_STATUS_LED_NONE) {
            esp_err_t err = control_display_led_set_virtual_segments(
                desired_segments);
            if (err != ESP_OK) {
                __atomic_store_n(&s_wifi_status_led_view,
                                 WIFI_STATUS_LED_NONE,
                                 __ATOMIC_RELEASE);
                ESP_LOGW(TAG, "Mat trang thai LED Wi-Fi, se khoi phuc: %s",
                         esp_err_to_name(err));
            }
        } else if (current_view != WIFI_STATUS_LED_NONE) {
            esp_err_t err = gpio_set_level(ENABLE_VIRTUAL_LED_GPIO, 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Khong tra duoc IO39 ve 0: %s",
                         esp_err_to_name(err));
            }
            control_display_led_end_virtual();
            __atomic_store_n(&s_wifi_status_led_view,
                             WIFI_STATUS_LED_NONE,
                             __ATOMIC_RELEASE);
            ESP_LOGI(TAG, "Wi-Fi da ket noi, tat CONFIG va tra IO39=0");
        }

        current_view = __atomic_load_n(&s_wifi_status_led_view,
                                       __ATOMIC_ACQUIRE);
        if ((current_view == WIFI_STATUS_LED_DONE ||
             current_view == WIFI_STATUS_LED_FAIL) &&
            (xTaskGetTickCount() - result_display_started) >=
                pdMS_TO_TICKS(WIFI_CONNECTION_RESULT_DISPLAY_MS)) {
            esp_err_t err = gpio_set_level(ENABLE_VIRTUAL_LED_GPIO, 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Khong tra duoc IO39 ve 0: %s",
                         esp_err_to_name(err));
            }
            control_display_led_end_virtual();
            __atomic_store_n(&s_wifi_status_led_view,
                             WIFI_STATUS_LED_NONE,
                             __ATOMIC_RELEASE);

            wifi_manager_acknowledge_connection_result();
            ESP_LOGI(TAG, "Ket thuc thong bao Wi-Fi, tra IO39=0");
        }

        vTaskDelay(pdMS_TO_TICKS(WIFI_CONFIG_LED_POLL_MS));
    }
}

static esp_err_t wifi_status_led_start(void)
{
    if (xTaskCreate(wifi_status_led_task, "wifi_status_led",
                    WIFI_CONFIG_LED_TASK_STACK_SIZE, NULL, 4,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void keypad_log_changed_keys(uint32_t old_mask, uint32_t new_mask)
{
    uint32_t changed_mask = old_mask ^ new_mask;

    for (size_t row = 0; row < KEYPAD_ROW_COUNT; row++) {
        for (size_t col = 0; col < KEYPAD_COL_COUNT; col++) {
            uint32_t key_bit =
                1UL << ((row * KEYPAD_COL_COUNT) + col);

            if ((changed_mask & key_bit) == 0) {
                continue;
            }

            uint8_t key_code = keypad_key_map[row][col];

            ESP_LOGI(
                TAG,
                "%s nut ROW_%u COL_%u -> phim %s",
                (new_mask & key_bit) ? "Nhan" : "Nha",
                (unsigned int)(row + 1),
                (unsigned int)(col + 1),
                key_get_name(key_code)
            );
        }
    }
}

static void keypad_scan_task(void *argument)
{
    (void)argument;

    uint32_t stable_mask = 0;
    uint32_t last_sample_mask = 0;
    bool shortcut_pending = false;
    bool calibration_pending = false;
    bool totalizer_view_pending = false;
    shortcut_mapping_t pending_mapping = {0};
    calibration_mapping_t pending_calibration = {0};

    while (1) {
        uint32_t sample_mask = keypad_scan_matrix();

        if (sample_mask == last_sample_mask &&
            sample_mask != stable_mask) {

            keypad_log_changed_keys(stable_mask, sample_mask);
            stable_mask = sample_mask;

#if COMBINES_KEYPAD_FUNTIONS
            uint8_t physical_key = keypad_get_first_key(stable_mask);
            if (physical_key == KEY_HASH &&
                device_settings_is_hash_key_locked()) {
                ESP_LOGW(TAG, "Phim # vat ly dang bi khoa, bo qua lan nhan");
                physical_key = KEY_NONE;
            }

            if (__atomic_load_n(&s_totalizer_view_active,
                                __ATOMIC_ACQUIRE)) {
                if (!virtual_key_output_is_active()) {
                    key_set_active(KEY_NONE);
                }
                shortcut_pending = false;
                calibration_pending = false;
                totalizer_view_pending = false;
                if (physical_key != KEY_NONE) {
                    ESP_LOGI(TAG, "Nhan phim %s, yeu cau thoat P88",
                             key_get_name(physical_key));
                    totalizer_view_request_exit();
                }
            } else if (!virtual_key_output_is_active()) {
                key_set_active(physical_key);
                if (physical_key != KEY_NONE) {
                    shortcut_pending = keypad_find_shortcut(
                        physical_key, &pending_mapping,
                        &pending_calibration, &calibration_pending,
                        &totalizer_view_pending);
                } else if (totalizer_view_pending) {
                    if (!totalizer_view_trigger()) {
                        ESP_LOGW(TAG, "Khong the bat che do xem P88");
                    }
                    totalizer_view_pending = false;
                    calibration_pending = false;
                    shortcut_pending = false;
                } else if (calibration_pending) {
                    (void)calibration_output_queue(&pending_calibration);
                    calibration_pending = false;
                    shortcut_pending = false;
                } else if (shortcut_pending) {
                    (void)shortcut_output_queue(&pending_mapping);
                    shortcut_pending = false;
                }
            }
#endif
        }

        last_sample_mask = sample_mask;

        vTaskDelay(pdMS_TO_TICKS(KEYPAD_SCAN_PERIOD_MS));
    }
}

static esp_err_t initialize_nvs_before_network(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS can khoi tao lai truoc khi ap dung mode boot");
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

static esp_err_t reapply_calibration_mode_on_boot(void)
{
    char mode[SHORTCUT_KEY_MAX_LENGTH + 1U] = {0};
    esp_err_t err = device_settings_get_mode_calibration(mode, sizeof(mode));
    if (err != ESP_OK) {
        return err;
    }

    calibration_mapping_t mapping = {0};
    bool mapping_found = false;
    for (uint8_t slot = 1U; slot <= CALIBRATION_MAPPING_MAX_COUNT; slot++) {
        if (device_settings_get_calibration_mapping_at(slot, &mapping) ==
                ESP_OK &&
            strcmp(mapping.name, mode) == 0) {
            mapping_found = true;
            break;
        }
    }
    if (!mapping_found) {
        ESP_LOGW(TAG,
                 "Mode boot %s chua co calibration mapping, bo qua ap dung lai",
                 mode);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Ap dung lai mode boot %s voi raw_command=%s",
             mode, mapping.raw_command);
    err = execute_calibration_command(mapping.raw_command);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Da ap dung lai mode boot %s truoc khi bat Wi-Fi", mode);
    } else {
        ESP_LOGE(TAG, "Ap dung lai mode boot %s that bai: %s",
                 mode, esp_err_to_name(err));
    }
    return err;
}

void app_main(void)
{
    virtual_led_enable_init();

    esp_err_t nvs_result = initialize_nvs_before_network();
    if (nvs_result != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(nvs_result));
    }

    esp_err_t settings_result = device_settings_init();
    if (settings_result != ESP_OK) {
        ESP_LOGE(TAG, "Device settings init failed: %s",
                 esp_err_to_name(settings_result));
    }

#if ENABLE_PUMP_DATA_SNIFFER
    pump_data_sniffer_start();
#endif

    keypad_gpio_init();

#if COMBINES_KEYPAD_FUNTIONS
    key_gpio_init();
    key_release();
    ESP_LOGI(TAG, "Combine keypad functions enabled");

    esp_err_t shortcut_result = shortcut_output_start();
    if (shortcut_result != ESP_OK) {
        ESP_LOGE(TAG, "Shortcut output init failed: %s",
                 esp_err_to_name(shortcut_result));
    }
#endif

    esp_err_t command_result = mqtt_command_handler_start();
    if (command_result != ESP_OK) {
        ESP_LOGE(TAG, "MQTT command handler init failed: %s",
                 esp_err_to_name(command_result));
    }

    if (settings_result == ESP_OK && command_result == ESP_OK) {
        (void)reapply_calibration_mode_on_boot();
    }

    esp_err_t wifi_result = wifi_manager_start();
    if (wifi_result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi manager init failed: %s", esp_err_to_name(wifi_result));
    }

    esp_err_t time_result = time_manager_start();
    if (time_result != ESP_OK) {
        ESP_LOGE(TAG, "Time manager init failed: %s", esp_err_to_name(time_result));
    }

    esp_err_t mqtt_result = mqtt_manager_start();
    if (mqtt_result != ESP_OK) {
        ESP_LOGE(TAG, "MQTT manager init failed: %s", esp_err_to_name(mqtt_result));
    }

    esp_err_t ota_result = ota_manager_start();
    if (ota_result != ESP_OK) {
        ESP_LOGE(TAG, "OTA manager init failed: %s",
                 esp_err_to_name(ota_result));
    }

    esp_err_t heartbeat_result = telemetry_heartbeat_start();
    if (heartbeat_result != ESP_OK) {
        ESP_LOGE(TAG, "Telemetry heartbeat init failed: %s",
                 esp_err_to_name(heartbeat_result));
    }

    esp_err_t totalizer_view_result = totalizer_view_start();
    if (totalizer_view_result != ESP_OK) {
        ESP_LOGE(TAG, "Totalizer view init failed: %s",
                 esp_err_to_name(totalizer_view_result));
    }

    esp_err_t config_led_result = wifi_status_led_start();
    if (config_led_result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi config LED init failed: %s",
                 esp_err_to_name(config_led_result));
    }

#if ENABLE_AUTO_KEY_SEQUENCE
    auto_key_sequence_start();
#endif

    BaseType_t task_result = xTaskCreate(
        keypad_scan_task,
        "keypad_scan_task",
        4096,
        NULL,
        5,
        NULL
    );

    if (task_result != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
}

#else

#define KEY_PRESS_TIME_MS       500
#define KEY_INTERVAL_MS         2000

static const uint8_t key_sequence[] = {
    KEY_C,
    KEY_HASH,
    KEY_4,
    KEY_4,
    KEY_5,
    KEY_0,
    KEY_4,
    KEY_E,
    KEY_T
};

void app_main(void)
{
    virtual_led_enable_init();

    esp_err_t wifi_result = wifi_manager_start();
    if (wifi_result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi manager init failed: %s", esp_err_to_name(wifi_result));
    }

    esp_err_t settings_result = device_settings_init();
    if (settings_result != ESP_OK) {
        ESP_LOGE(TAG, "Device settings init failed: %s",
                 esp_err_to_name(settings_result));
    }

    esp_err_t time_result = time_manager_start();
    if (time_result != ESP_OK) {
        ESP_LOGE(TAG, "Time manager init failed: %s", esp_err_to_name(time_result));
    }

    esp_err_t mqtt_result = mqtt_manager_start();
    if (mqtt_result != ESP_OK) {
        ESP_LOGE(TAG, "MQTT manager init failed: %s", esp_err_to_name(mqtt_result));
    }

    esp_err_t ota_result = ota_manager_start();
    if (ota_result != ESP_OK) {
        ESP_LOGE(TAG, "OTA manager init failed: %s",
                 esp_err_to_name(ota_result));
    }

    esp_err_t heartbeat_result = telemetry_heartbeat_start();
    if (heartbeat_result != ESP_OK) {
        ESP_LOGE(TAG, "Telemetry heartbeat init failed: %s",
                 esp_err_to_name(heartbeat_result));
    }

#if ENABLE_PUMP_DATA_SNIFFER
    pump_data_sniffer_start();
#endif

    key_gpio_init();
    key_release();

    esp_err_t command_result = mqtt_command_handler_start();
    if (command_result != ESP_OK) {
        ESP_LOGE(TAG, "MQTT command handler init failed: %s",
                 esp_err_to_name(command_result));
    }

#if ENABLE_AUTO_KEY_SEQUENCE
    auto_key_sequence_start();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

    const size_t key_count =
        sizeof(key_sequence) / sizeof(key_sequence[0]);

    ESP_LOGI(TAG, "So phim trong chuoi: %u", (unsigned int)key_count);

    while (1) {
        for (size_t i = 0; i < key_count; i++) {
            TickType_t cycle_start = xTaskGetTickCount();

            key_press(key_sequence[i], KEY_PRESS_TIME_MS);

            vTaskDelayUntil(
                &cycle_start,
                pdMS_TO_TICKS(KEY_INTERVAL_MS)
            );
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG, "Hoan thanh chuoi, bat dau lap lai");
    }
}

#endif
