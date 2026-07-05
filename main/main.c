#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "driver/spi_slave.h"

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

/* =========================================================
 * GPIO đọc tín hiệu từ mạch bên ngoài
 * ========================================================= */
#define OE_IN_GPIO                 GPIO_NUM_4
#define LE_IN_GPIO                 GPIO_NUM_5
#define SDI_IN_GPIO                GPIO_NUM_6
#define CLK_IN_GPIO                GPIO_NUM_15

#define MBI_SPI_HOST               SPI2_HOST

/*
 * Mode 0 là mode trước đó đã đọc gần đúng dữ liệu thực tế.
 *
 * CPOL = 0
 * CPHA = 0
 */
#define SPI_CAPTURE_MODE           0

/*
 * SPI Slave không DMA:
 * 64 byte = 512 bit dung lượng tối đa.
 *
 * Giao dịch thực tế đang đọc được:
 * 192 bit = 6 khối × 32 bit.
 */
#define MAX_CAPTURE_BYTES          64U
#define MAX_CAPTURE_BITS           (MAX_CAPTURE_BYTES * 8U)

#define EXPECTED_CAPTURE_BITS      192U
#define FRAME_BITS                 32U
#define FRAME_COUNT                6U

#define DISPLAY_ROWS               3U
#define DISPLAY_COLUMNS            6U

#define RX_SLOT_COUNT              16U

/*
 * Một dữ liệu cột phải xuất hiện lặp lại nhiều lần
 * mới được xác nhận.
 */
#define COLUMN_CONFIRM_COUNT       4U

/*
 * OUT8 và OUT9 của U2 không sử dụng trong schematic,
 * vì vậy hai bit này phải bằng 0.
 */
#define U2_UNUSED_BITS_MASK            0x0300U

/*
 * Khi năm cột 2...6 đã ổn định nhưng cột 1 vẫn không bắt được,
 * sau 1,5 giây xác nhận cột 1 là blank.
 *
 * Đây đúng với màn hình thực tế hiện tại của bạn.
 */
#define ENABLE_BLANK_COLUMN1_FALLBACK  1
#define BLANK_COLUMN1_TIMEOUT_US       1500000LL

static int64_t s_column1_missing_since_us = 0;

/*
 * Mức tin cậy tối đa. Giá trị càng lớn càng chống nhiễu,
 * nhưng màn hình thay đổi sẽ cập nhật chậm hơn.
 */
#define COLUMN_CONFIDENCE_MAX      12U

#define CAPTURE_YIELD_INTERVAL     16U
#define DECODE_DELAY_MS            5U
#define REPORT_PERIOD_MS           1000U

static const char *TAG = "MBI_SNIFFER";

/* =========================================================
 * Buffer giao dịch SPI
 * ========================================================= */
typedef struct {
    spi_slave_transaction_t transaction;

    uint8_t rx_data[MAX_CAPTURE_BYTES]
        __attribute__((aligned(4)));

    volatile uint8_t oe_at_end;
} mbi_rx_slot_t;

static mbi_rx_slot_t s_rx_slots[RX_SLOT_COUNT];

/* =========================================================
 * Mẫu dữ liệu chuyển từ capture task sang decode task
 * ========================================================= */
typedef struct {
    uint8_t raw[MAX_CAPTURE_BYTES];
    uint32_t received_bits;
    uint8_t oe_level;
} mbi_capture_sample_t;

static QueueHandle_t s_decode_queue;

/* =========================================================
 * Kết quả phân tích tốt nhất trong một mẫu 192 bit
 *
 * U2 được giả định truyền trước, U1 truyền sau.
 *
 * U1:
 * OUT0...7   = hàng LED 1
 * OUT8...15  = hàng LED 2
 *
 * U2:
 * OUT0...7   = hàng LED 3
 * OUT10...15 = CONTROL_COL_1...6
 * ========================================================= */
typedef struct {
    int score;

    uint8_t bit_offset;
    uint8_t valid_block_count;
    uint8_t seen_columns;

    uint16_t u1_by_column[DISPLAY_COLUMNS];
    uint16_t u2_by_column[DISPLAY_COLUMNS];

    uint8_t display[DISPLAY_ROWS][DISPLAY_COLUMNS];
} mbi_decode_result_t;

/* =========================================================
 * Dữ liệu dùng chung cho task report
 * ========================================================= */
static portMUX_TYPE s_shared_lock = portMUX_INITIALIZER_UNLOCKED;

static volatile uint32_t s_spi_rx_count = 0;

