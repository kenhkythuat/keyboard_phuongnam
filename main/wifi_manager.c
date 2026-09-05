#include "wifi_manager.h"

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#define WIFI_MANAGER_SSID                  "Technical IOT"
#define WIFI_MANAGER_PASSWORD              "123456789"
#define WIFI_RECONNECT_INITIAL_DELAY_MS    1000U
#define WIFI_RECONNECT_MAX_DELAY_MS        30000U

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "WIFI_MANAGER";

static EventGroupHandle_t s_wifi_event_group;
static TimerHandle_t s_reconnect_timer;
static esp_netif_t *s_station_netif;
static uint32_t s_reconnect_delay_ms = WIFI_RECONNECT_INITIAL_DELAY_MS;
static bool s_started;

static void schedule_reconnect(void)
{
    if (s_reconnect_timer == NULL || wifi_manager_is_connected()) {
        return;
    }

    uint32_t delay_ms = s_reconnect_delay_ms;
    TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
    if (delay_ticks == 0) {
        delay_ticks = 1;
    }

    if (xTimerChangePeriod(s_reconnect_timer, delay_ticks, 0) != pdPASS) {
        ESP_LOGE(TAG, "Khong the hen lich reconnect Wi-Fi");
        return;
    }

    ESP_LOGI(TAG, "Se reconnect Wi-Fi sau %" PRIu32 " ms", delay_ms);

    if (s_reconnect_delay_ms < WIFI_RECONNECT_MAX_DELAY_MS) {
        uint32_t next_delay = s_reconnect_delay_ms * 2U;
        s_reconnect_delay_ms = next_delay > WIFI_RECONNECT_MAX_DELAY_MS
                                   ? WIFI_RECONNECT_MAX_DELAY_MS
                                   : next_delay;
    }
}

static void reconnect_timer_callback(TimerHandle_t timer)
{
    (void)timer;

    if (wifi_manager_is_connected()) {
        return;
    }

    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Dang reconnect Wi-Fi SSID: %s", WIFI_MANAGER_SSID);
        return;
    }

    if (err == ESP_ERR_WIFI_CONN) {
        ESP_LOGD(TAG, "Wi-Fi dang trong qua trinh ket noi");
        return;
    }

    ESP_LOGW(TAG, "Reconnect Wi-Fi chua khoi dong duoc: %s", esp_err_to_name(err));
    schedule_reconnect();
}

static void wifi_event_handler(void *argument,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)argument;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "Khong the bat dau ket noi Wi-Fi: %s", esp_err_to_name(err));
            schedule_reconnect();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;

        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%u", (unsigned int)event->reason);
        schedule_reconnect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;

        s_reconnect_delay_ms = WIFI_RECONNECT_INITIAL_DELAY_MS;
        xTimerStop(s_reconnect_timer, 0);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS can khoi tao lai de Wi-Fi hoat dong");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }

    return err;
}

esp_err_t wifi_manager_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t err = initialize_nvs();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_reconnect_timer = xTimerCreate(
        "wifi_reconnect",
        pdMS_TO_TICKS(WIFI_RECONNECT_INITIAL_DELAY_MS),
        pdFALSE,
        NULL,
        reconnect_timer_callback
    );
    if (s_reconnect_timer == NULL) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_station_netif = esp_netif_create_default_wifi_sta();
    if (s_station_netif == NULL) {
        xTimerDelete(s_reconnect_timer, 0);
        vEventGroupDelete(s_wifi_event_group);
        s_reconnect_timer = NULL;
        s_wifi_event_group = NULL;
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, WIFI_MANAGER_SSID, sizeof(WIFI_MANAGER_SSID));
    memcpy(wifi_config.sta.password, WIFI_MANAGER_PASSWORD, sizeof(WIFI_MANAGER_PASSWORD));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "Wi-Fi Station started for SSID: %s", WIFI_MANAGER_SSID);
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }

    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_wait_for_connection(uint32_t timeout_ms)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }

    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0 && timeout_ticks == 0) {
        timeout_ticks = 1;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        timeout_ticks
    );

    return (bits & WIFI_CONNECTED_BIT) != 0;
}
