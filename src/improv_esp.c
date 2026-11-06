// Improv Wi-Fi on the console UART (the board's COM port): the web installer
// (flasher/, esp-web-tools) finds the firmware after flashing, lists the
// networks the board sees, sends the chosen one and links to the web page.
//
// The console's log shares the port: once the UART driver is installed the
// console writes through it too, so a log line never lands inside a packet
// (uart_write_bytes is atomic per call). Packets bypass stdio: its \n -> \r\n
// conversion would break them.
#include "improv_esp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "improv.h"
#include "local_config.h"
#include "sdkconfig.h"
#include "wifi.h"

static const char *TAG = "improv";

#define PORT CONFIG_ESP_CONSOLE_UART_NUM
#define CONNECT_TIMEOUT_MS 25000    // the installer gives up after 45 s

static improv_t s_improv;

static void write_packet(const uint8_t *data, size_t len, void *ctx)
{
    uart_write_bytes(PORT, data, len);
}

static bool page_url(char *buf, size_t n, void *ctx)
{
    char ip[16];
    if (!wifi_get_ip(ip, sizeof(ip)))
        return false;
    snprintf(buf, n, "http://%s/", ip);
    return true;
}

static bool join_network(const char *ssid, const char *password, void *ctx)
{
    return wifi_set_network(ssid, password, CONNECT_TIMEOUT_MS);
}

static int scan(improv_network_t *out, int max, void *ctx)
{
    wifi_network_t *nets = calloc((size_t)max, sizeof(*nets));
    int n = nets ? wifi_scan(nets, max) : -1;
    for (int i = 0; i < n; i++) {
        memcpy(out[i].ssid, nets[i].ssid, sizeof(out[i].ssid));     // both 33 bytes, terminated
        out[i].rssi = nets[i].rssi;
        out[i].secured = nets[i].secured;
    }
    free(nets);
    return n;
}

static void improv_task(void *arg)
{
    uint8_t buf[64];
    for (;;) {
        int n = uart_read_bytes(PORT, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n > 0)
            improv_feed(&s_improv, buf, (size_t)n);
    }
}

esp_err_t improv_esp_start(void)
{
    // the smallest receive buffer the driver takes (more than the 128-byte FIFO):
    // the installer sends one request at a time, at most 76 bytes
    esp_err_t err = uart_driver_install(PORT, 256, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%d driver: %s", PORT, esp_err_to_name(err));
        return err;
    }
    uart_vfs_dev_use_driver(PORT);
    improv_config_t cfg = {
        .firmware = IMPROV_FIRMWARE_NAME,
        .version = esp_app_get_description()->version,
        .chip = "ESP32-S3",
        .name = PRINTER_NAME,
        .write = write_packet,
        .url = page_url,
        .connect = join_network,
        .scan = scan,
    };
    improv_init(&s_improv, &cfg);
    // internal RAM (it writes to NVS); the packets (2 x 266 bytes), the WiFi
    // driver's scan and change calls and the log are on this stack
    if (xTaskCreate(improv_task, "improv", 4096, NULL, 3, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