static uint32_t s_decoded_sample_count = 0;
static uint32_t s_capture_192_count = 0;
static uint32_t s_capture_other_count = 0;

static uint32_t s_valid_block_total = 0;
static uint32_t s_display_complete_count = 0;
static uint32_t s_assembly_timeout_count = 0;

static uint32_t s_latest_received_bits = 0;
static uint8_t s_latest_oe_level = 0;
static uint8_t s_latest_assembly_mask = 0;

static uint32_t s_display_generation = 0;

static mbi_decode_result_t s_latest_candidate;
static mbi_decode_result_t s_latest_display;

/* =========================================================
 * Bộ đệm ghép màn hình
 *
 * Chỉ được sử dụng bởi decode task.
 * Không cần khóa.
 * ========================================================= */
static uint16_t s_assembly_u1[DISPLAY_COLUMNS];
static uint16_t s_assembly_u2[DISPLAY_COLUMNS];

static uint8_t s_assembly_confidence[DISPLAY_COLUMNS];
static uint8_t s_assembly_confirmed_mask = 0;

static int64_t s_assembly_start_us = 0;

/* =========================================================
 * Kiểm tra column mask
 *
 * Các giá trị hợp lệ:
 *
 * cột 1 = 0x01
 * cột 2 = 0x02
 * cột 3 = 0x04
 * cột 4 = 0x08
 * cột 5 = 0x10
 * cột 6 = 0x20
 * ========================================================= */
static inline bool is_one_hot_column(uint8_t mask)
{
    mask &= 0x3FU;

    return mask != 0U &&
           (mask & (mask - 1U)) == 0U;
}

/* =========================================================
 * Đọc một bit trong buffer theo dạng vòng tròn
 *
 * Dùng dạng vòng tròn vì transaction 192 bit có thể bắt đầu
 * ở giữa một frame 32 bit.
 * ========================================================= */
static inline uint8_t get_circular_bit(
    const uint8_t *data,
    uint32_t total_bits,
    uint32_t bit_position)
{
    bit_position %= total_bits;

    uint32_t byte_index = bit_position >> 3U;
    uint32_t bit_in_byte = 7U - (bit_position & 0x07U);

    return (uint8_t)(
        (data[byte_index] >> bit_in_byte) & 0x01U
    );
}

/* =========================================================
 * Đọc 16 bit liên tiếp, MSB trước
 * ========================================================= */
static uint16_t read_circular_u16(
    const uint8_t *data,
    uint32_t total_bits,
    uint32_t start_bit)
{
    uint16_t value = 0;

    for (uint32_t index = 0; index < 16U; index++) {
        value <<= 1U;

        value |= get_circular_bit(
            data,
            total_bits,
            start_bit + index
        );
    }

    return value;
}

/* =========================================================
 * Giải mã mã LED 7 đoạn
 *
 * bit 0 = A
 * bit 1 = B
 * bit 2 = C
 * bit 3 = D
 * bit 4 = E
 * bit 5 = F
 * bit 6 = G
 * bit 7 = DP
 * ========================================================= */
static char segment_to_character(uint8_t segments)
{
    switch (segments & 0x7FU) {
        case 0x00: return ' ';

        case 0x3F: return '0';
        case 0x06: return '1';
        case 0x5B: return '2';
        case 0x4F: return '3';
        case 0x66: return '4';
        case 0x6D: return '5';
        case 0x7D: return '6';
        case 0x07: return '7';
        case 0x7F: return '8';
        case 0x6F: return '9';

        case 0x77: return 'A';
        case 0x7C: return 'b';
        case 0x39: return 'C';
        case 0x5E: return 'd';
        case 0x79: return 'E';
        case 0x71: return 'F';
        case 0x76: return 'H';
        case 0x38: return 'L';
        case 0x73: return 'P';
        case 0x3E: return 'U';
        case 0x40: return '-';

        default: return '?';
    }
}

static inline bool is_known_segment(uint8_t segments)
{
    return segment_to_character(segments) != '?';
}

/* =========================================================
 * Đánh giá một vị trí lệch bit
 *
 * Mỗi vị trí offset tạo ra sáu frame, mỗi frame 32 bit:
 *
 * 16 bit đầu = U2
 * 16 bit sau = U1
 * ========================================================= */
