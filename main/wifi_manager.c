#include "wifi_manager.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "device_config.h"
#include "wifi_config_portal.h"

#define WIFI_RECONNECT_INITIAL_DELAY_MS  1000U
#define WIFI_RECONNECT_MAX_DELAY_MS      30000U
#define WIFI_MANAGER_TASK_PERIOD_MS      25U
#define WIFI_MANAGER_TASK_STACK_SIZE     4096U
#define WIFI_MANAGER_TASK_PRIORITY       5U
#define WIFI_CONNECTED_BIT               BIT0
#define WIFI_NOTIFY_GOT_IP               BIT0
#define WIFI_NOTIFY_TRIAL_FAILED          BIT1
#define WIFI_CREDENTIAL_NAMESPACE        "wifi_cfg"
#define WIFI_CREDENTIAL_KEY              "credentials"
#define WIFI_CREDENTIAL_MAGIC            0x57494649UL
#define WIFI_CREDENTIAL_VERSION          1U
#define WIFI_SSID_MAX_LENGTH             32U
#define WIFI_PASSWORD_MAX_LENGTH         64U
#define WIFI_AP_PASSWORD_MAX_LENGTH      63U

typedef struct {
    char ssid[WIFI_SSID_MAX_LENGTH + 1U];
    char password[WIFI_PASSWORD_MAX_LENGTH + 1U];
} wifi_credentials_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    wifi_credentials_t credentials;
} wifi_credential_store_t;

static const char *TAG = "WIFI_MANAGER";

static EventGroupHandle_t s_wifi_event_group;
static TimerHandle_t s_reconnect_timer;
static QueueHandle_t s_credential_queue;
static TaskHandle_t s_manager_task;
static esp_netif_t *s_station_netif;
static esp_netif_t *s_ap_netif;
static wifi_credentials_t s_active_credentials;
static wifi_credentials_t s_pending_credentials;
static uint32_t s_reconnect_delay_ms = WIFI_RECONNECT_INITIAL_DELAY_MS;
static bool s_have_credentials;
static bool s_credentials_persisted;
static bool s_started;
static bool s_config_mode;
static bool s_trial_active;
static bool s_ignore_next_disconnect;
static wifi_manager_result_t s_connection_result;

static esp_err_t save_credentials(const wifi_credentials_t *credentials);

_Static_assert(sizeof(WIFI_DEFAULT_SSID) - 1U <= WIFI_SSID_MAX_LENGTH,
               "WIFI_DEFAULT_SSID is too long");
_Static_assert(sizeof(WIFI_DEFAULT_PASSWORD) - 1U <= WIFI_PASSWORD_MAX_LENGTH,
               "WIFI_DEFAULT_PASSWORD is too long");
_Static_assert(sizeof(WIFI_CONFIG_AP_SSID) - 1U <= WIFI_SSID_MAX_LENGTH,
               "WIFI_CONFIG_AP_SSID is too long");
_Static_assert(sizeof(WIFI_CONFIG_AP_PASSWORD) == 1U ||
                   (sizeof(WIFI_CONFIG_AP_PASSWORD) - 1U >= 8U &&
                    sizeof(WIFI_CONFIG_AP_PASSWORD) - 1U <=
                        WIFI_AP_PASSWORD_MAX_LENGTH),
               "WIFI_CONFIG_AP_PASSWORD must be empty or 8..63 characters");

