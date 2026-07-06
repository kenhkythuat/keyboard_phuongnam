#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "esp_err.h"

/* =========================================================
 * Káº¾T Ná»I ESP32-S3 -> MBI5026
 * ========================================================= */
#define PIN_SDI     GPIO_NUM_37
#define PIN_OE      GPIO_NUM_38
#define PIN_CLK     GPIO_NUM_36

/*
 * Báº¡n chÆ°a cung cáº¥p chÃ¢n LE.
 * Táº¡m thá»i dÃ¹ng GPIO39, hÃ£y sá»­a láº¡i Ä‘Ãºng theo PCB.
 */
#define PIN_LE      GPIO_NUM_35

#define DISPLAY_ROWS       3
#define DISPLAY_COLUMNS    6

/*
 * 1: QuÃ©t tá»«ng LED riÃªng biá»‡t, tá»•ng cá»™ng 18 slot.
 *    An toÃ n hÆ¡n cho transistor cá»™t vÃ¬ chá»‰ má»™t hÃ ng sÃ¡ng táº¡i má»™t thá»i Ä‘iá»ƒm.
 *
 * 0: QuÃ©t 6 cá»™t, má»—i cá»™t lÃ m sÃ¡ng Ä‘á»“ng thá»i cáº£ 3 hÃ ng.
 *    SÃ¡ng hÆ¡n nhÆ°ng dÃ²ng qua transistor cá»™t cÃ³ thá»ƒ ráº¥t lá»›n.
 */
#define SAFE_ONE_DIGIT_SCAN    1

#if SAFE_ONE_DIGIT_SCAN
    /*
     * 18 slot x 500 us = 9 ms/frame
     * Táº§n sá»‘ lÃ m tÆ°Æ¡i khoáº£ng 111 Hz.
     */
    #define SCAN_PERIOD_US     500
#else
    /*
     * 6 slot x 1000 us = 6 ms/frame
     * Táº§n sá»‘ lÃ m tÆ°Æ¡i khoáº£ng 166 Hz.
     */
    #define SCAN_PERIOD_US     1000
#endif

static const char *TAG = "MBI5026_7SEG";

/* =========================================================
 * QUY Æ¯á»šC BIT SEGMENT
 *
 * bit 0 = A
 * bit 1 = B
 * bit 2 = C
 * bit 3 = D
 * bit 4 = E
 * bit 5 = F
 * bit 6 = G
 * bit 7 = DP
 *
 * MBI5026 lÃ  current sink:
 * bit = 1 -> output sink hoáº¡t Ä‘á»™ng -> segment sÃ¡ng.
 * ========================================================= */
/* MÃ£ hiá»ƒn thá»‹ sá»‘ 0...9, active-high Ä‘á»‘i vá»›i dá»¯ liá»‡u MBI5026 */
/*
 * display_buffer[row][column]
 *
 * row 0:
 * LED1, LED2, LED3, LED4, LED5, LED6
 *
 * row 1:
 * LED7, LED8, LED9, LED10, LED11, LED12
 *
 * row 2:
 * LED13, LED14, LED15, LED16, LED17, LED18
 *
 * Má»—i pháº§n tá»­ chá»©a mÃ£ segment A...G, DP.
 */
static uint8_t display_buffer[DISPLAY_ROWS][DISPLAY_COLUMNS];

static portMUX_TYPE display_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t scan_timer_handle;

/* =========================================================
 * Gá»¬I Dá»® LIá»†U CHO MBI5026
 * ========================================================= */

/*
 * Gá»­i 16 bit, bit 15 trÆ°á»›c.
 *
 * Vá»›i cÃ¡ch dá»‹ch nÃ y:
 * bit 0 cá»§a value  -> OUT0
 * bit 1           -> OUT1
 * ...
 * bit 15          -> OUT15
 */
static inline void mbi5026_shift_word_msb(uint16_t value)
{
    for (int bit = 15; bit >= 0; bit--) {
        gpio_set_level(PIN_SDI, (value >> bit) & 0x01U);

        gpio_set_level(PIN_CLK, 1);

        /*
         * MBI5026 há»— trá»£ CLK khÃ¡ cao.
         * Delay 1 us dÃ¹ng Ä‘á»ƒ test pháº§n cá»©ng á»•n Ä‘á»‹nh, dá»… Ä‘o oscilloscope.
         */
        esp_rom_delay_us(1);

        gpio_set_level(PIN_CLK, 0);
    }
}