static void evaluate_bit_offset(
    const uint8_t *raw,
    uint8_t bit_offset,
    mbi_decode_result_t *result)
{
    /*
     * Lưu chất lượng block tốt nhất riêng cho từng cột.
     * Không còn giữ block đầu tiên rồi bỏ block tốt hơn phía sau.
     */
    int best_quality[DISPLAY_COLUMNS];

    for (uint8_t column = 0;
         column < DISPLAY_COLUMNS;
         column++) {

        best_quality[column] = INT_MIN;
    }

    memset(result, 0, sizeof(*result));

    result->bit_offset = bit_offset;
    result->score = 0;

    for (uint8_t frame = 0;
         frame < FRAME_COUNT;
         frame++) {

        uint32_t frame_start =
            (uint32_t)bit_offset +
            ((uint32_t)frame * FRAME_BITS);

        /*
         * Theo chuỗi MBI5026:
         * 16 bit đầu = U2
         * 16 bit sau = U1
         */
        uint16_t u2_word = read_circular_u16(
            raw,
            EXPECTED_CAPTURE_BITS,
            frame_start
        );

        uint16_t u1_word = read_circular_u16(
            raw,
            EXPECTED_CAPTURE_BITS,
            frame_start + 16U
        );

        /*
         * Theo schematic, OUT8 và OUT9 của U2 không dùng.
         * Nếu hai bit này khác 0 thì nhiều khả năng đây chỉ
         * là một vị trí lệch bit giả.
         */
        if ((u2_word & U2_UNUSED_BITS_MASK) != 0U) {
            continue;
        }

        uint8_t column_mask =
            (uint8_t)((u2_word >> 10U) & 0x3FU);

        if (!is_one_hot_column(column_mask)) {
            continue;
        }

        uint8_t column =
            (uint8_t)__builtin_ctz(
                (unsigned int)column_mask
            );

        if (column >= DISPLAY_COLUMNS) {
            continue;
        }

        uint8_t row1 =
            (uint8_t)(u1_word & 0x00FFU);

        uint8_t row2 =
            (uint8_t)((u1_word >> 8U) & 0x00FFU);

        uint8_t row3 =
            (uint8_t)(u2_word & 0x00FFU);

        /*
         * Chỉ công nhận khi cả ba byte segment đều là
         * mã LED 7 đoạn đã biết.
         *
         * 0x00 cũng được xem là blank hợp lệ.
         */
        if (!is_known_segment(row1) ||
            !is_known_segment(row2) ||
            !is_known_segment(row3)) {

            continue;
        }

        int quality = 100;

        /*
         * Ưu tiên dữ liệu trùng với ứng viên đang được tích lũy.
         */
        if (s_assembly_confidence[column] != 0U &&
            s_assembly_u1[column] == u1_word &&
            s_assembly_u2[column] == u2_word) {

            quality += 80;
        }

        /*
         * Cột blank hoàn toàn là trường hợp hợp lệ nhưng
         * dễ bị mất trong thuật toán chấm điểm cũ.
         *
         * U1 = 0x0000
         * U2 phần segment = 0x00
         */
        if (u1_word == 0x0000U &&
            (u2_word & 0x00FFU) == 0x00U) {

            quality += 40;
        }

        /*
         * Dữ liệu bắt đầu đúng byte thường đáng tin cậy hơn.
         */
        if ((bit_offset & 0x07U) == 0U) {
            quality += 5;
        }

        /*
         * Nếu cùng một cột xuất hiện nhiều lần trong mẫu,
         * giữ block có chất lượng cao nhất.
         */
        if (quality <= best_quality[column]) {
            continue;
        }

        best_quality[column] = quality;

        result->u1_by_column[column] = u1_word;
        result->u2_by_column[column] = u2_word;

        result->display[0][column] = row1;
        result->display[1][column] = row2;
        result->display[2][column] = row3;
    }

    /*
     * Tổng hợp các cột hợp lệ đã chọn.
     */
    for (uint8_t column = 0;
         column < DISPLAY_COLUMNS;
         column++) {

        if (best_quality[column] == INT_MIN) {
            continue;
        }

        uint8_t column_bit =
            (uint8_t)(1U << column);

        result->seen_columns |= column_bit;
        result->valid_block_count++;
        result->score += best_quality[column];
    }

    /*
     * Ưu tiên offset tìm được nhiều cột hơn.
     */
    result->score +=
        (int)result->valid_block_count * 200;
}

/* =========================================================
 * Tìm offset tốt nhất từ 0...31
 * ========================================================= */
