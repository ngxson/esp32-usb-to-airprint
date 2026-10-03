// USB host driver for a single USB printer-class device (interface class 7)
// on the ESP32-S3 OTG port, plus the scanner function of the same device.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define CANON_VID 0x04a9
#define CANON_MG2500_PID 0x176d

typedef struct {
    uint16_t vid, pid;
    char manufacturer[48];
    char product[48];
    char serial[48];
    char device_id[384];        // IEEE 1284 Device ID, e.g. "MFG:Canon;CMD:...;MDL:MG2500 series;..."
    bool bidirectional;         // has a bulk IN endpoint
} usb_printer_info_t;

// Last status the printer reported on the back channel (IVEC GetStatusResponse)
typedef struct {
    bool valid;
    char state[24];             // "idle", "processing", ...
    char detail[48];            // e.g. "MarkerSupplyAttention"
    char data[240];             // Canon status fields, e.g. "BST:04;...;CIR:CL=000,BK=UK;..."
} usb_printer_status_t;

// Called from the USB client task on attach/detach of a printer-class device.
typedef void (*usb_printer_conn_cb_t)(bool connected, const usb_printer_info_t *info);

esp_err_t usb_printer_start(usb_printer_conn_cb_t cb);
bool usb_printer_connected(void);
bool usb_printer_get_info(usb_printer_info_t *out);
bool usb_printer_get_status(usb_printer_status_t *out);
bool usb_printer_job_active(void);      // a job (AirPrint or test page) holds the printer

// --- job I/O, one job at a time, from a single task -------------------------
esp_err_t usb_printer_job_begin(void);
// Buffered write; blocks while the printer is busy (it NAKs until it has room).
// Fails if the device disappears or the job is cancelled.
esp_err_t usb_printer_write(const uint8_t *data, size_t len);
// Sends any buffered data and waits until the printer has accepted it.
esp_err_t usb_printer_flush(void);
// Waits until the back channel has delivered text containing `needle`.
// ESP_OK = found, ESP_ERR_TIMEOUT = not seen in time, ESP_ERR_NOT_SUPPORTED =
// the printer has no bulk IN endpoint.
esp_err_t usb_printer_wait_response(const char *needle, uint32_t timeout_ms);
void usb_printer_job_end(void);

// Abort a running job from any task: stops pending transfers and makes the
// job's write/flush calls fail with ESP_ERR_INVALID_STATE.
void usb_printer_cancel(void);
// Printer class SOFT_RESET request (what Canon's driver sends on cancel).
esp_err_t usb_printer_soft_reset(void);
// Printer class GET_PORT_STATUS: bit 5 paper empty, bit 4 selected, bit 3 no error.
esp_err_t usb_printer_port_status(uint8_t *status);

// --- scanner: the device's vendor-specific interface (interface 0 on the
// MG2500), Canon's pixma protocol on its bulk endpoints. A scan holds the
// device like a print job: one or the other.
#define USB_SCAN_ERROR (-1)
#define USB_SCAN_TIMEOUT (-2)
#define USB_SCAN_MAX_READ 8192      // = PIXMA_READ_CHUNK

bool usb_scanner_present(void);
bool usb_scanner_active(void);
esp_err_t usb_scanner_begin(void);
// Synchronous bulk transfers: bytes transferred (0 = zero-length packet),
// USB_SCAN_TIMEOUT or USB_SCAN_ERROR. A read returns one USB transfer of up
// to len (<= USB_SCAN_MAX_READ) bytes.
int usb_scanner_write(const uint8_t *data, size_t len, uint32_t timeout_ms);
int usb_scanner_read(uint8_t *buf, size_t len, uint32_t timeout_ms);
void usb_scanner_end(void);
