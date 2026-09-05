#include "mqtt_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "mqtt_client.h"

#include "device_config.h"
#include "wifi_manager.h"

#define MQTT_CONNECTED_BIT              BIT0
#define MQTT_QOS                        1
#define MQTT_KEEPALIVE_SECONDS          60
#define MQTT_RECONNECT_INITIAL_MS       1000U
#define MQTT_RECONNECT_MAX_MS           60000U
#define MQTT_RECONNECT_JITTER_MAX_MS    500U
#define MQTT_TOPIC_BUFFER_SIZE          96U

static const char *TAG = "MQTT_MANAGER";

static EventGroupHandle_t s_mqtt_event_group;
static TimerHandle_t s_reconnect_timer;
static esp_mqtt_client_handle_t s_client;
static mqtt_command_callback_t s_command_callback;
static uint32_t s_reconnect_delay_ms = MQTT_RECONNECT_INITIAL_MS;
static bool s_client_started;
static bool s_initialized;

static char s_telemetry_topic[MQTT_TOPIC_BUFFER_SIZE];
static char s_command_topic[MQTT_TOPIC_BUFFER_SIZE];
static char s_ack_topic[MQTT_TOPIC_BUFFER_SIZE];
static char s_event_topic[MQTT_TOPIC_BUFFER_SIZE];

static bool node_id_is_valid(void)
{
    static const char prefix[] = "node_kbd_";

    if (strncmp(NODE_ID, prefix, sizeof(prefix) - 1U) != 0 ||
        strlen(NODE_ID) != (sizeof(prefix) - 1U) + 3U) {
        return false;
    }

    const char *number = NODE_ID + sizeof(prefix) - 1U;
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
        NODE_ID,
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
    if (!node_id_is_valid()) {
        ESP_LOGE(TAG, "NODE_ID khong hop le: %s", NODE_ID);
        return ESP_ERR_INVALID_ARG;
    }

    if (MQTT_USERNAME[0] == '\0' || MQTT_PASSWORD[0] == '\0') {
        ESP_LOGE(TAG, "MQTT credential cho %s chua duoc cau hinh", NODE_ID);
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
            ESP_LOGI(TAG, "MQTT client started: %s", NODE_ID);
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
    request_connection();
}

static bool event_topic_is_command(const esp_mqtt_event_handle_t event)
{
    size_t command_topic_length = strlen(s_command_topic);

    return event->topic != NULL &&
           event->topic_len == (int)command_topic_length &&
           memcmp(event->topic, s_command_topic, command_topic_length) == 0;
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
            xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);

            int message_id = esp_mqtt_client_subscribe(
                s_client,
                s_command_topic,
                MQTT_QOS
            );

            if (message_id < 0) {
                ESP_LOGE(TAG, "Subscribe command that bai: %s", s_command_topic);
            } else {
                ESP_LOGI(
                    TAG,
                    "MQTT connected, subscribed %s qos=%d msg_id=%d",
                    s_command_topic,
                    MQTT_QOS,
                    message_id
                );
            }
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
            xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
            ESP_LOGW(TAG, "MQTT disconnected");
            schedule_reconnect();
            break;

        case MQTT_EVENT_DATA:
            if (event_topic_is_command(event)) {
                ESP_LOGI(TAG, "Nhan command MQTT, length=%d", event->data_len);
                if (s_command_callback != NULL) {
                    s_command_callback(event->data, (size_t)event->data_len);
                }
            } else {
                ESP_LOGW(TAG, "Bo qua MQTT data ngoai command topic");
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT transport/protocol error");
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
        xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        request_connection();
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

    esp_err_t err = build_topics();
    if (err != ESP_OK) {
        return err;
    }

    err = validate_configuration();
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

    esp_mqtt_client_config_t config = {
        .broker.address.hostname = MQTT_BROKER_HOST,
        .broker.address.port = MQTT_BROKER_PORT,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.username = MQTT_USERNAME,
        .credentials.client_id = NODE_ID,
        .credentials.authentication.password = MQTT_PASSWORD,
        .session.disable_clean_session = MQTT_CLEAN_SESSION ? false : true,
        .session.keepalive = MQTT_KEEPALIVE_SECONDS,
        .network.disable_auto_reconnect = true,
    };

    s_client = esp_mqtt_client_init(&config);
    if (s_client == NULL) {
        xTimerDelete(s_reconnect_timer, 0);
        vEventGroupDelete(s_mqtt_event_group);
        s_reconnect_timer = NULL;
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

    ESP_LOGI(TAG, "MQTT manager initialized, client_id=%s", NODE_ID);
    ESP_LOGI(TAG, "Telemetry topic: %s", s_telemetry_topic);
    ESP_LOGI(TAG, "Command topic: %s", s_command_topic);
    ESP_LOGI(TAG, "ACK topic: %s", s_ack_topic);
    ESP_LOGI(TAG, "Event topic: %s", s_event_topic);

    if (wifi_manager_is_connected()) {
        request_connection();
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

void mqtt_manager_set_command_callback(mqtt_command_callback_t callback)
{
    s_command_callback = callback;
}

esp_err_t mqtt_manager_publish_telemetry(const char *json_payload)
{
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