static void decode_best_candidate(
    const uint8_t *raw,
    mbi_decode_result_t *best_result)
{
    mbi_decode_result_t candidate;

    memset(best_result, 0, sizeof(*best_result));
    best_result->score = INT_MIN;

    for (uint8_t offset = 0;
         offset < FRAME_BITS;
         offset++) {

        evaluate_bit_offset(
            raw,
            offset,
            &candidate
        );

        /*
         * Tiêu chí ưu tiên:
         *
         * 1. Offset tìm được nhiều cột hợp lệ hơn.
         * 2. Nếu cùng số cột, chọn điểm cao hơn.
         */
        bool better =
            candidate.valid_block_count >
                best_result->valid_block_count;

        if (candidate.valid_block_count ==
                best_result->valid_block_count &&
            candidate.score > best_result->score) {

            better = true;
        }

        if (better) {
            *best_result = candidate;
        }
    }
}

/* =========================================================
 * Xóa dữ liệu ghép màn hình
 * ========================================================= */
static void reset_display_assembly(void)
{
    memset(
        s_assembly_u1,
        0,
        sizeof(s_assembly_u1)
    );

    memset(
        s_assembly_u2,
        0,
        sizeof(s_assembly_u2)
    );

    memset(
        s_assembly_confidence,
        0,
        sizeof(s_assembly_confidence)
    );

    s_assembly_confirmed_mask = 0;
    s_assembly_start_us = 0;
}

/* =========================================================
 * Tạo kết quả màn hình từ bộ đệm assembly
 * ========================================================= */
static void build_complete_display(
    const mbi_decode_result_t *latest_candidate,
    mbi_decode_result_t *complete_result)
{
    memset(
        complete_result,
        0,
        sizeof(*complete_result)
    );

    complete_result->score =
        latest_candidate->score;

    complete_result->bit_offset =
        latest_candidate->bit_offset;

    complete_result->valid_block_count =
        DISPLAY_COLUMNS;

    complete_result->seen_columns =
        0x3FU;

    for (uint8_t column = 0;
         column < DISPLAY_COLUMNS;
         column++) {

        uint16_t u1_word =
            s_assembly_u1[column];

        uint16_t u2_word =
            s_assembly_u2[column];

        complete_result->u1_by_column[column] =
            u1_word;

        complete_result->u2_by_column[column] =
            u2_word;

        complete_result->display[0][column] =
            (uint8_t)(u1_word & 0x00FFU);

        complete_result->display[1][column] =
            (uint8_t)((u1_word >> 8U) & 0x00FFU);

        complete_result->display[2][column] =
            (uint8_t)(u2_word & 0x00FFU);
    }
}

/* =========================================================
 * Đưa các block hợp lệ vào bộ ghép màn hình
 *
 * Một cột chỉ được xác nhận khi cùng một U1/U2 xuất hiện
 * COLUMN_CONFIRM_COUNT lần liên tiếp.
 * ========================================================= */
static bool feed_candidate_to_assembly(
    const mbi_decode_result_t *candidate,
    int64_t now_us,
    mbi_decode_result_t *complete_result)
{
    (void)now_us;

    if (candidate == NULL ||
        complete_result == NULL ||
        candidate->seen_columns == 0U) {

        return false;
    }

    for (uint8_t column = 0;
         column < DISPLAY_COLUMNS;
         column++) {

        uint8_t column_bit =
            (uint8_t)(1U << column);

        if ((candidate->seen_columns &
             column_bit) == 0U) {

            continue;
        }

        uint16_t new_u1 =
            candidate->u1_by_column[column];

        uint16_t new_u2 =
            candidate->u2_by_column[column];

        uint8_t confidence =
            s_assembly_confidence[column];

        /*
         * Cột chưa có ứng viên.
         */
        if (confidence == 0U) {
            s_assembly_u1[column] = new_u1;
            s_assembly_u2[column] = new_u2;
            s_assembly_confidence[column] = 1U;

            s_assembly_confirmed_mask &=
                (uint8_t)~column_bit;

            continue;
        }

        bool same_data =
            s_assembly_u1[column] == new_u1 &&
            s_assembly_u2[column] == new_u2;

        if (same_data) {
            if (s_assembly_confidence[column] <
                COLUMN_CONFIDENCE_MAX) {

                s_assembly_confidence[column]++;
            }
        } else {
            /*
             * Dữ liệu khác chỉ làm giảm tin cậy của cột đó.
             * Không xóa năm cột còn lại.
             */
            if (s_assembly_confidence[column] > 0U) {
                s_assembly_confidence[column]--;
            }

            /*
             * Chỉ thay ứng viên sau khi ứng viên cũ đã mất
             * toàn bộ độ tin cậy.
             */
            if (s_assembly_confidence[column] == 0U) {
                s_assembly_u1[column] = new_u1;
                s_assembly_u2[column] = new_u2;
                s_assembly_confidence[column] = 1U;

                s_assembly_confirmed_mask &=
                    (uint8_t)~column_bit;
            }
        }

        /*
         * Cột blank chỉ cần hai lần giống nhau.
         * Các cột khác dùng COLUMN_CONFIRM_COUNT.
         */
        uint8_t required_confidence =
            COLUMN_CONFIRM_COUNT;

        if (s_assembly_u1[column] == 0x0000U &&
            (s_assembly_u2[column] & 0x00FFU) == 0x00U) {

            required_confidence = 2U;
        }

        if (s_assembly_confidence[column] >=
            required_confidence) {

            s_assembly_confirmed_mask |= column_bit;
        }

        /*
         * Không hủy confirmed chỉ vì một mẫu nhiễu làm
         * confidence giảm nhẹ. Chỉ hủy khi thay ứng viên.
         */
    }

    if (s_assembly_confirmed_mask != 0x3FU) {
        return false;
    }

    build_complete_display(
        candidate,
        complete_result
    );

    return true;
}