static bool credentials_are_valid(const wifi_credentials_t *credentials)
{
    if (credentials == NULL) {
        return false;
    }

    size_t ssid_length = strnlen(credentials->ssid,
                                 sizeof(credentials->ssid));
    size_t password_length = strnlen(credentials->password,
                                     sizeof(credentials->password));
    bool password_is_valid = password_length == 0U ||
                             (password_length >= 8U &&
                              password_length < WIFI_PASSWORD_MAX_LENGTH);
    if (password_length == WIFI_PASSWORD_MAX_LENGTH) {
        password_is_valid = true;
        for (size_t index = 0U; index < password_length; index++) {
            char character = credentials->password[index];
            if (!((character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f') ||
                  (character >= 'A' && character <= 'F'))) {
                password_is_valid = false;
                break;
            }
        }
    }
    return ssid_length > 0U && ssid_length <= WIFI_SSID_MAX_LENGTH &&
           password_is_valid;
}

static void load_factory_credentials(wifi_credentials_t *credentials)
{
    memset(credentials, 0, sizeof(*credentials));
    strlcpy(credentials->ssid, WIFI_DEFAULT_SSID,
            sizeof(credentials->ssid));
    strlcpy(credentials->password, WIFI_DEFAULT_PASSWORD,
            sizeof(credentials->password));
}

static esp_err_t seed_factory_credentials(void)
{
    load_factory_credentials(&s_active_credentials);
    s_have_credentials = credentials_are_valid(&s_active_credentials);
    s_credentials_persisted = false;
    if (!s_have_credentials) {
        ESP_LOGI(TAG, "Chua co Wi-Fi trong NVS va factory SSID rong");
        return ESP_OK;
    }

    esp_err_t err = save_credentials(&s_active_credentials);
    if (err != ESP_OK) {
        return err;
    }
    s_credentials_persisted = true;
    ESP_LOGI(TAG, "Da seed Wi-Fi factory vao NVS, SSID=%s",
             s_active_credentials.ssid);
    return ESP_OK;
}

static esp_err_t load_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CREDENTIAL_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return seed_factory_credentials();
    }
    if (err != ESP_OK) {
        return err;
    }

    wifi_credential_store_t store = {0};
    size_t size = sizeof(store);
    err = nvs_get_blob(handle, WIFI_CREDENTIAL_KEY, &store, &size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_INVALID_LENGTH) {
        return seed_factory_credentials();
    }
    if (err != ESP_OK) {
        return err;
    }
    if (size != sizeof(store) || store.magic != WIFI_CREDENTIAL_MAGIC ||
        store.version != WIFI_CREDENTIAL_VERSION ||
        !credentials_are_valid(&store.credentials)) {
        ESP_LOGW(TAG, "Wi-Fi credential trong NVS khong hop le, dung cau hinh firmware");
        return seed_factory_credentials();
    }

    s_active_credentials = store.credentials;
    s_have_credentials = true;
    s_credentials_persisted = true;
    ESP_LOGI(TAG, "Da tai Wi-Fi credential tu NVS, SSID=%s",
             s_active_credentials.ssid);
    return ESP_OK;
}

static esp_err_t save_credentials(const wifi_credentials_t *credentials)
{
    if (!credentials_are_valid(credentials)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_credentials_persisted &&
        strcmp(credentials->ssid, s_active_credentials.ssid) == 0 &&
        strcmp(credentials->password, s_active_credentials.password) == 0) {
        return ESP_OK;
    }

    wifi_credential_store_t store = {
        .magic = WIFI_CREDENTIAL_MAGIC,
        .version = WIFI_CREDENTIAL_VERSION,
    };
    store.credentials = *credentials;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CREDENTIAL_NAMESPACE, NVS_READWRITE,
                             &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, WIFI_CREDENTIAL_KEY, &store, sizeof(store));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err == ESP_OK) {
        wifi_credential_store_t readback = {0};
        size_t size = sizeof(readback);
        err = nvs_get_blob(handle, WIFI_CREDENTIAL_KEY, &readback, &size);
        if (err == ESP_OK &&
            (size != sizeof(readback) ||
             memcmp(&readback, &store, sizeof(store)) != 0)) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    nvs_close(handle);
    return err;
}

static void fill_station_config(const wifi_credentials_t *credentials,
                                wifi_config_t *config)
{
    memset(config, 0, sizeof(*config));
    memcpy(config->sta.ssid, credentials->ssid,
           strlen(credentials->ssid));
    memcpy(config->sta.password, credentials->password,
           strlen(credentials->password));
    config->sta.threshold.authmode = WIFI_AUTH_OPEN;
    config->sta.pmf_cfg.capable = true;
    config->sta.pmf_cfg.required = false;
}

static void schedule_reconnect(void)
{
    if (s_reconnect_timer == NULL || wifi_manager_is_connected() ||
        wifi_manager_is_config_mode() || !s_have_credentials) {
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
    if (wifi_manager_is_connected() || wifi_manager_is_config_mode() ||
        !s_have_credentials) {
        return;
    }

    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Dang reconnect Wi-Fi SSID: %s",
                 s_active_credentials.ssid);
        return;
    }
    if (err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "Reconnect Wi-Fi chua khoi dong duoc: %s",
                 esp_err_to_name(err));
        schedule_reconnect();
    }
}

