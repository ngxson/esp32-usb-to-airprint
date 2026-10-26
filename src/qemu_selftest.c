// Self test for the emulated ESP32-S3 (`pio run -e qemu`, tools/qemu_selftest.sh):
//  1. dry-run test page jobs, logging the stream CRC per mode
//  2. the AirPrint/IPP server on QEMU's emulated Ethernet (OpenEth), with a
//     fake printer that logs the CRC of every job it receives, and the scan
//     service with the emulated scanner
//  3. the debug server (log, info, OTA updates with rollback), as in MODE=debug
// QEMU has no USB OTG controller, WiFi or WS2812, so those are left out.
#ifdef PRINTSERVER_QEMU_SELFTEST

#include <string.h>

#include "airprint.h"
#include "canon_bjl.h"
#include "debug.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fake_scanner.h"
#include "oled.h"
#include "print_job.h"
#include "scan_service.h"
#include "shared_buf.h"
#include "webui.h"

// No PSRAM in QEMU: a small buffer in internal RAM (a bigger one starves the
// network during the 600 dpi print test). Streams wrap it many times; a kept
// scan does not fit and must fail cleanly.
#define SCAN_BUF_SIZE (48 * 1024)

static const char *TAG = "selftest";

static SemaphoreHandle_t s_got_ip;
static volatile bool s_net_up;
static uint32_t s_crc, s_bytes;

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *e = data;
    ESP_LOGI(TAG, "ethernet up, IP " IPSTR, IP2STR(&e->ip_info.ip));
    s_net_up = true;
    xSemaphoreGive(s_got_ip);
}

static void start_openeth(void)
{
    s_got_ip = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&ncfg);
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = 1;
    phy_cfg.reset_gpio_num = -1;
    phy_cfg.autonego_timeout_ms = 100;
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_cfg);
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth;
    ESP_ERROR_CHECK(esp_eth_driver_install(&cfg, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}

// fake printer: checksum of the Canon stream
static void get_status(void *ctx, airprint_status_t *st)
{
    st->connected = true;
}

static int job_open(void *ctx)
{
    s_crc = 0;
    s_bytes = 0;
    return 0;
}

static int job_write(void *ctx, const uint8_t *data, size_t len)
{
    s_crc = bjl_crc32(s_crc, data, len);
    s_bytes += len;
    return 0;
}

static void job_close(void *ctx, bool ok)
{
    // runs on the IPP connection thread: how much of its stack was used?
    ESP_LOGI(TAG, "QEMU_JOB bytes=%lu crc32=%08lx ok=%d stack_free_min=%u heap_min=%lu", (unsigned long)s_bytes,
             (unsigned long)s_crc, ok, (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned long)esp_get_minimum_free_heap_size());
}

// emulated scanner, as fast as it goes
static fake_scanner_t s_scanner;

static int scan_open(void *ctx)
{
    return 0;
}

static void scan_close(void *ctx)
{
    ESP_LOGI(TAG, "QEMU_SCAN violations=%d stack_free_min=%u heap_min=%lu %s", s_scanner.errors,
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned long)esp_get_minimum_free_heap_size(),
             s_scanner.error);
}

static bool http_handler(void *ctx, airprint_http_t *h, const char *method, const char *path)
{
    return scan_service_http(h, method, path) || events_http(h, method, path) || webui_http(h, method, path);
}

static bool net_up(void)
{
    return s_net_up;
}

// long settle time: the test reboots an update before it confirms itself
static void ota_confirm_task(void *arg)
{
    debug_ota_confirm(net_up, 30, 300);
    vTaskDelete(NULL);
}

void app_main(void)
{
    debug_log_start(16 * 1024);
    xTaskCreate(ota_confirm_task, "ota_confirm", 3072, NULL, 2, NULL);
    oled_start("qemu");     // no display on the emulated I2C bus: must just say so
    for (int m = 0; m < BJL_MODE_COUNT; m++) {
        print_stats_t st;
        esp_err_t err = print_job_test_page(true, (bjl_mode_id_t)m, &st);
        ESP_LOGI(TAG, "QEMU_SELFTEST mode=%s bytes=%lu crc32=%08lx err=%s heap_min=%lu",
                 bjl_get_mode((bjl_mode_id_t)m)->name, (unsigned long)st.bytes, (unsigned long)st.crc32,
                 esp_err_to_name(err), (unsigned long)esp_get_minimum_free_heap_size());
    }

    start_openeth();
    if (xSemaphoreTake(s_got_ip, pdMS_TO_TICKS(20000)) != pdTRUE) {
        ESP_LOGE(TAG, "no IP address");
        return;
    }
    fake_scanner_init(&s_scanner, 0);
    sbuf_init(heap_caps_malloc(SCAN_BUF_SIZE, MALLOC_CAP_8BIT), SCAN_BUF_SIZE);
    ESP_LOGI(TAG, "shared buffer %s, free heap %lu (largest block %u)", sbuf_size() ? "allocated" : "MISSING",
             (unsigned long)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    static scan_config_t scfg = {.open = scan_open, .close = scan_close, .make_model = "Canon MG2500 series",
                                 .uuid = "a4d1e9b2-0b6f-4c3e-9d2a-000000000000"};
    scfg.io = fake_scanner_io(&s_scanner);
    scan_service_init(&scfg);
    static const airprint_config_t cfg = {
        .name = "Canon MG2500 (QEMU)",
        .make_model = "Canon MG2500 series",
        .uuid = "a4d1e9b2-0b6f-4c3e-9d2a-000000000000",
        .device_id = "MFG:Canon;MDL:MG2500 series;CMD:URF,PWG;CLS:PRINTER;",
        .port = 631,
        .web_port = 80,
        .get_status = get_status,
        .job_open = job_open,
        .job_write = job_write,
        .job_close = job_close,
        .http_handler = http_handler,
        .home_html = SCAN_HOME_HTML,
    };
    events_add_source("printer", airprint_status_json, NULL);
    events_add_source("scan", scan_service_status_json, NULL);
    airprint_start(&cfg);
    events_start();
    debug_server_start(NULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "QEMU_SELFTEST ipp ready");
}

#endif
