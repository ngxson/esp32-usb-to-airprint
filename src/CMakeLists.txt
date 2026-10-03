idf_component_register(SRCS "main.c" "status_led.c" "usb_printer.c" "print_job.c" "wifi.c" "airprint_esp.c" "debug.c" "qemu_selftest.c"
                       INCLUDE_DIRS "."
                       REQUIRES usb canon_bjl airprint scanner webui esp_driver_rmt esp_driver_gpio esp_timer esp_wifi
                                esp_netif esp_eth nvs_flash mdns
                                esp_http_server app_update espcoredump esp_partition)