static esp_err_t portal_submit_callback(const char *ssid,
                                        const char *password,
                                        void *context)
{
    (void)context;
    if (s_credential_queue == NULL || !wifi_manager_is_config_mode()) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_credentials_t credentials = {0};
    strlcpy(credentials.ssid, ssid, sizeof(credentials.ssid));
    strlcpy(credentials.password, password, sizeof(credentials.password));
    if (!credentials_are_valid(&credentials)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueOverwrite(s_credential_queue, &credentials) != pdPASS) {
        memset(&credentials, 0, sizeof(credentials));
        return ESP_FAIL;
    }
    memset(&credentials, 0, sizeof(credentials));
    return ESP_OK;
}

static esp_err_t start_config_mode(void)
{
    if (wifi_manager_is_config_mode()) {
        return ESP_OK;
    }

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, WIFI_CONFIG_AP_SSID,
            sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(WIFI_CONFIG_AP_SSID);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    if (WIFI_CONFIG_AP_PASSWORD[0] == '\0') {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        strlcpy((char *)ap_config.ap.password, WIFI_CONFIG_AP_PASSWORD,
                sizeof(ap_config.ap.password));
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    xTimerStop(s_reconnect_timer, 0);
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    }
    if (err != ESP_OK) {
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        return err;
    }

    __atomic_store_n(&s_config_mode, true, __ATOMIC_RELEASE);
    wifi_config_portal_set_status(WIFI_CONFIG_PORTAL_READY);
    err = wifi_config_portal_start(portal_submit_callback, NULL);
    if (err != ESP_OK) {
        __atomic_store_n(&s_config_mode, false, __ATOMIC_RELEASE);
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        return err;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi Config Mode: AP=%s IP=" IPSTR,
                 WIFI_CONFIG_AP_SSID, IP2STR(&ip_info.ip));
    } else {
        ESP_LOGI(TAG, "WiFi Config Mode: AP=%s", WIFI_CONFIG_AP_SSID);
    }
    return ESP_OK;
}

static void finish_config_mode(bool credentials_saved)
{
    __atomic_store_n(&s_trial_active, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_config_mode, false, __ATOMIC_RELEASE);
    wifi_config_portal_stop();
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Khong the tat SoftAP: %s", esp_err_to_name(err));
    }
    if (credentials_saved) {
        ESP_LOGI(TAG, "Da luu Wi-Fi moi va tro ve Station mode, SSID=%s",
                 s_active_credentials.ssid);
    } else {
        ESP_LOGW(TAG, "Wi-Fi moi that bai, dong portal va tro ve Station mode");
        if (!wifi_manager_is_connected()) {
            schedule_reconnect();
        }
    }
}

static void begin_credential_trial(const wifi_credentials_t *credentials)
{
    s_pending_credentials = *credentials;
    __atomic_store_n(&s_connection_result, WIFI_MANAGER_RESULT_NONE,
                     __ATOMIC_RELEASE);
    wifi_config_portal_set_status(WIFI_CONFIG_PORTAL_CONNECTING);

    bool was_connected = wifi_manager_is_connected();
    if (was_connected) {
        __atomic_store_n(&s_ignore_next_disconnect, true, __ATOMIC_RELEASE);
        if (esp_wifi_disconnect() != ESP_OK) {
            __atomic_store_n(&s_ignore_next_disconnect, false,
                             __ATOMIC_RELEASE);
        }
    }

    wifi_config_t station_config;
    fill_station_config(credentials, &station_config);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &station_config);
    if (err == ESP_OK) {
        __atomic_store_n(&s_trial_active, true, __ATOMIC_RELEASE);
        err = esp_wifi_connect();
    }
    memset(&station_config, 0, sizeof(station_config));
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        __atomic_store_n(&s_trial_active, false, __ATOMIC_RELEASE);
        __atomic_store_n(&s_connection_result, WIFI_MANAGER_RESULT_FAIL,
                         __ATOMIC_RELEASE);
        wifi_config_portal_set_status(WIFI_CONFIG_PORTAL_CONNECTION_FAILED);
        ESP_LOGW(TAG, "Khong the thu ket noi SSID=%s: %s",
                 credentials->ssid, esp_err_to_name(err));
        if (s_manager_task != NULL) {
            xTaskNotify(s_manager_task, WIFI_NOTIFY_TRIAL_FAILED, eSetBits);
        }
        return;
    }
    ESP_LOGI(TAG, "Dang thu Wi-Fi moi, SSID=%s", credentials->ssid);
}

