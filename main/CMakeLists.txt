idf_component_register(
    SRCS
        "device_settings.c"
        "wifi_manager.c"
        "wifi_config_portal.c"
        "mqtt_manager.c"
        "mqtt_command_handler.c"
        "ota_manager.c"
        "pump_data_sniffer.c"
        "pump_transaction_filter.c"
        "pump_transaction_store.c"
        "telemetry_heartbeat.c"
        "time_manager.c"
        "main.c"
        "control_display_led.c"
    INCLUDE_DIRS ""
    REQUIRES driver esp_timer esp_wifi esp_event esp_netif esp_http_server nvs_flash lwip mqtt json esp_https_ota app_update mbedtls
)
