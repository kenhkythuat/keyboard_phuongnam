#include "time_manager.h"

#include <inttypes.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"

#include "wifi_manager.h"

#define TIME_VALID_BIT             BIT0
#define SNTP_SYNC_INTERVAL_MS      (60U * 60U * 1000U)
#define SNTP_SERVER_NAME           "pool.ntp.org"
#define DEVICE_TIMEZONE            "ICT-7"
#define DEVICE_TIME_FORMAT         "%d/%m/%Y %H:%M:%S"

static const char *TAG = "TIME_MANAGER";

static EventGroupHandle_t s_time_event_group;
static bool s_started;
static bool s_sync_requested_for_connection;

static void time_sync_notification_callback(struct timeval *time_value)
{
    if (time_value == NULL || s_time_event_group == NULL) {
        return;
    }

    xEventGroupSetBits(s_time_event_group, TIME_VALID_BIT);
    ESP_LOGI(
        TAG,
        "SNTP sync thanh cong, ts=%" PRId64,
        (int64_t)time_value->tv_sec
    );
}

static void start_or_restart_sntp(void)
{
    if (__atomic_exchange_n(
            &s_sync_requested_for_connection,
            true,
            __ATOMIC_ACQ_REL)) {
        return;
    }

    esp_err_t err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        __atomic_store_n(
            &s_sync_requested_for_connection,
            false,
            __ATOMIC_RELEASE
        );
        ESP_LOGW(TAG, "Khong the start/re-sync SNTP: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Da yeu cau SNTP sync/re-sync");
}

static void network_event_handler(void *argument,
                                  esp_event_base_t event_base,
                                  int32_t event_id,
                                  void *event_data)
{
    (void)argument;
    (void)event_data;

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        start_or_restart_sntp();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        __atomic_store_n(
            &s_sync_requested_for_connection,
            false,
            __ATOMIC_RELEASE
        );
    }
}

static esp_err_t format_device_time(time_t timestamp, char *buf, size_t len)
{
    if (buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len < TIME_MANAGER_DEVICE_TIME_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }

    struct tm local_time;
    if (localtime_r(&timestamp, &local_time) == NULL) {
        return ESP_FAIL;
    }

    if (strftime(buf, len, DEVICE_TIME_FORMAT, &local_time) == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t time_manager_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_time_event_group = xEventGroupCreate();
    if (s_time_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (setenv("TZ", DEVICE_TIMEZONE, 1) != 0) {
        vEventGroupDelete(s_time_event_group);
        s_time_event_group = NULL;
        return ESP_FAIL;
    }
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER_NAME);
    config.start = false;
    config.wait_for_sync = false;
    config.sync_cb = time_sync_notification_callback;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        vEventGroupDelete(s_time_event_group);
        s_time_event_group = NULL;
        return err;
    }

    esp_sntp_set_sync_interval(SNTP_SYNC_INTERVAL_MS);

    err = esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        network_event_handler,
        NULL
    );
    if (err != ESP_OK) {
        esp_netif_sntp_deinit();
        vEventGroupDelete(s_time_event_group);
        s_time_event_group = NULL;
        return err;
    }

    err = esp_event_handler_register(
        WIFI_EVENT,
        WIFI_EVENT_STA_DISCONNECTED,
        network_event_handler,
        NULL
    );
    if (err != ESP_OK) {
        esp_event_handler_unregister(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            network_event_handler
        );
        esp_netif_sntp_deinit();
        vEventGroupDelete(s_time_event_group);
        s_time_event_group = NULL;
        return err;
    }

    s_started = true;

    ESP_LOGI(
        TAG,
        "Time manager started, server=%s, re-sync=%u ms, timezone=Asia/Ho_Chi_Minh",
        SNTP_SERVER_NAME,
        (unsigned int)SNTP_SYNC_INTERVAL_MS
    );

    if (wifi_manager_is_connected()) {
        start_or_restart_sntp();
    }

    return ESP_OK;
}

bool time_manager_is_valid(void)
{
    if (s_time_event_group == NULL) {
        return false;
    }

    return (xEventGroupGetBits(s_time_event_group) & TIME_VALID_BIT) != 0;
}

int64_t time_manager_get_timestamp(void)
{
    if (!time_manager_is_valid()) {
        return -1;
    }

    return (int64_t)time(NULL);
}

esp_err_t time_manager_get_device_time(char *buf, size_t len)
{
    if (!time_manager_is_valid()) {
        return ESP_ERR_INVALID_STATE;
    }

    time_t now = time(NULL);
    return format_device_time(now, buf, len);
}

esp_err_t time_manager_get_snapshot(time_manager_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!time_manager_is_valid()) {
        return ESP_ERR_INVALID_STATE;
    }

    time_t now = time(NULL);
    esp_err_t err = format_device_time(
        now,
        snapshot->time_device,
        sizeof(snapshot->time_device)
    );
    if (err != ESP_OK) {
        return err;
    }

    snapshot->ts = (int64_t)now;
    return ESP_OK;
}