static void handle_trial_connected(void)
{
    if (!wifi_manager_is_config_mode() ||
        !__atomic_load_n(&s_trial_active, __ATOMIC_ACQUIRE)) {
        return;
    }

    esp_err_t err = save_credentials(&s_pending_credentials);
    if (err != ESP_OK) {
        __atomic_store_n(&s_connection_result, WIFI_MANAGER_RESULT_FAIL,
                         __ATOMIC_RELEASE);
        wifi_config_portal_set_status(WIFI_CONFIG_PORTAL_STORAGE_FAILED);
        ESP_LOGE(TAG, "Wi-Fi moi da ket noi nhung khong luu duoc NVS: %s",
                 esp_err_to_name(err));
        __atomic_store_n(&s_trial_active, false, __ATOMIC_RELEASE);
        if (s_manager_task != NULL) {
            xTaskNotify(s_manager_task, WIFI_NOTIFY_TRIAL_FAILED, eSetBits);
        }
        return;
    }

    s_active_credentials = s_pending_credentials;
    memset(&s_pending_credentials, 0, sizeof(s_pending_credentials));
    s_have_credentials = true;
    s_credentials_persisted = true;
    __atomic_store_n(&s_connection_result, WIFI_MANAGER_RESULT_DONE,
                     __ATOMIC_RELEASE);
    finish_config_mode(true);
}

static void restore_previous_connection(void)
{
    memset(&s_pending_credentials, 0, sizeof(s_pending_credentials));
    if (!wifi_manager_is_config_mode() || !s_have_credentials) {
        return;
    }

    wifi_config_t station_config;
    fill_station_config(&s_active_credentials, &station_config);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &station_config);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    memset(&station_config, 0, sizeof(station_config));
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "Khong the khoi phuc Wi-Fi cu SSID=%s: %s",
                 s_active_credentials.ssid, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Dang khoi phuc Wi-Fi cu, portal van mo, SSID=%s",
             s_active_credentials.ssid);
}

static void manager_task(void *argument)
{
    (void)argument;
    int candidate_level = gpio_get_level((gpio_num_t)WIFI_CONFIG_BUTTON_GPIO);
    int stable_level = candidate_level;
    TickType_t candidate_since = xTaskGetTickCount();
    TickType_t pressed_since = candidate_since;
    bool hold_triggered = false;
    while (true) {
        uint32_t notification = 0U;
        (void)xTaskNotifyWait(0U, UINT32_MAX, &notification,
                              pdMS_TO_TICKS(WIFI_MANAGER_TASK_PERIOD_MS));
        if ((notification & WIFI_NOTIFY_GOT_IP) != 0U) {
            handle_trial_connected();
        }
        if ((notification & WIFI_NOTIFY_TRIAL_FAILED) != 0U) {
            restore_previous_connection();
        }

        wifi_credentials_t submitted;
        if (xQueueReceive(s_credential_queue, &submitted, 0) == pdPASS) {
            begin_credential_trial(&submitted);
            memset(&submitted, 0, sizeof(submitted));
        }

        TickType_t now = xTaskGetTickCount();
        int sample = gpio_get_level((gpio_num_t)WIFI_CONFIG_BUTTON_GPIO);
        if (sample != candidate_level) {
            candidate_level = sample;
            candidate_since = now;
        }
        if (candidate_level != stable_level &&
            (now - candidate_since) >= pdMS_TO_TICKS(WIFI_CONFIG_DEBOUNCE_MS)) {
            stable_level = candidate_level;
            if (stable_level == 0) {
                pressed_since = now;
                hold_triggered = false;
            } else {
                hold_triggered = false;
            }
        }
        if (stable_level == 0 && !hold_triggered &&
            (now - pressed_since) >= pdMS_TO_TICKS(WIFI_CONFIG_HOLD_TIME_MS)) {
            hold_triggered = true;
            esp_err_t err = start_config_mode();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Khong the vao WiFi Config Mode: %s",
                         esp_err_to_name(err));
            }
        }
    }
}

