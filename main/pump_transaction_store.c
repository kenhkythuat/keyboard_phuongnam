#include "pump_transaction_store.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "device_config.h"

#define STORE_NAMESPACE          "pump_tx"
#define STORE_BLOB_KEY           "queue"
#define STORE_MAGIC              0x50545851UL
#define STORE_VERSION            2U
#define STORE_LEGACY_VERSION     1U
#define STORE_MAX_TRANSACTIONS   32U

typedef struct {
    uint32_t amount_vnd;
    uint32_t volume_ml;
    uint32_t unit_price;
} pump_stored_transaction_v1_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    pump_stored_transaction_v1_t transactions[STORE_MAX_TRANSACTIONS];
} pump_store_blob_v1_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    pump_stored_transaction_t transactions[STORE_MAX_TRANSACTIONS];
} pump_store_blob_t;

static const char *TAG = "PUMP_STORE";
static nvs_handle_t s_nvs_handle;
static pump_store_blob_t s_store;
static bool s_initialized;

static bool command_code_is_stored_valid(const char *command_code)
{
    return command_code[0] != '\0' &&
           memchr(command_code, '\0',
                  PUMP_TRANSACTION_COMMAND_CODE_MAX_LENGTH + 1U) != NULL;
}

static bool current_store_is_valid(const pump_store_blob_t *store)
{
    if (store->magic != STORE_MAGIC || store->version != STORE_VERSION ||
        store->count > STORE_MAX_TRANSACTIONS) {
        return false;
    }
    for (uint16_t index = 0U; index < store->count; index++) {
        if (!command_code_is_stored_valid(
                store->transactions[index].command_code)) {
            return false;
        }
    }
    return true;
}

static bool legacy_store_is_valid(const pump_store_blob_v1_t *store)
{
    return store->magic == STORE_MAGIC &&
           store->version == STORE_LEGACY_VERSION &&
           store->count <= STORE_MAX_TRANSACTIONS;
}

static esp_err_t commit_store(void)
{
    esp_err_t err = nvs_set_blob(
        s_nvs_handle,
        STORE_BLOB_KEY,
        &s_store,
        sizeof(s_store)
    );
    return err == ESP_OK ? nvs_commit(s_nvs_handle) : err;
}

static void reset_store(void)
{
    memset(&s_store, 0, sizeof(s_store));
    s_store.magic = STORE_MAGIC;
    s_store.version = STORE_VERSION;
}

esp_err_t pump_transaction_store_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    union {
        pump_store_blob_t current;
        pump_store_blob_v1_t legacy;
    } stored;
    memset(&stored, 0, sizeof(stored));

    size_t blob_size = sizeof(stored);
    err = nvs_get_blob(s_nvs_handle, STORE_BLOB_KEY, &stored, &blob_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        reset_store();
    } else if (err == ESP_OK && blob_size == sizeof(stored.current) &&
               current_store_is_valid(&stored.current)) {
        s_store = stored.current;
    } else if (err == ESP_OK && blob_size == sizeof(stored.legacy) &&
               legacy_store_is_valid(&stored.legacy)) {
        reset_store();
        s_store.count = stored.legacy.count;
        for (uint16_t index = 0U; index < s_store.count; index++) {
            s_store.transactions[index].amount_vnd =
                stored.legacy.transactions[index].amount_vnd;
            s_store.transactions[index].volume_ml =
                stored.legacy.transactions[index].volume_ml;
            s_store.transactions[index].unit_price =
                stored.legacy.transactions[index].unit_price;
            strlcpy(s_store.transactions[index].command_code,
                    DEFAULT_MODE_CALIBRATION,
                    sizeof(s_store.transactions[index].command_code));
        }
        err = commit_store();
        if (err != ESP_OK) {
            nvs_close(s_nvs_handle);
            return err;
        }
        ESP_LOGI(TAG, "Da migrate %u giao dich NVS cu voi command_code=%s",
                 (unsigned)s_store.count, DEFAULT_MODE_CALIBRATION);
    } else {
        ESP_LOGW(TAG, "Du lieu queue NVS khong hop le, khoi tao lai");
        reset_store();
        err = commit_store();
        if (err != ESP_OK) {
            nvs_close(s_nvs_handle);
            return err;
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Khoi phuc %u giao dich tu Flash", (unsigned)s_store.count);
    return ESP_OK;
}

esp_err_t pump_transaction_store_append(
    const pump_stored_transaction_t *transaction)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (transaction == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!command_code_is_stored_valid(transaction->command_code)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_store.count >= STORE_MAX_TRANSACTIONS) {
        return ESP_ERR_NO_MEM;
    }

    s_store.transactions[s_store.count] = *transaction;
    s_store.count++;
    esp_err_t err = commit_store();
    if (err != ESP_OK) {
        s_store.count--;
        memset(&s_store.transactions[s_store.count], 0,
               sizeof(s_store.transactions[s_store.count]));
    }
    return err;
}

esp_err_t pump_transaction_store_peek(pump_stored_transaction_t *transaction)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (transaction == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_store.count == 0U) {
        return ESP_ERR_NOT_FOUND;
    }

    *transaction = s_store.transactions[0];
    return ESP_OK;
}

esp_err_t pump_transaction_store_pop(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_store.count == 0U) {
        return ESP_ERR_NOT_FOUND;
    }

    pump_store_blob_t previous = s_store;
    s_store.count--;
    memmove(
        &s_store.transactions[0],
        &s_store.transactions[1],
        s_store.count * sizeof(s_store.transactions[0])
    );
    memset(&s_store.transactions[s_store.count], 0,
           sizeof(s_store.transactions[s_store.count]));

    esp_err_t err = commit_store();
    if (err != ESP_OK) {
        s_store = previous;
    }
    return err;
}

size_t pump_transaction_store_count(void)
{
    return s_initialized ? s_store.count : 0U;
}
