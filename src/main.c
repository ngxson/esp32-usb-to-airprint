// ESP32-S3 print server for the Canon PIXMA MG2500 series.
//
// LED: blinking yellow = ready, no printer / solid green = printer attached /
//      blinking green = printing (test page or AirPrint) or scanning / fast red = last
//      job failed / blue flashes = Identify-Printer.
// BOOT button: short press = print an A4 colour test page (dry run without a
//      printer), hold 2 s while printing or scanning = cancel.
// AirPrint: joins the WiFi from .env and serves IPP on port 631; the status
//      and scan pages are at http://<host>/ and http://<host>/scan (port 80).
// MODE=debug: also the remote log, crash dumps and firmware updates over
//      WiFi (debug.h, tools/remote.py).
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "airprint_esp.h"
#include "debug.h"
#include "local_config.h"
#include "print_job.h"
#include "scan_service.h"
#include "sdkconfig.h"
#include "status_led.h"
#include "usb_printer.h"
#include "wifi.h"

static const char *TAG = "main";

#define LONG_PRESS_MS 2000
#define ERROR_SHOW_MS 3000

static volatile bool s_busy;            // test page task running
static volatile bool s_airprint_busy;   // AirPrint job running
static volatile bool s_showing_error;

static void show_idle_state(void)
{
    if (s_showing_error)
        return;
    if (s_busy || s_airprint_busy)
        status_led_set(LED_BUSY);
    else
        status_led_set(usb_printer_connected() ? LED_READY : LED_WAITING);
}

static void on_printer(bool connected, const usb_printer_info_t *info)
{
    if (connected)
        ESP_LOGI(TAG, "printer ready: %s %s", info->manufacturer, info->product);
    show_idle_state();
}

static void on_airprint_job(bool printing)
{
    s_airprint_busy = printing;
    show_idle_state();
}

static void print_task(void *arg)
{
    bool dry_run = (bool)(intptr_t)arg;
    print_stats_t st;
    status_led_set(LED_BUSY);
    esp_err_t err = print_job_test_page(dry_run, print_job_default_mode(), &st);
    if (dry_run)
        ESP_LOGI(TAG, "dry run done: %lu bytes, crc32 %08lx, %lu ms (%s)", (unsigned long)st.bytes,
                 (unsigned long)st.crc32, (unsigned long)st.ms, esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "print job finished: %s, %lu bytes in %lu ms", esp_err_to_name(err), (unsigned long)st.bytes,
                 (unsigned long)st.ms);
    s_busy = false;
    if (err != ESP_OK) {
        s_showing_error = true;
        status_led_set(LED_ERROR);
        vTaskDelay(pdMS_TO_TICKS(ERROR_SHOW_MS));
        s_showing_error = false;
    }
    show_idle_state();
    vTaskDelete(NULL);
}

static void on_short_press(void)
{
    if (s_busy || s_airprint_busy) {
        ESP_LOGI(TAG, "busy; hold BOOT for 2 s to cancel");
        return;
    }
    bool dry_run = !usb_printer_connected();
    if (dry_run)
        ESP_LOGI(TAG, "no printer attached: dry run (generate the job, discard it, log its CRC)");
    s_busy = true;
    // lower priority than the USB tasks; rendering is CPU heavy
    if (xTaskCreatePinnedToCore(print_task, "print", 8192, (void *)(intptr_t)dry_run, 4, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "cannot start print task");
        s_busy = false;
    }
}

static void button_task(void *arg)
{
    const gpio_num_t pin = CONFIG_PRINT_BUTTON_GPIO;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    bool stable = true;         // true = released (active low)
    int same = 0;
    TickType_t pressed_at = 0;
    bool long_fired = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        bool level = gpio_get_level(pin);
        if (level == stable) {
            same = 0;
        } else if (++same >= 3) {           // 30 ms debounce
            stable = level;
            same = 0;
            if (!stable) {
                pressed_at = xTaskGetTickCount();
                long_fired = false;
            } else if (!long_fired) {
                on_short_press();
            }
        }
        if (!stable && !long_fired && xTaskGetTickCount() - pressed_at >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
            long_fired = true;
            if (s_busy || s_airprint_busy) {
                ESP_LOGW(TAG, "cancel requested");
                usb_printer_cancel();
                scan_service_cancel();
            }
        }
    }
}

static volatile bool s_debug_server_up;

static bool device_busy(void)
{
    return usb_printer_job_active();
}

// a freshly updated firmware is kept once the WiFi (and in debug mode the
// debug server, the way to fix things) has worked for a while
static bool healthy(void)
{
    return wifi_connected() && (strcmp(MODE, "debug") != 0 || s_debug_server_up);
}

static void ota_confirm_task(void *arg)
{
    debug_ota_confirm(healthy, 20, 180);
    vTaskDelete(NULL);
}

#ifdef PRINTSERVER_QEMU_SELFTEST
// app_main is in qemu_selftest.c
void main_unused(void)
{
    (void)button_task;
    (void)on_printer;
    (void)on_airprint_job;
    (void)ota_confirm_task;
    (void)device_busy;
}
#else
void app_main(void)
{
    bool debug = strcmp(MODE, "debug") == 0;
    if (debug)
        debug_log_start(64 * 1024);
    ESP_ERROR_CHECK(status_led_init());
    status_led_set(LED_WAITING);
    ESP_ERROR_CHECK(usb_printer_start(on_printer));
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "ready: press BOOT to print a test page");

    xTaskCreate(ota_confirm_task, "ota_confirm", 3072, NULL, 2, NULL);
    // AirPrint; the printer keeps working locally if WiFi isn't configured
    if (wifi_start(HOSTNAME) == ESP_OK) {
        if (airprint_esp_start(HOSTNAME, on_airprint_job) != ESP_OK)
            ESP_LOGE(TAG, "AirPrint failed to start");
        if (debug)
            s_debug_server_up = debug_server_start(device_busy) == ESP_OK;
    }
}
#endif
