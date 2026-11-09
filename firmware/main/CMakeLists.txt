idf_component_register(SRCS "main.c" "pn5180_hal.c" "iso14443_isodep.c" "epd_driver.c" "wifi_portal.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_driver_gpio esp_driver_spi esp_timer freertos esp_wifi esp_event esp_netif nvs_flash esp_http_server spi_flash esp_partition)
