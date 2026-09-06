#include "pump_transaction_store.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define STORE_NAMESPACE          "pump_tx"
#define STORE_BLOB_KEY           "queue"
#define STORE_MAGIC              0x50545851UL
#define STORE_VERSION            1U
#define STORE_MAX_TRANSACTIONS   32U

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

    size_t blob_size = sizeof(s_store);
    err = nvs_get_blob(s_nvs_handle, STORE_BLOB_KEY, &s_store, &blob_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        reset_store();
    } else if (err != ESP_OK || blob_size != sizeof(s_store) ||
               s_store.magic != STORE_MAGIC ||
               s_store.version != STORE_VERSION ||
               s_store.count > STORE_MAX_TRANSACTIONS) {
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