/*
 * ESP32 -> SDI U1 -> SDO U1 -> SDI U2
 *
 * Sau khi gá»­i Ä‘á»§ 32 bit:
 * - 16 bit gá»­i Ä‘áº§u tiÃªn náº±m trong U2.
 * - 16 bit gá»­i sau náº±m trong U1.
 *
 * Do Ä‘Ã³ pháº£i gá»­i U2 trÆ°á»›c, U1 sau.
 */
static inline void mbi5026_load_words(uint16_t u1_word, uint16_t u2_word)
{
    /* Táº¯t toÃ n bá»™ output Ä‘á»ƒ trÃ¡nh nhÃ¡y sai hoáº·c ghosting */
    gpio_set_level(PIN_OE, 1);

    gpio_set_level(PIN_LE, 0);
    gpio_set_level(PIN_CLK, 0);

    /* IC náº±m xa ESP32 hÆ¡n pháº£i gá»­i trÆ°á»›c */
    mbi5026_shift_word_msb(u2_word);

    /* IC Ä‘áº§u tiÃªn trong chuá»—i gá»­i sau */
    mbi5026_shift_word_msb(u1_word);

    /*
     * LE high: dá»¯ liá»‡u Ä‘Æ°á»£c truyá»n sang output latch.
     * LE low: giá»¯ dá»¯ liá»‡u.
     */
    gpio_set_level(PIN_LE, 1);
    esp_rom_delay_us(1);

    gpio_set_level(PIN_LE, 0);
    esp_rom_delay_us(1);
}

/* =========================================================
 * CALLBACK QUÃ‰T LED
 * ========================================================= */

static void scan_timer_callback(void *arg)
{
    (void)arg;

#if SAFE_ONE_DIGIT_SCAN

    /*
     * QuÃ©t 18 vá»‹ trÃ­:
     *
     * slot 0...5   = hÃ ng 1, cá»™t 1...6
     * slot 6...11  = hÃ ng 2, cá»™t 1...6
     * slot 12...17 = hÃ ng 3, cá»™t 1...6
     */
    static uint8_t scan_slot = 0;

    uint8_t row = scan_slot / DISPLAY_COLUMNS;
    uint8_t column = scan_slot % DISPLAY_COLUMNS;
    uint8_t segment_data;

    portENTER_CRITICAL(&display_lock);
    segment_data = display_buffer[row][column];
    portEXIT_CRITICAL(&display_lock);

    uint16_t u1_word = 0;
    uint16_t u2_word = 0;

    /*
     * Chá»‰ báº­t segment cá»§a má»™t hÃ ng táº¡i má»—i slot.
     * Hai hÃ ng cÃ²n láº¡i cÃ³ segment = 0.
     */
    if (row == 0) {
        /* U1 OUT0...OUT7 = hÃ ng 1 */
        u1_word = segment_data;
    } else if (row == 1) {
        /* U1 OUT8...OUT15 = hÃ ng 2 */
        u1_word = ((uint16_t)segment_data << 8);
    } else {
        /* U2 OUT0...OUT7 = hÃ ng 3 */
        u2_word = segment_data;
    }

    /*
     * Theo schematic:
     *
     * U2 OUT10 = CONTROL_COL_1
     * U2 OUT11 = CONTROL_COL_2
     * ...
     * U2 OUT15 = CONTROL_COL_6
     *
     * Output MBI báº­t sáº½ kÃ©o base transistor PNP xuá»‘ng,
     * lÃ m transistor cáº¥p nguá»“n cho common anode.
     */
    u2_word |= (uint16_t)(1U << (10U + column));

    mbi5026_load_words(u1_word, u2_word);

    /* OE active-low: cho phÃ©p output hoáº¡t Ä‘á»™ng */
    gpio_set_level(PIN_OE, 0);

    scan_slot++;

    if (scan_slot >= (DISPLAY_ROWS * DISPLAY_COLUMNS)) {
        scan_slot = 0;
    }

#else

    /*
     * Cháº¿ Ä‘á»™ quÃ©t 6 cá»™t.
     * Ba LED cÃ¹ng cá»™t Ä‘Æ°á»£c hiá»ƒn thá»‹ Ä‘á»“ng thá»i.
     */
    static uint8_t column = 0;

    uint8_t row1_segments;
    uint8_t row2_segments;
    uint8_t row3_segments;

    portENTER_CRITICAL(&display_lock);

    row1_segments = display_buffer[0][column];
    row2_segments = display_buffer[1][column];
    row3_segments = display_buffer[2][column];

    portEXIT_CRITICAL(&display_lock);

    /*
     * U1:
     * OUT0...7  = hÃ ng 1
     * OUT8...15 = hÃ ng 2
     */
    uint16_t u1_word =
        ((uint16_t)row2_segments << 8) |
        row1_segments;

    /*
     * U2:
     * OUT0...7   = hÃ ng 3
     * OUT10...15 = cá»™t 1...6
     */
    uint16_t u2_word =
        row3_segments |
        (uint16_t)(1U << (10U + column));

    mbi5026_load_words(u1_word, u2_word);

    gpio_set_level(PIN_OE, 0);

    column++;

    if (column >= DISPLAY_COLUMNS) {
        column = 0;
    }

#endif
}

