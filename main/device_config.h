#pragma once

/*
 * Factory seed values. On first boot they are copied to NVS. Later firmware
 * builds and OTA updates load NVS and do not overwrite provisioned values.
 * NODE_ID must be unique for every board: node_kbd_001 .. node_kbd_999.
 */
#define NODE_ID             "node_kbd_002"

#define MQTT_BROKER_HOST    "161.248.146.170"
#define MQTT_BROKER_PORT    1883

/* Replace with the credentials issued specifically for this NODE_ID. */
#define MQTT_USERNAME       "thuanphat"
#define MQTT_PASSWORD       "123456789"

#define MQTT_CLEAN_SESSION  1

/* Public GitHub Raw release files. Private repositories require authentication. */
#define OTA_GITHUB_RAW_BASE \
    "https://raw.githubusercontent.com/kenhkythuat/keyboard_phuongnam/"
#define OTA_RELEASE_BRANCH "feature/control_kepad"
#define OTA_MANIFEST_URL \
    OTA_GITHUB_RAW_BASE OTA_RELEASE_BRANCH "/OTA/version.json"
#define OTA_FIRMWARE_URL \
    OTA_GITHUB_RAW_BASE OTA_RELEASE_BRANCH "/OTA/file.bin"

/* Factory Wi-Fi seed copied to NVS when credentials have not been saved yet. */
#define WIFI_DEFAULT_SSID              "Technical IOT"
#define WIFI_DEFAULT_PASSWORD          "123456789"

/* Wi-Fi setup portal configuration. Change the AP password before deployment. */
#define WIFI_CONFIG_AP_SSID            "ESP32-Keyboard-Setup"
#define WIFI_CONFIG_AP_PASSWORD        "12345678"
#define WIFI_CONFIG_BUTTON_GPIO        0
#define WIFI_CONFIG_HOLD_TIME_MS       7000U
#define WIFI_CONFIG_DEBOUNCE_MS        50U
#define WIFI_CONNECTION_RESULT_DISPLAY_MS 5000U

/* Used until a successful calibration command stores another mode in NVS. */
#define DEFAULT_MODE_CALIBRATION "P1E"
