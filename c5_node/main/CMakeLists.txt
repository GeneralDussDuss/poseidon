idf_component_register(
    SRCS "main.c" "proto.c" "wifi_scanner.c" "zb_sniffer.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif nvs_flash esp_now esp_event esp_ieee802154
)