/* =========================================================
 * HÃ€M ÄIá»€U KHIá»‚N BUFFER HIá»‚N THá»Š
 * ========================================================= */

static void display_clear(void)
{
    portENTER_CRITICAL(&display_lock);

    for (int row = 0; row < DISPLAY_ROWS; row++) {
        for (int column = 0; column < DISPLAY_COLUMNS; column++) {
            display_buffer[row][column] = 0;
        }
    }

    portEXIT_CRITICAL(&display_lock);
}

void control_display_led_clear(void)
{
    display_clear();
}

void control_display_led_set_segments(
    const uint8_t segments[DISPLAY_ROWS][DISPLAY_COLUMNS])
{
    if (segments == NULL) {
        return;
    }

    portENTER_CRITICAL(&display_lock);
    memcpy(display_buffer, segments, sizeof(display_buffer));
    portEXIT_CRITICAL(&display_lock);
}

/*
 * HÃ m nÃ y dÃ¹ng khi sau nÃ y cáº§n Ä‘áº·t sá»‘ riÃªng cho tá»«ng LED.
 *
 * row:    0...2
 * column: 0...5
 * number: 0...9
 */
/* =========================================================
 * KHá»žI Táº O GPIO
 * ========================================================= */

static void display_gpio_init(void)
{
    gpio_config_t gpio_config_output = {
        .pin_bit_mask =
            (1ULL << PIN_SDI) |
            (1ULL << PIN_OE)  |
            (1ULL << PIN_CLK) |
            (1ULL << PIN_LE),

        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&gpio_config_output));

    /*
     * OE = 1 Ä‘á»ƒ táº¯t output trong lÃºc khá»Ÿi táº¡o.
     */
    gpio_set_level(PIN_OE, 1);
    gpio_set_level(PIN_CLK, 0);
    gpio_set_level(PIN_LE, 0);
    gpio_set_level(PIN_SDI, 0);

    /* XÃ³a dá»¯ liá»‡u cÃ²n láº¡i trong cáº£ hai IC */
    mbi5026_load_words(0x0000, 0x0000);

    /* Giá»¯ output táº¯t cho Ä‘áº¿n khi timer quÃ©t báº¯t Ä‘áº§u */
    gpio_set_level(PIN_OE, 1);
}

/* =========================================================
 * PUBLIC API
 * ========================================================= */

void control_display_led_init(void)
{
    ESP_LOGI(TAG, "Khoi tao MBI5026 va 18 LED 7 doan");

#if SAFE_ONE_DIGIT_SCAN
    ESP_LOGI(TAG, "Che do quet an toan: 18 slot, mot LED moi slot");
#else
    ESP_LOGI(TAG, "Che do quet nhanh: 6 cot, ba hang dong thoi");
#endif

    display_gpio_init();
    display_clear();

    const esp_timer_create_args_t scan_timer_args = {
        .callback = scan_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led7seg_scan"
    };

    ESP_ERROR_CHECK(
        esp_timer_create(&scan_timer_args, &scan_timer_handle)
    );

    ESP_ERROR_CHECK(
        esp_timer_start_periodic(scan_timer_handle, SCAN_PERIOD_US)
    );

}