/* =========================================================
 * Xử lý một mẫu SPI
 * ========================================================= */
static void process_capture_sample(
    const mbi_capture_sample_t *sample)
{
    if (sample == NULL) {
        return;
    }

    int64_t now_us = esp_timer_get_time();

    mbi_decode_result_t candidate;
    mbi_decode_result_t complete_display;

    memset(&candidate, 0, sizeof(candidate));
    candidate.score = INT_MIN;

    memset(
        &complete_display,
        0,
        sizeof(complete_display)
    );

    bool display_completed = false;

    if (sample->received_bits ==
        EXPECTED_CAPTURE_BITS) {

        decode_best_candidate(
            sample->raw,
            &candidate
        );

        if (candidate.valid_block_count > 0U &&
            candidate.seen_columns != 0U) {

            display_completed =
                feed_candidate_to_assembly(
                    &candidate,
                    now_us,
                    &complete_display
                );
        }
    }

#if ENABLE_BLANK_COLUMN1_FALLBACK

    /*
     * 0x3E = cột 2,3,4,5,6 đã xác nhận.
     * Chỉ còn thiếu cột 1.
     */
    if (!display_completed &&
        s_assembly_confirmed_mask == 0x3EU) {

        if (s_column1_missing_since_us == 0) {
            s_column1_missing_since_us = now_us;
        }

        if ((now_us - s_column1_missing_since_us) >=
            BLANK_COLUMN1_TIMEOUT_US) {

            /*
             * Theo màn hình thực tế hiện tại:
             *
             * cột 1:
             * row 1 = blank
             * row 2 = blank
             * row 3 = blank
             *
             * U2 bit10 = CONTROL_COL_1.
             */
            s_assembly_u1[0] = 0x0000U;
            s_assembly_u2[0] = 0x0400U;

            s_assembly_confidence[0] =
                COLUMN_CONFIRM_COUNT;

            s_assembly_confirmed_mask |= 0x01U;

            build_complete_display(
                &candidate,
                &complete_display
            );

            display_completed = true;

            ESP_LOGW(
                TAG,
                "COL1 khong bat duoc on dinh, "
                "tam suy ra COL1=blank: U2=0400 U1=0000"
            );
        }
    } else {
        /*
         * Reset bộ đếm nếu:
         * - đã bắt được cột 1 thật;
         * - hoặc chưa đủ cột 2...6.
         */
        if ((s_assembly_confirmed_mask & 0x01U) != 0U ||
            s_assembly_confirmed_mask != 0x3EU) {

            s_column1_missing_since_us = 0;
        }
    }

#endif

    portENTER_CRITICAL(&s_shared_lock);

    s_decoded_sample_count++;

    s_latest_received_bits =
        sample->received_bits;

    s_latest_oe_level =
        sample->oe_level;

    if (sample->received_bits ==
        EXPECTED_CAPTURE_BITS) {

        s_capture_192_count++;

        s_valid_block_total +=
            candidate.valid_block_count;

        s_latest_candidate =
            candidate;
    } else {
        s_capture_other_count++;
    }

    s_latest_assembly_mask =
        s_assembly_confirmed_mask;

    if (display_completed) {
        bool first_display =
            s_display_generation == 0U;

        bool display_changed =
            first_display ||
            memcmp(
                s_latest_display.display,
                complete_display.display,
                sizeof(complete_display.display)
            ) != 0;

        if (display_changed) {
            s_latest_display =
                complete_display;

            s_display_complete_count++;
            s_display_generation++;
        }
    }

    portEXIT_CRITICAL(&s_shared_lock);
}

