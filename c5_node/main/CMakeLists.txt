idf_component_register(
    SRCS "main.c" "proto.c" "wifi_scanner.c" "zb_sniffer.c" "wifi_attacker.c" "led_fx.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif nvs_flash esp_event ieee802154 esp_timer esp_driver_rmt
)
