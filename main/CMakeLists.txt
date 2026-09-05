idf_component_register(
    SRCS
        "wifi_manager.c"
        "pump_data_sniffer.c"
        "main.c"
        "control_display_led.c"
    INCLUDE_DIRS ""
    REQUIRES driver esp_timer esp_wifi esp_event esp_netif nvs_flash
)
