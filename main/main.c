#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#include "pump_data_sniffer.h"

static const char *TAG = "KEY_OUTPUT";

#define ENABLE_KEYPAD               1
#define COMBINES_KEYPAD_FUNTIONS    1
#define ENABLE_PUMP_DATA_SNIFFER    1
#define ENABLE_AUTO_KEY_SEQUENCE    1

#define KEY_OUTPUT_PIN_COUNT        5

#define ENABLE_VIRTUAL_LED_GPIO     GPIO_NUM_39

#define AUTO_KEY_SEQUENCE              "#122973E2522EC"
#define AUTO_KEY_HOLD_TIME_MS          50
#define AUTO_KEY_DELAY_BETWEEN_MS      50
#define AUTO_KEY_REPEAT_DELAY_MS       2000

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

    while (1) {
        uint32_t sample_mask = keypad_scan_matrix();

        if (sample_mask == last_sample_mask &&
            sample_mask != stable_mask) {

            keypad_log_changed_keys(stable_mask, sample_mask);
            stable_mask = sample_mask;

#if COMBINES_KEYPAD_FUNTIONS
            key_set_active(keypad_get_first_key(stable_mask));
#endif
        }

        last_sample_mask = sample_mask;

        vTaskDelay(pdMS_TO_TICKS(KEYPAD_SCAN_PERIOD_MS));
    }
}

void app_main(void)
{
    virtual_led_enable_init();

#if ENABLE_PUMP_DATA_SNIFFER
    pump_data_sniffer_start();
#endif

    keypad_gpio_init();

#if COMBINES_KEYPAD_FUNTIONS
    key_gpio_init();
    key_release();
    ESP_LOGI(TAG, "Combine keypad functions enabled");
#endif

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

#if ENABLE_PUMP_DATA_SNIFFER
    pump_data_sniffer_start();
#endif

    key_gpio_init();
    key_release();

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
