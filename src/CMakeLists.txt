idf_component_register(SRCS "main.c" "status_led.c" "usb_printer.c" "print_job.c" "wifi.c" "improv_esp.c" "airprint_esp.c" "debug.c" "oled.c" "qemu_selftest.c"
                       INCLUDE_DIRS "."
                       REQUIRES usb canon_bjl improv esp_driver_uart airprint printers scanner webui oled esp_driver_rmt esp_driver_gpio esp_driver_i2c esp_timer esp_wifi
                                esp_netif esp_eth nvs_flash mdns
                                esp_http_server app_update espcoredump esp_partition)
