// I2C driver for the status screen and the task that keeps it up to date.
#include "oled.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "local_config.h"
#include "oled_ui.h"
#include "scan_service.h"
#include "sdkconfig.h"
#include "usb_printer.h"
#include "wifi.h"

static const char *TAG = "oled";

#define PERIOD_MS 250
#define RETRY_MS 5000           // looking for a display that stopped answering
#define PROBE_TRIES 3           // at boot, in case the module is still powering up
#define PROBE_GAP_MS 50
#define I2C_TIMEOUT_MS 50
#define I2C_HZ 400000

#ifdef CONFIG_OLED_SH1106
#define COLUMN_OFFSET 2         // 132 columns of RAM, the panel shows 2..129
#else
#define COLUMN_OFFSET 0
#endif

static const char *s_hostname;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint16_t s_addr;
static volatile bool s_wake;
static volatile bool s_asleep;  // turned off for burn-in (not: lost, or no display)
static TaskHandle_t s_task;

// static: too big for the task's stack
static oled_state_t s_state;
static uint8_t s_fb[OLED_FB_SIZE], s_sent[OLED_FB_SIZE], s_content[OLED_FB_SIZE];

static esp_err_t cmds(const uint8_t *c, size_t n)
{
    uint8_t buf[32];
    buf[0] = 0x00;      // control byte: commands follow
    memcpy(buf + 1, c, n);
    return i2c_master_transmit(s_dev, buf, n + 1, I2C_TIMEOUT_MS);
}

static esp_err_t cmd1(uint8_t c)
{
    return cmds(&c, 1);
}

static esp_err_t controller_init(void)
{
#ifdef CONFIG_OLED_FLIP
    const uint8_t seg = 0xa0, com = 0xc0;      // rotated by 180 degrees
#else
    const uint8_t seg = 0xa1, com = 0xc8;
#endif
#ifdef CONFIG_OLED_SH1106
    const uint8_t init[] = {
        0xae,               // display off
        0xd5, 0x80,         // clock
        0xa8, 0x3f,         // 64 rows
        0xd3, 0x00,         // no vertical offset
        0x40,               // start line 0
        0xad, 0x8b,         // DC-DC on
        seg, com,           // orientation
        0xda, 0x12,         // COM pins
        0x81, 0x80,         // contrast
        0xd9, 0x22,         // precharge
        0xdb, 0x35,         // VCOM level
        0xa4,               // show the RAM
        0xa6,               // not inverted
    };
#else
    const uint8_t init[] = {
        0xae,               // display off
        0xd5, 0x80,         // clock
        0xa8, 0x3f,         // 64 rows
        0xd3, 0x00,         // no vertical offset
        0x40,               // start line 0
        0x8d, 0x14,         // charge pump on
        0x20, 0x02,         // page addressing (as the SH1106)
        seg, com,           // orientation
        0xda, 0x12,         // COM pins
        0x81, 0xcf,         // contrast
        0xd9, 0xf1,         // precharge
        0xdb, 0x40,         // VCOM level
        0x2e,               // no scrolling
        0xa4,               // show the RAM
        0xa6,               // not inverted
    };
#endif
    return cmds(init, sizeof(init));
}

static esp_err_t send_page(int page)
{
    const uint8_t pos[] = {(uint8_t)(0xb0 | page), (uint8_t)(COLUMN_OFFSET & 0x0f),
                           (uint8_t)(0x10 | COLUMN_OFFSET >> 4)};
    esp_err_t err = cmds(pos, sizeof(pos));
    if (err != ESP_OK)
        return err;
    uint8_t buf[1 + OLED_WIDTH];
    buf[0] = 0x40;      // control byte: data follows
    memcpy(buf + 1, s_fb + page * OLED_WIDTH, OLED_WIDTH);
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

// Sends the pages that changed since the last call (all of them if force).
static esp_err_t flush(bool force)
{
    for (int p = 0; p < OLED_HEIGHT / 8; p++) {
        const uint8_t *now = s_fb + p * OLED_WIDTH, *old = s_sent + p * OLED_WIDTH;
        if (!force && !memcmp(now, old, OLED_WIDTH))
            continue;
        esp_err_t err = send_page(p);
        if (err != ESP_OK)
            return err;
        memcpy(s_sent + p * OLED_WIDTH, now, OLED_WIDTH);
    }
    return ESP_OK;
}

// The address of the display, 0 if nothing answers at 0x3C or 0x3D.
static uint16_t find_display(void)
{
    static const uint16_t addrs[] = {0x3c, 0x3d};
    for (int t = 0; t < PROBE_TRIES; t++) {
        if (t)
            vTaskDelay(pdMS_TO_TICKS(PROBE_GAP_MS));
        for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++)
            if (i2c_master_probe(s_bus, addrs[i], I2C_TIMEOUT_MS) == ESP_OK)
                return addrs[i];
    }
    return 0;
}