/* =========================================================
 * Callback khi SPI transaction kết thúc
 * ========================================================= */
static void IRAM_ATTR spi_post_transaction_callback(
    spi_slave_transaction_t *transaction)
{
    mbi_rx_slot_t *slot =
        (mbi_rx_slot_t *)transaction->user;

    slot->oe_at_end =
        gpio_get_level(OE_IN_GPIO) ? 1U : 0U;
}

/* =========================================================
 * Queue buffer SPI
 * ========================================================= */
static esp_err_t queue_rx_slot(mbi_rx_slot_t *slot)
{
    memset(
        &slot->transaction,
        0,
        sizeof(slot->transaction)
    );

    memset(
        slot->rx_data,
        0,
        sizeof(slot->rx_data)
    );

    slot->oe_at_end = 0;

    slot->transaction.length =
        MAX_CAPTURE_BITS;

    slot->transaction.rx_buffer =
        slot->rx_data;

    slot->transaction.tx_buffer =
        NULL;

    slot->transaction.user =
        slot;

    return spi_slave_queue_trans(
        MBI_SPI_HOST,
        &slot->transaction,
        portMAX_DELAY
    );
}

/* =========================================================
 * Task thu SPI
 *
 * Chỉ nhận dữ liệu và chuyển mẫu mới nhất sang decode task.
 * Không giải mã hoặc in log trong task này.
 * ========================================================= */
static void mbi_capture_task(void *argument)
{
    (void)argument;

    gpio_config_t oe_config = {
        .pin_bit_mask =
            1ULL << OE_IN_GPIO,

        .mode =
            GPIO_MODE_INPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(
        gpio_config(&oe_config)
    );

    spi_bus_config_t bus_config = {
        .mosi_io_num =
            SDI_IN_GPIO,

        .miso_io_num =
            -1,

        .sclk_io_num =
            CLK_IN_GPIO,

        .quadwp_io_num =
            -1,

        .quadhd_io_num =
            -1,

        .max_transfer_sz =
            MAX_CAPTURE_BYTES
    };

    spi_slave_interface_config_t slave_config = {
        .mode =
            SPI_CAPTURE_MODE,

        /*
         * LE đang tạo transaction 192 bit ổn định,
         * nên tiếp tục dùng làm CS active-low.
         */
        .spics_io_num =
            LE_IN_GPIO,

        .queue_size =
            RX_SLOT_COUNT,

        .flags =
            0,

        .post_setup_cb =
            NULL,

        .post_trans_cb =
            spi_post_transaction_callback
    };

    ESP_ERROR_CHECK(
        spi_slave_initialize(
            MBI_SPI_HOST,
            &bus_config,
            &slave_config,
            SPI_DMA_DISABLED
        )
    );

    for (uint32_t index = 0;
         index < RX_SLOT_COUNT;
         index++) {

        ESP_ERROR_CHECK(
            queue_rx_slot(&s_rx_slots[index])
        );
    }

    ESP_LOGI(
        TAG,
        "Capture started: OE=%d LE/CS=%d SDI=%d CLK=%d mode=%d",
        OE_IN_GPIO,
        LE_IN_GPIO,
        SDI_IN_GPIO,
        CLK_IN_GPIO,
        SPI_CAPTURE_MODE
    );

    uint32_t yield_counter = 0;

    while (1) {
        spi_slave_transaction_t *completed =
            NULL;

        esp_err_t result =
            spi_slave_get_trans_result(
                MBI_SPI_HOST,
                &completed,
                portMAX_DELAY
            );

        if (result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "spi_slave_get_trans_result: %s",
                esp_err_to_name(result)
            );

            vTaskDelay(1);
            continue;
        }

        mbi_rx_slot_t *completed_slot =
            (mbi_rx_slot_t *)completed->user;

        mbi_capture_sample_t sample;

        sample.received_bits =
            (uint32_t)completed->trans_len;

        sample.oe_level =
            completed_slot->oe_at_end;

        memcpy(
            sample.raw,
            completed_slot->rx_data,
            sizeof(sample.raw)
        );

        /*
         * Trả buffer về SPI trước.
         */
        ESP_ERROR_CHECK(
            queue_rx_slot(completed_slot)
        );

        __atomic_fetch_add(
            &s_spi_rx_count,
            1U,
            __ATOMIC_RELAXED
        );

        /*
         * Queue dài một phần tử:
         * luôn giữ mẫu mới nhất.
         */
        xQueueOverwrite(
            s_decode_queue,
            &sample
        );

        yield_counter++;

        /*
         * Nhường CPU0 cho Idle Task và các task hệ thống.
         */
        if (yield_counter >=
            CAPTURE_YIELD_INTERVAL) {

            yield_counter = 0;
            vTaskDelay(1);
        }
    }
}

