#include "print_job.h"

#include <stdio.h>
#include <string.h>

#include "canon_bjl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "testpage.h"
#include "usb_printer.h"

static const char *TAG = "print_job";

#if CONFIG_PRINTER_RES_300
#define TEST_PAGE_MODE BJL_MODE_300DPI
#else
#define TEST_PAGE_MODE BJL_MODE_600DPI
#endif

static int usb_sink(void *user, const uint8_t *data, size_t len)
{
    return usb_printer_write(data, len) == ESP_OK ? 0 : -1;
}

static int crc_sink(void *user, const uint8_t *data, size_t len)
{
    uint32_t *crc = user;
    *crc = bjl_crc32(*crc, data, len);
    return 0;
}

// "MDL:MG2500 series;" from the IEEE 1284 ID, else the USB product string
static void printer_label(char *out, size_t n)
{
    usb_printer_info_t info;
    if (!usb_printer_get_info(&info)) {
        snprintf(out, n, "PRINTER: ?");
        return;
    }
    const char *mdl = strstr(info.device_id, "MDL:");
    char model[sizeof(info.product)] = "";
    if (mdl) {
        mdl += 4;
        size_t i = 0;
        while (mdl[i] && mdl[i] != ';' && i < sizeof(model) - 1) {
            model[i] = mdl[i];
            i++;
        }
        model[i] = 0;
    } else {
        snprintf(model, sizeof(model), "%s", info.product);
    }
    snprintf(out, n, "PRINTER: %s %04X:%04X", model, info.vid, info.pid);
}

// Canon's driver waits for each IVEC response; Gutenprint does not wait at
// all. Wait a bit, but carry on either way.
static esp_err_t send_and_wait(const char *what, uint32_t timeout_ms)
{
    esp_err_t err = usb_printer_flush();
    if (err != ESP_OK)
        return err;
    err = usb_printer_wait_response(what, timeout_ms);
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_SUPPORTED) {
        if (err == ESP_ERR_TIMEOUT)
            ESP_LOGW(TAG, "no %s response within %lu ms, continuing", what, (unsigned long)timeout_ms);
        return ESP_OK;
    }
    if (err == ESP_OK)
        ESP_LOGI(TAG, "%s acknowledged", what);
    return err;
}

bjl_mode_id_t print_job_default_mode(void)
{
    return TEST_PAGE_MODE;
}

esp_err_t print_job_test_page(bool dry_run, bjl_mode_id_t mode, print_stats_t *stats)
{
    int64_t t0 = esp_timer_get_time();
    uint32_t crc = 0;
    bjl_job_t job;
    testpage_t tp;
    esp_err_t err = ESP_OK;
    memset(stats, 0, sizeof(*stats));

    if (!dry_run && (err = usb_printer_job_begin()) != ESP_OK) {
        ESP_LOGE(TAG, "printer not ready: %s", esp_err_to_name(err));
        return err;
    }
    if (bjl_job_init(&job, mode, BJL_PAPER_A4, dry_run ? crc_sink : usb_sink, &crc) != 0) {
        if (!dry_run)
            usb_printer_job_end();
        return ESP_ERR_NO_MEM;
    }
    char info1[64], info2[64];
    testpage_format_mode(&job, info1, sizeof(info1));
    if (dry_run)
        snprintf(info2, sizeof(info2), "%s", TESTPAGE_DRYRUN_INFO);
    else
        printer_label(info2, sizeof(info2));
    if (testpage_init(&tp, &job, info1, info2) != 0) {
        bjl_job_free(&job);
        if (!dry_run)
            usb_printer_job_end();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%s: A4 %s, %dx%d dots, free heap %lu", dry_run ? "dry run" : "printing", job.mode->name,
             job.width, job.height, (unsigned long)esp_get_free_heap_size());

    bjl_emit_start_job(&job);
    if (!dry_run && job.err == 0)
        err = send_and_wait("StartJob", 5000);
    if (err == ESP_OK)
        bjl_emit_mode_shift(&job);
    if (!dry_run && err == ESP_OK && job.err == 0)
        err = send_and_wait("ModeShift", 5000);

    if (err == ESP_OK && bjl_page_begin(&job) == 0) {
        for (int y = 0; y < job.height; y++) {
            if (bjl_page_add_line(&job, testpage_render_line(&tp, y)) != 0)
                break;
            if ((y & 63) == 63)
                vTaskDelay(1);      // let the idle task run (task watchdog)
            if (y % 1024 == 1023)
                ESP_LOGI(TAG, "line %d/%d, %lu bytes", y + 1, job.height, (unsigned long)job.bytes_out);
        }
        bjl_page_end(&job);
        bjl_emit_end_job(&job);
    }
    if (err == ESP_OK && job.err != 0)
        err = ESP_FAIL;

    if (!dry_run) {
        if (err == ESP_OK)
            err = usb_printer_flush();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "all data sent (%lu bytes), waiting for the printer to finish",
                     (unsigned long)job.bytes_out);
            // EndJob response arrives once the page is done; keep the busy LED until then
            if (usb_printer_wait_response("EndJob", 180000) == ESP_ERR_TIMEOUT)
                ESP_LOGW(TAG, "no EndJob response");
        } else if (usb_printer_connected()) {
            ESP_LOGW(TAG, "job aborted, resetting printer");
        }
        usb_printer_job_end();
        if (err != ESP_OK && usb_printer_connected())
            usb_printer_soft_reset();
    }

    stats->bytes = job.bytes_out;
    stats->crc32 = crc;
    stats->ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    testpage_free(&tp);
    bjl_job_free(&job);
    return err;
}
