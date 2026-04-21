idf_component_register(
    SRCS "main.c" "proto.c" "wifi_scanner.c" "zb_sniffer.c" "wifi_attacker.c"
         "pmkid_capture.c" "led_fx.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif nvs_flash esp_event ieee802154 esp_timer esp_driver_rmt
)

# Sanity-check bypass now done by direct binary patch to
# ESP-IDF's libnet80211.a (see c5_node/README or tools/patch_libnet80211.sh
# if we later formalize it). The wifi_sanity_override.c source file and
# its --wrap / --allow-multiple-definition flags are gone — neither
# approach worked because esp_wifi_80211_tx and its call to the sanity
# check live in the same object file, so the linker already treated the
# symbol as resolved internally.