/* =========================================================
 * Task giải mã
 * ========================================================= */
static void mbi_decode_task(void *argument)
{
    (void)argument;

    mbi_capture_sample_t sample;

    TickType_t decode_delay =
        pdMS_TO_TICKS(DECODE_DELAY_MS);

    if (decode_delay == 0) {
        decode_delay = 1;
    }

    while (1) {
        if (xQueueReceive(
                s_decode_queue,
                &sample,
                portMAX_DELAY) == pdTRUE) {

            process_capture_sample(&sample);

            /*
             * Không cần giải mã hàng nghìn mẫu mỗi giây.
             * Luôn lấy mẫu mới nhất từ queue.
             */
            vTaskDelay(decode_delay);
        }
    }
}

/* =========================================================
 * Format một hàng LED
 * ========================================================= */
static void format_display_row(
    const uint8_t display
        [DISPLAY_ROWS][DISPLAY_COLUMNS],
    uint8_t row,
    char *output,
    size_t output_size)
{
    if (output == NULL ||
        output_size == 0U ||
        row >= DISPLAY_ROWS) {

        return;
    }

    size_t used = 0;
    output[0] = '\0';

    for (uint8_t column = 0;
         column < DISPLAY_COLUMNS;
         column++) {

        uint8_t segments =
            display[row][column];

        char character =
            segment_to_character(segments);

        char decimal_point =
            (segments & 0x80U) ? '.' : ' ';

        int written = snprintf(
            output + used,
            output_size - used,
            "[%c%c]%s",
            character,
            decimal_point,
            column == DISPLAY_COLUMNS - 1U ?
                "" : " "
        );

        if (written < 0) {
            return;
        }

        if ((size_t)written >=
            output_size - used) {

            output[output_size - 1U] =
                '\0';

            return;
        }

        used += (size_t)written;
    }
}

/* =========================================================
 * In chi tiết sáu cột
 * ========================================================= */
static void print_display_columns(
    const mbi_decode_result_t *display)
{
    ESP_LOGI(
        TAG,
        "DISPLAY COMPLETE: offset gan nhat=%u score=%d",
        display->bit_offset,
        display->score
    );

    for (uint8_t column = 0;
         column < DISPLAY_COLUMNS;
         column++) {

        uint16_t u1_word =
            display->u1_by_column[column];

        uint16_t u2_word =
            display->u2_by_column[column];

        uint8_t row1 =
            display->display[0][column];

        uint8_t row2 =
            display->display[1][column];

        uint8_t row3 =
            display->display[2][column];

        uint8_t column_mask =
            (uint8_t)(
                (u2_word >> 10U) & 0x3FU
            );

        ESP_LOGI(
            TAG,
            "COL%u MASK=%02X U2=%04X U1=%04X "
            "R1=%02X(%c%s) R2=%02X(%c%s) R3=%02X(%c%s)",

            (unsigned int)(column + 1U),

            column_mask,
            u2_word,
            u1_word,

            row1,
            segment_to_character(row1),
            (row1 & 0x80U) ? "." : "",

            row2,
            segment_to_character(row2),
            (row2 & 0x80U) ? "." : "",

            row3,
            segment_to_character(row3),
            (row3 & 0x80U) ? "." : ""
        );
    }
}

/* =========================================================
 * Hàm lấy dữ liệu để sau này mô phỏng lên LED thật
 * ========================================================= */
bool mbi_sniffer_get_display(
    uint8_t output
        [DISPLAY_ROWS][DISPLAY_COLUMNS])
{
    if (output == NULL) {
        return false;
    }

    bool available;

    portENTER_CRITICAL(&s_shared_lock);

    available =
        s_display_generation != 0U;

    if (available) {
        memcpy(
            output,
            s_latest_display.display,
            sizeof(s_latest_display.display)
        );
    }

    portEXIT_CRITICAL(&s_shared_lock);

    return available;
}

/* =========================================================
 * Task in log
 * ========================================================= */
