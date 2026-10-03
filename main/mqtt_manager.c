#include "mqtt_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "mqtt_client.h"

#include "device_config.h"
#include "device_settings.h"
#include "wifi_manager.h"

#define MQTT_CONNECTED_BIT              BIT0
#define MQTT_READY_BIT                  BIT1
#define MQTT_QOS                        1
#define MQTT_KEEPALIVE_SECONDS          60
#define MQTT_RECONNECT_INITIAL_MS       1000U
#define MQTT_RECONNECT_MAX_MS           60000U
#define MQTT_RECONNECT_JITTER_MAX_MS    500U
#define MQTT_TOPIC_BUFFER_SIZE          96U
#define MQTT_CONTROL_TASK_STACK_SIZE    4096U
#define MQTT_CONTROL_TASK_PRIORITY      5U
#define MQTT_CONTROL_NOTIFY_RECONNECT   BIT0
#define MQTT_CONTROL_NOTIFY_ROLLBACK    BIT1

static const char *TAG = "MQTT_MANAGER";

static EventGroupHandle_t s_mqtt_event_group;
static TimerHandle_t s_reconnect_timer;
static TimerHandle_t s_mqtt_config_trial_timer;
static TaskHandle_t s_mqtt_control_task;
static esp_mqtt_client_handle_t s_client;
static mqtt_command_callback_t s_command_callback;
static uint32_t s_reconnect_delay_ms = MQTT_RECONNECT_INITIAL_MS;
static bool s_client_started;
static bool s_initialized;
static int s_command_subscribe_message_id = -1;
static device_core_config_t s_runtime_config;
static bool s_mqtt_trial_resolution_started;

static char s_telemetry_topic[MQTT_TOPIC_BUFFER_SIZE];
static char s_command_topic[MQTT_TOPIC_BUFFER_SIZE];
static char s_ack_topic[MQTT_TOPIC_BUFFER_SIZE];
static char s_event_topic[MQTT_TOPIC_BUFFER_SIZE];

static void start_mqtt_config_trial_timer(void);
static void request_connection(void);

static void mqtt_config_trial_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    if (s_mqtt_control_task == NULL ||
        xTaskNotify(s_mqtt_control_task, MQTT_CONTROL_NOTIFY_ROLLBACK,
                    eSetBits) != pdPASS) {
        ESP_LOGE(TAG, "Khong notify duoc task rollback MQTT config");
    }
}

static void start_mqtt_config_trial_timer(void)
{
    if (s_mqtt_config_trial_timer == NULL ||
        !device_settings_mqtt_config_trial_is_pending() ||
        !wifi_manager_is_connected() ||
        __atomic_load_n(&s_mqtt_trial_resolution_started,
                        __ATOMIC_ACQUIRE)) {
        return;
    }

    if (xTimerChangePeriod(
            s_mqtt_config_trial_timer,
            pdMS_TO_TICKS(MQTT_CONFIG_TRIAL_TIMEOUT_MS), 0) != pdPASS) {
        ESP_LOGE(TAG, "Khong khoi dong duoc timeout thu MQTT config");
    } else {
        ESP_LOGI(TAG, "Bat dau thu MQTT config moi, timeout=%u ms",
                 (unsigned)MQTT_CONFIG_TRIAL_TIMEOUT_MS);
    }
}