static void wifi_event_handler(void *argument,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)argument;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_have_credentials) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "Khong the bat dau ket noi Wi-Fi: %s",
                         esp_err_to_name(err));
                schedule_reconnect();
            }
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        bool ignore = __atomic_exchange_n(&s_ignore_next_disconnect, false,
                                          __ATOMIC_ACQ_REL);
        if (wifi_manager_is_config_mode()) {
            if (!ignore &&
                __atomic_load_n(&s_trial_active, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&s_trial_active, false, __ATOMIC_RELEASE);
                __atomic_store_n(&s_connection_result,
                                 WIFI_MANAGER_RESULT_FAIL,
                                 __ATOMIC_RELEASE);
                wifi_config_portal_set_status(
                    WIFI_CONFIG_PORTAL_CONNECTION_FAILED);
                ESP_LOGW(TAG, "Wi-Fi moi ket noi that bai, reason=%u",
                         (unsigned)event->reason);
                if (s_manager_task != NULL) {
                    xTaskNotify(s_manager_task, WIFI_NOTIFY_TRIAL_FAILED,
                                eSetBits);
                }
            }
            return;
        }

        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%u",
                 (unsigned)event->reason);
        schedule_reconnect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        s_reconnect_delay_ms = WIFI_RECONNECT_INITIAL_DELAY_MS;
        xTimerStop(s_reconnect_timer, 0);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR,
                 IP2STR(&event->ip_info.ip));
        if (s_manager_task != NULL) {
            xTaskNotify(s_manager_task, WIFI_NOTIFY_GOT_IP, eSetBits);
        }
    }
}

static esp_err_t initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS can khoi tao lai de Wi-Fi hoat dong");
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
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
    s_credential_queue = xQueueCreate(1U, sizeof(wifi_credentials_t));
    s_reconnect_timer = xTimerCreate(
        "wifi_reconnect", pdMS_TO_TICKS(WIFI_RECONNECT_INITIAL_DELAY_MS),
        pdFALSE, NULL, reconnect_timer_callback);
    if (s_wifi_event_group == NULL || s_credential_queue == NULL ||
        s_reconnect_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_station_netif = esp_netif_create_default_wifi_sta();
    if (s_station_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_event_handler, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         wifi_event_handler, NULL);
    }
    if (err != ESP_OK) {
        return err;
    }

    err = load_credentials();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    if (s_have_credentials) {
        wifi_config_t station_config;
        fill_station_config(&s_active_credentials, &station_config);
        err = esp_wifi_set_config(WIFI_IF_STA, &station_config);
        memset(&station_config, 0, sizeof(station_config));
        if (err != ESP_OK) {
            return err;
        }
    }

    gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << WIFI_CONFIG_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&button_config);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(manager_task, "wifi_manager", WIFI_MANAGER_TASK_STACK_SIZE,
                    NULL, WIFI_MANAGER_TASK_PRIORITY, &s_manager_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    err = esp_wifi_start();
    if (err != ESP_OK) {
        s_started = false;
        return err;
    }

    if (s_have_credentials) {
        ESP_LOGI(TAG, "Wi-Fi Station started for SSID: %s",
                 s_active_credentials.ssid);
    } else {
        ESP_LOGW(TAG, "Wi-Fi Station started without credentials; hold IO0 for 7 seconds to configure");
    }
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_wifi_event_group != NULL &&
           (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_config_mode(void)
{
    return __atomic_load_n(&s_config_mode, __ATOMIC_ACQUIRE);
}

wifi_manager_result_t wifi_manager_get_connection_result(void)
{
    return __atomic_load_n(&s_connection_result, __ATOMIC_ACQUIRE);
}

void wifi_manager_acknowledge_connection_result(void)
{
    wifi_manager_result_t result = wifi_manager_get_connection_result();
    __atomic_store_n(&s_connection_result, WIFI_MANAGER_RESULT_NONE,
                     __ATOMIC_RELEASE);
    if (result == WIFI_MANAGER_RESULT_FAIL &&
        wifi_manager_is_config_mode()) {
        finish_config_mode(false);
    }
}

bool wifi_manager_wait_for_connection(uint32_t timeout_ms)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0U && timeout_ticks == 0) {
        timeout_ticks = 1;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
        timeout_ticks);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_manager_get_rssi(int8_t *rssi)
{
    if (rssi == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wifi_manager_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_ap_record_t access_point;
    esp_err_t err = esp_wifi_sta_get_ap_info(&access_point);
    if (err == ESP_OK) {
        *rssi = access_point.rssi;
    }
    return err;
}