static void mbi_report_task(void *argument)
{
    (void)argument;

    uint32_t last_generation = 0;

    uint8_t last_display
        [DISPLAY_ROWS][DISPLAY_COLUMNS] = {0};

    bool have_last_display = false;

    while (1) {
        vTaskDelay(
            pdMS_TO_TICKS(REPORT_PERIOD_MS)
        );

        uint32_t spi_rx_count =
            __atomic_load_n(
                &s_spi_rx_count,
                __ATOMIC_RELAXED
            );

        uint32_t decoded_samples;
        uint32_t capture_192;
        uint32_t capture_other;
        uint32_t valid_blocks;
        uint32_t completed_displays;
        uint32_t assembly_timeouts;

        uint32_t latest_bits;
        uint8_t latest_oe;
        uint8_t assembly_mask;

        uint32_t generation;

        mbi_decode_result_t candidate;
        mbi_decode_result_t display;

        portENTER_CRITICAL(&s_shared_lock);

        decoded_samples =
            s_decoded_sample_count;

        capture_192 =
            s_capture_192_count;

        capture_other =
            s_capture_other_count;

        valid_blocks =
            s_valid_block_total;

        completed_displays =
            s_display_complete_count;

        assembly_timeouts =
            s_assembly_timeout_count;

        latest_bits =
            s_latest_received_bits;

        latest_oe =
            s_latest_oe_level;

        assembly_mask =
            s_latest_assembly_mask;

        generation =
            s_display_generation;

        candidate =
            s_latest_candidate;

        display =
            s_latest_display;

        portEXIT_CRITICAL(&s_shared_lock);

        ESP_LOGI(
            TAG,
            "STAT spi_rx=%" PRIu32
            " decoded=%" PRIu32
            " bits192=%" PRIu32
            " other=%" PRIu32
            " valid_blocks=%" PRIu32
            " display_ok=%" PRIu32
            " timeout=%" PRIu32
            " latest_bits=%" PRIu32
            " OE=%u"
            " assembling=0x%02X",

            spi_rx_count,
            decoded_samples,
            capture_192,
            capture_other,
            valid_blocks,
            completed_displays,
            assembly_timeouts,
            latest_bits,
            latest_oe,
            assembly_mask
        );

        if (generation == 0U) {
            ESP_LOGW(
                TAG,
                "Chua ghep du 6 cot. Best sample: "
                "offset=%u valid_blocks=%u "
                "seen=0x%02X score=%d",

                candidate.bit_offset,
                candidate.valid_block_count,
                candidate.seen_columns,
                candidate.score
            );

            continue;
        }

        if (generation == last_generation) {
            continue;
        }

        last_generation = generation;

        bool display_changed =
            !have_last_display ||
            memcmp(
                last_display,
                display.display,
                sizeof(last_display)
            ) != 0;

        if (!display_changed) {
            continue;
        }

        memcpy(
            last_display,
            display.display,
            sizeof(last_display)
        );

        have_last_display = true;

        print_display_columns(&display);

        char row1_string[64];
        char row2_string[64];
        char row3_string[64];

        format_display_row(
            display.display,
            0,
            row1_string,
            sizeof(row1_string)
        );

        format_display_row(
            display.display,
            1,
            row2_string,
            sizeof(row2_string)
        );

        format_display_row(
            display.display,
            2,
            row3_string,
            sizeof(row3_string)
        );

        ESP_LOGI(
            TAG,
            "========================================"
        );

        ESP_LOGI(
            TAG,
            "LED 01-06: %s",
            row1_string
        );

        ESP_LOGI(
            TAG,
            "LED 07-12: %s",
            row2_string
        );

        ESP_LOGI(
            TAG,
            "LED 13-18: %s",
            row3_string
        );

        ESP_LOGI(
            TAG,
            "========================================"
        );
    }
}

/* =========================================================
 * APP MAIN
 * ========================================================= */
void app_main(void)
{
    reset_display_assembly();

    s_decode_queue = xQueueCreate(
        1,
        sizeof(mbi_capture_sample_t)
    );

    if (s_decode_queue == NULL) {
        ESP_LOGE(TAG, "Khong tao duoc decode queue");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    BaseType_t capture_result =
        xTaskCreatePinnedToCore(
            mbi_capture_task,
            "mbi_capture_task",
            4096,
            NULL,
            8,
            NULL,
            0
        );

    if (capture_result != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

#if CONFIG_FREERTOS_UNICORE
    const BaseType_t worker_core = 0;
#else
    const BaseType_t worker_core = 1;
#endif

    BaseType_t decode_result =
        xTaskCreatePinnedToCore(
            mbi_decode_task,
            "mbi_decode_task",
            6144,
            NULL,
            5,
            NULL,
            worker_core
        );

    if (decode_result != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    BaseType_t report_result =
        xTaskCreatePinnedToCore(
            mbi_report_task,
            "mbi_report_task",
            6144,
            NULL,
            2,
            NULL,
            worker_core
        );

    if (report_result != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
}