// WS2812 driver on the RMT peripheral plus a small task that animates the
// current state.
#include "status_led.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "led";

#define RMT_RES_HZ (10 * 1000 * 1000)   // 0.1 us per tick

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static volatile led_state_t s_state = LED_OFF;
static volatile int s_identify;     // remaining identify flashes (x2: on + off)
static TaskHandle_t s_task;

static void led_write(uint8_t r, uint8_t g, uint8_t b)
{
    // brightness cap applies to every colour
    const uint32_t pct = CONFIG_STATUS_LED_BRIGHTNESS;
    uint8_t grb[3] = {(uint8_t)(g * pct / 100), (uint8_t)(r * pct / 100), (uint8_t)(b * pct / 100)};
    rmt_transmit_config_t tx = {.loop_count = 0};
    if (rmt_transmit(s_chan, s_enc, grb, sizeof(grb), &tx) == ESP_OK)
        rmt_tx_wait_all_done(s_chan, pdMS_TO_TICKS(50));
}

static void led_task(void *arg)
{
    bool phase = false;
    led_state_t last = (led_state_t)-1;
    while (1) {
        if (s_identify > 0) {
            led_write(0, 0, (s_identify & 1) ? 0 : 255);
            s_identify--;
            last = (led_state_t)-1;     // redraw the state afterwards
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(150));
            continue;
        }
        led_state_t st = s_state;
        if (st != last)
            phase = true;               // start every new state with the LED on
        last = st;
        uint32_t period_ms = 500;
        switch (st) {
        case LED_WAITING:
            led_write(255, 180, 0);
            break;
        case LED_READY:
            led_write(0, 255, 0);
            break;
        case LED_BUSY:
            period_ms = 250;
            if (phase)
                led_write(0, 255, 0);
            else
                led_write(0, 0, 0);
            break;
        case LED_ERROR:
            period_ms = 100;
            if (phase)
                led_write(255, 0, 0);
            else
                led_write(0, 0, 0);
            break;
        default:
            led_write(0, 0, 0);
            break;
        }
        phase = !phase;
        // wake early when the state changes
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(period_ms));
    }
}

esp_err_t status_led_init(void)
{
    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = CONFIG_STATUS_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&chan_cfg, &s_chan), TAG, "rmt channel");
    // WS2812: 0 = 0.3 us high + 0.9 us low, 1 = 0.9 us high + 0.3 us low
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&enc_cfg, &s_enc), TAG, "encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(s_chan), TAG, "rmt enable");
    led_write(0, 0, 0);
    if (xTaskCreate(led_task, "status_led", 3072, NULL, 3, &s_task) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void status_led_identify(void)
{
    s_identify = 10;
    if (s_task)
        xTaskNotifyGive(s_task);
}

void status_led_set(led_state_t state)
{
    if (s_state == state)
        return;
    s_state = state;
    if (s_task)
        xTaskNotifyGive(s_task);
}
