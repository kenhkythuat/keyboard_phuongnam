idf_component_register(
    SRCS
        "device_settings.c"
        "wifi_manager.c"
        "mqtt_manager.c"
        "mqtt_command_handler.c"
        "pump_data_sniffer.c"
        "pump_transaction_filter.c"
        "pump_transaction_store.c"
        "time_manager.c"
        "main.c"
        "control_display_led.c"
    INCLUDE_DIRS ""
    REQUIRES driver esp_timer esp_wifi esp_event esp_netif nvs_flash lwip mqtt json
)
