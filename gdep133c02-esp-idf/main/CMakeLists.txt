idf_component_register(
    SRCS "main.c" "frame.c" "comm.c" "display.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_server esp_http_client nvs_flash esp_netif esp_event
             esp_driver_gpio esp_driver_spi esp_timer esp_hw_support
)