static bool payload_contains(const char *payload, size_t payload_length,
                             const char *needle)
{
    size_t needle_length = strlen(needle);
    if (payload == NULL || needle_length == 0U ||
        payload_length < needle_length) {
        return false;
    }
    for (size_t index = 0U;
         index + needle_length <= payload_length; index++) {
        if (memcmp(payload + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool node_id_is_valid(const char *node_id)
{
    static const char prefix[] = "node_kbd_";

    if (node_id == NULL ||
        strncmp(node_id, prefix, sizeof(prefix) - 1U) != 0 ||
        strlen(node_id) != (sizeof(prefix) - 1U) + 3U) {
        return false;
    }

    const char *number = node_id + sizeof(prefix) - 1U;
    if (number[0] < '0' || number[0] > '9' ||
        number[1] < '0' || number[1] > '9' ||
        number[2] < '0' || number[2] > '9') {
        return false;
    }

    int value = ((number[0] - '0') * 100) +
                ((number[1] - '0') * 10) +
                (number[2] - '0');
    return value >= 1 && value <= 999;
}

static esp_err_t build_topic(char *buffer, size_t size, const char *direction)
{
    int written = snprintf(
        buffer,
        size,
        "tbmq/keyboard/%s/%s",
        s_runtime_config.node_id,
        direction
    );

    if (written < 0 || (size_t)written >= size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t build_topics(void)
{
    esp_err_t err = build_topic(
        s_telemetry_topic,
        sizeof(s_telemetry_topic),
        "telemetry"
    );
    if (err != ESP_OK) {
        return err;
    }

    err = build_topic(s_command_topic, sizeof(s_command_topic), "command");
    if (err != ESP_OK) {
        return err;
    }

    err = build_topic(s_ack_topic, sizeof(s_ack_topic), "ack");
    if (err != ESP_OK) {
        return err;
    }

    return build_topic(s_event_topic, sizeof(s_event_topic), "event");
}

static esp_err_t validate_configuration(void)
{
    if (!device_settings_core_config_is_valid(&s_runtime_config) ||
        !node_id_is_valid(s_runtime_config.node_id)) {
        ESP_LOGE(TAG, "Core MQTT config trong NVS khong hop le");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_runtime_config.mqtt_username[0] == '\0' ||
        s_runtime_config.mqtt_password[0] == '\0') {
        ESP_LOGE(TAG, "MQTT credential cho %s chua duoc cau hinh",
                 s_runtime_config.node_id);
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static void schedule_reconnect(void)
{
    if (s_reconnect_timer == NULL ||
        mqtt_manager_is_connected() ||
        !wifi_manager_is_connected()) {
        return;
    }

    uint32_t jitter_ms = esp_random() % (MQTT_RECONNECT_JITTER_MAX_MS + 1U);
    uint32_t scheduled_delay_ms = s_reconnect_delay_ms + jitter_ms;
    TickType_t delay_ticks = pdMS_TO_TICKS(scheduled_delay_ms);
    if (delay_ticks == 0) {
        delay_ticks = 1;
    }

    if (xTimerChangePeriod(s_reconnect_timer, delay_ticks, 0) != pdPASS) {
        ESP_LOGE(TAG, "Khong the hen lich MQTT reconnect");
        return;
    }

    ESP_LOGI(
        TAG,
        "MQTT reconnect sau %" PRIu32 " ms (backoff=%" PRIu32
        " jitter=%" PRIu32 ")",
        scheduled_delay_ms,
        s_reconnect_delay_ms,
        jitter_ms
    );

    if (s_reconnect_delay_ms < MQTT_RECONNECT_MAX_MS) {
        uint32_t next_delay = s_reconnect_delay_ms * 2U;
        s_reconnect_delay_ms = next_delay > MQTT_RECONNECT_MAX_MS
                                   ? MQTT_RECONNECT_MAX_MS
                                   : next_delay;
    }
}

static void request_connection(void)
{
    if (!wifi_manager_is_connected() || mqtt_manager_is_connected()) {
        return;
    }

    esp_err_t err;
    if (!s_client_started) {
        err = esp_mqtt_client_start(s_client);
        if (err == ESP_OK) {
            s_client_started = true;
            ESP_LOGI(TAG, "MQTT client started: %s",
                     s_runtime_config.node_id);
            return;
        }
    } else {
        err = esp_mqtt_client_reconnect(s_client);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Da yeu cau MQTT reconnect");
            return;
        }
    }

    ESP_LOGW(TAG, "Khong the bat dau MQTT connection: %s", esp_err_to_name(err));
    schedule_reconnect();
}

static void reconnect_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    if (s_mqtt_control_task == NULL ||
        xTaskNotify(s_mqtt_control_task, MQTT_CONTROL_NOTIFY_RECONNECT,
                    eSetBits) != pdPASS) {
        ESP_LOGE(TAG, "Khong notify duoc task MQTT reconnect");
    }
}

static void mqtt_control_task(void *argument)
{
    (void)argument;
    while (1) {
        uint32_t notifications = 0U;
        if (xTaskNotifyWait(0U, UINT32_MAX, &notifications,
                            portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if ((notifications & MQTT_CONTROL_NOTIFY_ROLLBACK) != 0U) {
            bool expected = false;
            if (__atomic_compare_exchange_n(
                    &s_mqtt_trial_resolution_started, &expected, true,
                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                ESP_LOGW(TAG,
                         "MQTT config moi khong ready sau %u ms, rollback",
                         (unsigned)MQTT_CONFIG_TRIAL_TIMEOUT_MS);
                esp_err_t err =
                    device_settings_rollback_mqtt_config_trial();
                if (err == ESP_OK) {
                    ESP_LOGW(TAG,
                             "Rollback thanh cong, reboot de dung MQTT config cu");
                    vTaskDelay(pdMS_TO_TICKS(500U));
                    esp_restart();
                }

                ESP_LOGE(TAG, "Rollback MQTT config that bai: %s",
                         esp_err_to_name(err));
                __atomic_store_n(&s_mqtt_trial_resolution_started, false,
                                 __ATOMIC_RELEASE);
                start_mqtt_config_trial_timer();
            }
        }

        if ((notifications & MQTT_CONTROL_NOTIFY_RECONNECT) != 0U) {
            request_connection();
        }
    }
}

static bool event_topic_is_command(const esp_mqtt_event_handle_t event)
{
    size_t command_topic_length = strlen(s_command_topic);

    return event->topic != NULL &&
           event->topic_len == (int)command_topic_length &&
           memcmp(event->topic, s_command_topic, command_topic_length) == 0;
}

static bool subscription_was_granted(
    const esp_mqtt_event_handle_t event)
{
    if (event->error_handle != NULL &&
        event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
        return false;
    }
    if (event->data == NULL || event->data_len <= 0) {
        return false;
    }
    for (int index = 0; index < event->data_len; index++) {
        if ((uint8_t)event->data[index] >= 0x80U) {
            return false;
        }
    }
    return true;
}

static void mqtt_event_handler(void *argument,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)argument;
    (void)event_base;

    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            xTimerStop(s_reconnect_timer, 0);
            s_reconnect_delay_ms = MQTT_RECONNECT_INITIAL_MS;
            xEventGroupClearBits(s_mqtt_event_group, MQTT_READY_BIT);
            xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);

            s_command_subscribe_message_id = esp_mqtt_client_subscribe(
                s_client,
                s_command_topic,
                MQTT_QOS
            );

            if (s_command_subscribe_message_id < 0) {
                ESP_LOGE(TAG, "Subscribe command that bai: %s", s_command_topic);
            } else {
                ESP_LOGI(
                    TAG,
                    "MQTT connected, dang subscribe %s qos=%d msg_id=%d",
                    s_command_topic,
                    MQTT_QOS,
                    s_command_subscribe_message_id
                );
            }
            break;
        }

        case MQTT_EVENT_SUBSCRIBED:
            if (event->msg_id == s_command_subscribe_message_id) {
                if (!subscription_was_granted(event)) {
                    xEventGroupClearBits(s_mqtt_event_group, MQTT_READY_BIT);
                    ESP_LOGE(TAG, "Broker tu choi subscribe command topic");
                    break;
                }
                xEventGroupSetBits(s_mqtt_event_group, MQTT_READY_BIT);
                ESP_LOGI(TAG, "MQTT ready, command topic da subscribe");
                if (device_settings_mqtt_config_trial_is_pending()) {
                    bool expected = false;
                    if (__atomic_compare_exchange_n(
                            &s_mqtt_trial_resolution_started, &expected,
                            true, false, __ATOMIC_ACQ_REL,
                            __ATOMIC_ACQUIRE)) {
                        xTimerStop(s_mqtt_config_trial_timer, 0);
                        esp_err_t confirm_err =
                            device_settings_confirm_mqtt_config_trial();
                        if (confirm_err == ESP_OK) {
                            ESP_LOGI(TAG, "MQTT config moi da ket noi thanh cong");
                        } else {
                            ESP_LOGE(TAG, "Khong confirm duoc MQTT config moi: %s",
                                     esp_err_to_name(confirm_err));
                            __atomic_store_n(
                                &s_mqtt_trial_resolution_started, false,
                                __ATOMIC_RELEASE);
                            start_mqtt_config_trial_timer();
                        }
                    }
                }
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            xEventGroupClearBits(
                s_mqtt_event_group,
                MQTT_CONNECTED_BIT | MQTT_READY_BIT
            );
            s_command_subscribe_message_id = -1;
            ESP_LOGW(TAG, "MQTT disconnected");
            schedule_reconnect();
            break;

        case MQTT_EVENT_DATA:
            if (event_topic_is_command(event)) {
                if (payload_contains(event->data, (size_t)event->data_len,
                                     "\"set_mqtt_config\"")) {
                    ESP_LOGI(TAG,
                             "Nhan command MQTT: topic=%.*s "
                             "payload=<redacted:set_mqtt_config> length=%d",
                             event->topic_len, event->topic,
                             event->data_len);
                } else {
                    ESP_LOGI(
                        TAG,
                        "Nhan command MQTT: topic=%.*s payload=%.*s length=%d",
                        event->topic_len,
                        event->topic,
                        event->data_len,
                        event->data,
                        event->data_len
                    );
                }
                if (s_command_callback != NULL) {
                    s_command_callback(event->data, (size_t)event->data_len);
                }
            } else {
                ESP_LOGW(TAG, "Bo qua MQTT data ngoai command topic");
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT transport/protocol error");
            if (device_settings_mqtt_config_trial_is_pending() &&
                event->error_handle != NULL &&
                event->error_handle->error_type ==
                    MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
                (event->error_handle->connect_return_code ==
                     MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                 event->error_handle->connect_return_code ==
                     MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) &&
                s_mqtt_control_task != NULL) {
                ESP_LOGE(TAG,
                         "MQTT config moi bi tu choi xac thuc, rollback ngay");
                xTaskNotify(s_mqtt_control_task,
                            MQTT_CONTROL_NOTIFY_ROLLBACK, eSetBits);
            }
            break;

        default:
            break;
    }
}

static void network_event_handler(void *argument,
                                  esp_event_base_t event_base,
                                  int32_t event_id,
                                  void *event_data)
{
    (void)argument;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xTimerStop(s_reconnect_timer, 0);
        if (s_mqtt_config_trial_timer != NULL &&
            device_settings_mqtt_config_trial_is_pending() &&
            !__atomic_load_n(&s_mqtt_trial_resolution_started,
                             __ATOMIC_ACQUIRE)) {
            xTimerStop(s_mqtt_config_trial_timer, 0);
        }
        xEventGroupClearBits(
            s_mqtt_event_group,
            MQTT_CONNECTED_BIT | MQTT_READY_BIT
        );
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        start_mqtt_config_trial_timer();
        if (s_mqtt_control_task != NULL) {
            xTaskNotify(s_mqtt_control_task, MQTT_CONTROL_NOTIFY_RECONNECT,
                        eSetBits);
        }
    }
}

static esp_err_t publish_json(const char *topic, const char *json_payload)
{
    if (topic == NULL || json_payload == NULL || json_payload[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (!mqtt_manager_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    int message_id = esp_mqtt_client_enqueue(
        s_client,
        topic,
        json_payload,
        0,
        MQTT_QOS,
        0,
        true
    );

    if (message_id < 0) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "MQTT queued topic=%s qos=%d msg_id=%d", topic, MQTT_QOS, message_id);
    return ESP_OK;
}

esp_err_t mqtt_manager_start(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = device_settings_get_core_config(&s_runtime_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Khong doc duoc core MQTT config tu NVS: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = validate_configuration();
    if (err != ESP_OK) {
        return err;
    }

    err = build_topics();
    if (err != ESP_OK) {
        return err;
    }

    s_mqtt_event_group = xEventGroupCreate();
    if (s_mqtt_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_reconnect_timer = xTimerCreate(
        "mqtt_reconnect",
        pdMS_TO_TICKS(MQTT_RECONNECT_INITIAL_MS),
        pdFALSE,
        NULL,
        reconnect_timer_callback
    );
    if (s_reconnect_timer == NULL) {
        vEventGroupDelete(s_mqtt_event_group);
        s_mqtt_event_group = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_mqtt_config_trial_timer = xTimerCreate(
        "mqtt_cfg_trial",
        pdMS_TO_TICKS(MQTT_CONFIG_TRIAL_TIMEOUT_MS),
        pdFALSE,
        NULL,
        mqtt_config_trial_timer_callback
    );
    if (s_mqtt_config_trial_timer == NULL) {
        xTimerDelete(s_reconnect_timer, 0);
        vEventGroupDelete(s_mqtt_event_group);
        s_reconnect_timer = NULL;
        s_mqtt_event_group = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_config_t config = {
        .broker.address.hostname = s_runtime_config.mqtt_broker_host,
        .broker.address.port = s_runtime_config.mqtt_broker_port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.username = s_runtime_config.mqtt_username,
        .credentials.client_id = s_runtime_config.node_id,
        .credentials.authentication.password = s_runtime_config.mqtt_password,
        .session.disable_clean_session = MQTT_CLEAN_SESSION ? false : true,
        .session.keepalive = MQTT_KEEPALIVE_SECONDS,
        .network.disable_auto_reconnect = true,
    };

    s_client = esp_mqtt_client_init(&config);
    if (s_client == NULL) {
        xTimerDelete(s_mqtt_config_trial_timer, 0);
        xTimerDelete(s_reconnect_timer, 0);
        vEventGroupDelete(s_mqtt_event_group);
        s_reconnect_timer = NULL;
        s_mqtt_config_trial_timer = NULL;
        s_mqtt_event_group = NULL;
        return ESP_FAIL;
    }

    err = esp_mqtt_client_register_event(
        s_client,
        ESP_EVENT_ANY_ID,
        mqtt_event_handler,
        NULL
    );
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        network_event_handler,
        NULL
    );
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(
        WIFI_EVENT,
        WIFI_EVENT_STA_DISCONNECTED,
        network_event_handler,
        NULL
    );
    if (err != ESP_OK) {
        return err;
    }

    s_initialized = true;

    if (xTaskCreate(mqtt_control_task, "mqtt_control",
                    MQTT_CONTROL_TASK_STACK_SIZE, NULL,
                    MQTT_CONTROL_TASK_PRIORITY,
                    &s_mqtt_control_task) != pdPASS) {
        s_initialized = false;
        ESP_LOGE(TAG, "Khong tao duoc MQTT control task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "MQTT manager initialized, client_id=%s broker=%s:%u",
             s_runtime_config.node_id,
             s_runtime_config.mqtt_broker_host,
             (unsigned)s_runtime_config.mqtt_broker_port);
    ESP_LOGI(TAG, "Telemetry topic: %s", s_telemetry_topic);
    ESP_LOGI(TAG, "Command topic: %s", s_command_topic);
    ESP_LOGI(TAG, "ACK topic: %s", s_ack_topic);
    ESP_LOGI(TAG, "Event topic: %s", s_event_topic);

    if (wifi_manager_is_connected()) {
        start_mqtt_config_trial_timer();
        xTaskNotify(s_mqtt_control_task, MQTT_CONTROL_NOTIFY_RECONNECT,
                    eSetBits);
    }

    return ESP_OK;
}

bool mqtt_manager_is_connected(void)
{
    if (s_mqtt_event_group == NULL) {
        return false;
    }

    return (xEventGroupGetBits(s_mqtt_event_group) & MQTT_CONNECTED_BIT) != 0;
}

bool mqtt_manager_is_ready(void)
{
    if (s_mqtt_event_group == NULL) {
        return false;
    }

    EventBits_t required_bits = MQTT_CONNECTED_BIT | MQTT_READY_BIT;
    return (xEventGroupGetBits(s_mqtt_event_group) & required_bits) ==
           required_bits;
}

void mqtt_manager_set_command_callback(mqtt_command_callback_t callback)
{
    s_command_callback = callback;
}

esp_err_t mqtt_manager_publish_telemetry(const char *json_payload)
{
    if (!mqtt_manager_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    return publish_json(s_telemetry_topic, json_payload);
}

esp_err_t mqtt_manager_publish_ack(const char *json_payload)
{
    return publish_json(s_ack_topic, json_payload);
}

esp_err_t mqtt_manager_publish_event(const char *json_payload)
{
    return publish_json(s_event_topic, json_payload);
}
