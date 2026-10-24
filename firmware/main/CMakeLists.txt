idf_component_register(
    SRCS "main.c" "scene.c" "motion.c" "voice.c" "net.c" "console.c" "answers.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_client json esp_timer mbedtls
             esp_driver_usb_serial_jtag esp_vfs_console
    EMBED_FILES "fonts/BebasNeue-Regular.ttf")