// Sets the display up again after it stopped answering (a loose wire).
static bool reattach(void)
{
    if (i2c_master_probe(s_bus, s_addr, I2C_TIMEOUT_MS) != ESP_OK || controller_init() != ESP_OK)
        return false;
    ESP_LOGI(TAG, "display back at 0x%02x", s_addr);
    return true;
}

static void gather(oled_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->name = s_hostname;
    st->wifi_configured = wifi_configured();
    if (wifi_connected()) {
        wifi_get_ip(st->ip, sizeof(st->ip));
        st->rssi = wifi_rssi();
    }
    st->started = airprint_get_activity(&st->print) == 0;
    if (!st->started) {
        // no WiFi: the button's test page still works
        st->print.status.connected = usb_printer_connected();
        st->print.status.busy = usb_printer_job_active();
        st->print.job.percent = -1;
        st->print.job.ended_ago = -1;
    }
    if (st->started)
        scan_service_get_info(&st->scan);
}

static void oled_task(void *arg)
{
    bool attached = controller_init() == ESP_OK;
    bool on = false;
    int64_t changed_at = esp_timer_get_time(), retry_at = 0;
    const int64_t sleep_us = (int64_t)CONFIG_OLED_SLEEP_MIN * 60 * 1000000;
    while (1) {
        int64_t now = esp_timer_get_time();
        if (!attached) {
            if (now >= retry_at) {
                attached = reattach();
                retry_at = now + RETRY_MS * 1000LL;
                on = false;
                changed_at = now;
            }
            if (!attached) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PERIOD_MS));
                continue;
            }
        }
        gather(&s_state);
        oled_ui_render(&s_state, s_fb);
        // the WiFi signal alone does not count as a change
        uint8_t saved[24];
        memcpy(saved, s_fb + OLED_WIDTH - sizeof(saved), sizeof(saved));
        memset(s_fb + OLED_WIDTH - sizeof(saved), 0, sizeof(saved));
        if (memcmp(s_fb, s_content, OLED_FB_SIZE)) {
            memcpy(s_content, s_fb, OLED_FB_SIZE);
            changed_at = now;
        }
        memcpy(s_fb + OLED_WIDTH - sizeof(saved), saved, sizeof(saved));
        if (s_wake) {
            s_wake = false;
            changed_at = now;
        }

        esp_err_t err = ESP_OK;
        bool want_on = sleep_us == 0 || now - changed_at < sleep_us;
        if (want_on && !on) {
            err = flush(true);
            if (err == ESP_OK)
                err = cmd1(0xaf);       // display on
            on = err == ESP_OK;
            s_asleep = false;
        } else if (want_on) {
            err = flush(false);
        } else if (on) {
            err = cmd1(0xae);           // display off (burn-in)
            on = err != ESP_OK;
            s_asleep = !on;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "display lost: %s", esp_err_to_name(err));
            attached = false;
            s_asleep = false;
            retry_at = now + RETRY_MS * 1000LL;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PERIOD_MS));
    }
}

void oled_start(const char *hostname)
{
    s_hostname = hostname;
    i2c_master_bus_config_t bus = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_OLED_SDA_GPIO,
        .scl_io_num = CONFIG_OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   // modules have their own; these are weak
    };
    esp_err_t err = i2c_new_master_bus(&bus, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus: %s", esp_err_to_name(err));
        return;
    }
    s_addr = find_display();
    if (!s_addr) {
        // no screen: give the pins back and run without one
        ESP_LOGI(TAG, "no display at 0x3c or 0x3d (SDA GPIO%d, SCL GPIO%d): screen disabled", CONFIG_OLED_SDA_GPIO,
                 CONFIG_OLED_SCL_GPIO);
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_addr,
        .scl_speed_hz = I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display at 0x%02x: %s", s_addr, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "display at 0x%02x (SDA GPIO%d, SCL GPIO%d)", s_addr, CONFIG_OLED_SDA_GPIO, CONFIG_OLED_SCL_GPIO);
    if (xTaskCreate(oled_task, "oled", 4096, NULL, 2, &s_task) != pdPASS)
        ESP_LOGE(TAG, "cannot start the display task");
}

bool oled_wake(void)
{
    if (!s_task)
        return false;
    bool asleep = s_asleep;
    s_asleep = false;   // only the first press on a dark screen is swallowed
    s_wake = true;
    xTaskNotifyGive(s_task);
    return asleep;
}
