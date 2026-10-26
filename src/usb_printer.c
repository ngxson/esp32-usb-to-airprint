// USB printer class host driver (single device, no hub support).
//
// Tasks:
//   usb_lib     - runs usb_host_lib_handle_events()
//   usb_client  - runs usb_host_client_handle_events(); opens/claims/tears
//                 down the printer and executes all transfer callbacks
// A print job runs in its own task and talks to the printer through
// usb_printer_write()/flush()/wait_response(). A scan uses the device's
// vendor interface through usb_scanner_*(); it holds the device like a job.
#include "usb_printer.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_printer";

#define OUT_XFER_SIZE 16384     // two of these: one filling while one is sent
#define OUT_XFER_COUNT 2
#define IN_XFER_SIZE 512        // multiple of the 64-byte full-speed MPS
#define CTRL_XFER_SIZE (sizeof(usb_setup_packet_t) + 1024)
#define RESP_BUF_SIZE 2048
#define MSG_BUF_SIZE 4096       // reassembly of one XML reply (status is ~1 KB)
#define IN_POLL_IDLE_MS 2000    // back-channel poll interval; every read returns
#define IN_POLL_JOB_MS 250      // a fresh ~1 KB status, so don't read back-to-back

#define SCAN_OUT_SIZE 4224      // largest scanner command: gamma table, 16 + 2056 bytes
#define SCAN_IN_SIZE USB_SCAN_MAX_READ

#define USB_CLASS_PRINTER 0x07
#define USB_CLASS_VENDOR 0xff
#define PRN_REQ_GET_DEVICE_ID 0
#define PRN_REQ_GET_PORT_STATUS 1
#define PRN_REQ_SOFT_RESET 2

typedef struct {
    usb_host_client_handle_t client;
    TaskHandle_t client_task;
    usb_printer_conn_cb_t conn_cb;
    SemaphoreHandle_t lock;             // protects resp buffer and info

    // device (owned by the client task)
    uint8_t pending_addr;
    volatile bool gone;
    volatile bool connected;
    usb_device_handle_t dev;
    uint8_t intf, alt, ep_out, ep_in;
    bool claimed;
    usb_printer_info_t info;

    // transfers
    usb_transfer_t *out[OUT_XFER_COUNT];
    usb_transfer_t *in;
    usb_transfer_t *ctrl;
    QueueHandle_t out_free;             // OUT transfers not in flight
    usb_transfer_t *fill;               // OUT transfer being filled by the job
    atomic_int out_inflight;            // OUT transfers submitted, not completed
    volatile bool in_inflight;
    bool in_stopped;                    // IN endpoint failed, stop polling
    TickType_t in_last;                 // when the last IN read completed
    volatile bool ctrl_done;
    SemaphoreHandle_t ctrl_sem;
    SemaphoreHandle_t ctrl_mutex;

    // scanner interface
    uint8_t scan_intf, scan_ep_out, scan_ep_in;
    uint16_t scan_mps;                  // bulk IN max packet size: reads are multiples of it
    bool scan_claimed;
    volatile bool scan_active;
    bool scan_stuck;                    // a transfer never came back: no more scanning
    usb_transfer_t *scan_out, *scan_in;
    SemaphoreHandle_t scan_out_sem, scan_in_sem;

    // job state
    volatile bool job_active;           // a print job or a scan holds the device
    volatile bool cancel;
    volatile usb_transfer_status_t out_status;  // first failed OUT status in the job

    char resp[RESP_BUF_SIZE];           // non-status replies, one line each
    size_t resp_len;
    char msg[MSG_BUF_SIZE];             // XML reply being reassembled (client task)
    size_t msg_len;
    char last_status[320];              // last logged status summary
    usb_printer_status_t status;        // parsed GetStatusResponse (under lock)
} printer_t;

static printer_t s_p;

// ---------------------------------------------------------------------------
// transfer callbacks (run in the client task)

static void out_cb(usb_transfer_t *xfer)
{
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED && s_p.out_status == USB_TRANSFER_STATUS_COMPLETED) {
        s_p.out_status = xfer->status;
        ESP_LOGW(TAG, "bulk OUT failed, status %d", xfer->status);
    }
    atomic_fetch_sub(&s_p.out_inflight, 1);
    xQueueSend(s_p.out_free, &xfer, 0);
}

static void in_submit(void)
{
    if (s_p.gone || !s_p.ep_in || s_p.in_inflight || s_p.in_stopped)
        return;
    s_p.in->num_bytes = IN_XFER_SIZE;
    s_p.in_inflight = true;
    if (usb_host_transfer_submit(s_p.in) != ESP_OK)
        s_p.in_inflight = false;
}

// copies the text of <tag>...</tag> (first occurrence) into out
static void xml_value(const char *msg, const char *tag, char *out, size_t n)
{
    out[0] = 0;
    const char *p = strstr(msg, tag);
    if (!p)
        return;
    p += strlen(tag);
    size_t i = 0;
    while (p[i] && p[i] != '<' && p[i] != ']' && i < n - 1) {
        out[i] = p[i];
        i++;
    }
    out[i] = 0;
}

static void resp_append(const char *line)
{
    size_t n = strlen(line);
    xSemaphoreTake(s_p.lock, portMAX_DELAY);
    if (s_p.resp_len + n + 1 >= RESP_BUF_SIZE)     // full: drop the history
        s_p.resp_len = 0;
    memcpy(s_p.resp + s_p.resp_len, line, n);
    s_p.resp_len += n;
    s_p.resp[s_p.resp_len++] = '\n';
    s_p.resp[s_p.resp_len] = 0;
    xSemaphoreGive(s_p.lock);
}

// One complete IVEC reply. Status replies are only logged when they change;
// everything else (StartJobResponse, ...) is logged and kept for
// usb_printer_wait_response().
static void handle_reply(const char *msg)
{
    char op[48], resp[24], detail[64];
    xml_value(msg, "<ivec:operation>", op, sizeof(op));
    xml_value(msg, "<ivec:response>", resp, sizeof(resp));
    xml_value(msg, "<ivec:response_detail>", detail, sizeof(detail));
    if (strcmp(op, "GetStatusResponse") == 0) {
        char status[24], sdetail[48], data[240], summary[sizeof(s_p.last_status)];
        xml_value(msg, "<ivec:status>", status, sizeof(status));
        xml_value(msg, "<ivec:status_detail>", sdetail, sizeof(sdetail));
        xml_value(msg, "<![CDATA[", data, sizeof(data));
        snprintf(summary, sizeof(summary), "%s (%s) %s", status, sdetail[0] ? sdetail : "-", data);
        xSemaphoreTake(s_p.lock, portMAX_DELAY);
        s_p.status.valid = true;
        snprintf(s_p.status.state, sizeof(s_p.status.state), "%s", status);
        snprintf(s_p.status.detail, sizeof(s_p.status.detail), "%s", sdetail);
        snprintf(s_p.status.data, sizeof(s_p.status.data), "%s", data);
        xSemaphoreGive(s_p.lock);
        if (strcmp(summary, s_p.last_status) != 0) {
            strcpy(s_p.last_status, summary);
            ESP_LOGI(TAG, "printer status: %s", summary);
        }
        return;
    }
    char line[160];
    snprintf(line, sizeof(line), "%s %s %s", op[0] ? op : "?", resp, detail);
    ESP_LOGI(TAG, "printer reply: %s", line);
    resp_append(line);
}

static void in_cb(usb_transfer_t *xfer)
{
    s_p.in_inflight = false;
    s_p.in_last = xTaskGetTickCount();
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
        size_t n = xfer->actual_num_bytes;
        if (s_p.msg_len + n >= MSG_BUF_SIZE) {
            ESP_LOGW(TAG, "back-channel message too long, dropped");
            s_p.msg_len = 0;
        }
        memcpy(s_p.msg + s_p.msg_len, xfer->data_buffer, n);
        s_p.msg_len += n;
        s_p.msg[s_p.msg_len] = 0;
        char *end;
        while ((end = strstr(s_p.msg, "</cmd>")) != NULL) {
            end += strlen("</cmd>");
            char saved = *end;
            *end = 0;
            handle_reply(s_p.msg);
            *end = saved;
            size_t used = end - s_p.msg;
            memmove(s_p.msg, end, s_p.msg_len - used + 1);
            s_p.msg_len -= used;
        }
        // skip whitespace between replies
        while (s_p.msg_len && (s_p.msg[0] == '\r' || s_p.msg[0] == '\n' || s_p.msg[0] == ' ')) {
            memmove(s_p.msg, s_p.msg + 1, s_p.msg_len--);
        }
        if (s_p.msg_len)
            in_submit();        // rest of a reply is still pending
    } else if (xfer->status != USB_TRANSFER_STATUS_COMPLETED && xfer->status != USB_TRANSFER_STATUS_CANCELED &&
               xfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_LOGW(TAG, "bulk IN failed (status %d), back channel disabled", xfer->status);
        s_p.in_stopped = true;
    }
}

static void ctrl_cb(usb_transfer_t *xfer)
{
    s_p.ctrl_done = true;
    xSemaphoreGive(s_p.ctrl_sem);
}

// Synchronous control transfer. Callable from any task: from the client task
// it pumps client events itself, since that is where the callback runs.
static esp_err_t ctrl_request(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                              uint16_t wLength, uint8_t *data, size_t *actual)
{
    if (!s_p.dev || s_p.gone)
        return ESP_ERR_INVALID_STATE;
    if (wLength > CTRL_XFER_SIZE - sizeof(usb_setup_packet_t))
        return ESP_ERR_INVALID_SIZE;
    xSemaphoreTake(s_p.ctrl_mutex, portMAX_DELAY);
    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_p.ctrl->data_buffer;
    setup->bmRequestType = bmRequestType;
    setup->bRequest = bRequest;
    setup->wValue = wValue;
    setup->wIndex = wIndex;
    setup->wLength = wLength;
    if (!(bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN) && wLength && data)
        memcpy(s_p.ctrl->data_buffer + sizeof(usb_setup_packet_t), data, wLength);
    s_p.ctrl->num_bytes = sizeof(usb_setup_packet_t) + wLength;
    s_p.ctrl->device_handle = s_p.dev;
    s_p.ctrl->bEndpointAddress = 0;
    s_p.ctrl_done = false;
    xSemaphoreTake(s_p.ctrl_sem, 0);

    esp_err_t err = usb_host_transfer_submit_control(s_p.client, s_p.ctrl);
    if (err == ESP_OK) {
        bool in_client = xTaskGetCurrentTaskHandle() == s_p.client_task;
        TickType_t start = xTaskGetTickCount();
        while (!s_p.ctrl_done && xTaskGetTickCount() - start < pdMS_TO_TICKS(2000)) {
            if (in_client)
                usb_host_client_handle_events(s_p.client, pdMS_TO_TICKS(10));
            else
                xSemaphoreTake(s_p.ctrl_sem, pdMS_TO_TICKS(100));
        }
        if (!s_p.ctrl_done)
            err = ESP_ERR_TIMEOUT;
        else if (s_p.ctrl->status != USB_TRANSFER_STATUS_COMPLETED)
            err = ESP_FAIL;
    }
    if (err == ESP_OK && (bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN) && data) {
        size_t n = s_p.ctrl->actual_num_bytes - sizeof(usb_setup_packet_t);
        if (n > wLength)
            n = wLength;
        memcpy(data, s_p.ctrl->data_buffer + sizeof(usb_setup_packet_t), n);
        if (actual)
            *actual = n;
    }
    xSemaphoreGive(s_p.ctrl_mutex);
    return err;
}

// ---------------------------------------------------------------------------
// attach / detach (client task)

static void str_desc_ascii(const usb_str_desc_t *d, char *out, size_t out_len)
{
    size_t j = 0;
    if (d && d->bLength >= 2) {
        for (size_t i = 0; i < (size_t)(d->bLength - 2) / 2 && j < out_len - 1; i++) {
            uint16_t ch = d->wData[i];
            out[j++] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
        }
    }
    out[j] = 0;
}

static void free_scan_transfers(void)
{
    if (s_p.scan_out)
        usb_host_transfer_free(s_p.scan_out);
    if (s_p.scan_in)
        usb_host_transfer_free(s_p.scan_in);
    s_p.scan_out = s_p.scan_in = NULL;
}

static void release_device(void)
{
    if (s_p.claimed)
        usb_host_interface_release(s_p.client, s_p.dev, s_p.intf);
    s_p.claimed = false;
    if (!s_p.scan_stuck)
        free_scan_transfers();
    if (s_p.scan_claimed)
        usb_host_interface_release(s_p.client, s_p.dev, s_p.scan_intf);
    s_p.scan_claimed = false;
    s_p.scan_ep_in = s_p.scan_ep_out = 0;
    for (int i = 0; i < OUT_XFER_COUNT; i++) {
        if (s_p.out[i])
            usb_host_transfer_free(s_p.out[i]);
        s_p.out[i] = NULL;
    }
    if (s_p.in)
        usb_host_transfer_free(s_p.in);
    if (s_p.ctrl)
        usb_host_transfer_free(s_p.ctrl);
    s_p.in = s_p.ctrl = NULL;
    xQueueReset(s_p.out_free);
    if (s_p.dev)
        usb_host_device_close(s_p.client, s_p.dev);
    s_p.dev = NULL;
    s_p.ep_in = s_p.ep_out = 0;
}

// Ranks printer-class protocols: 2 = bidirectional (preferred), 1 =
// unidirectional, 3 = IEEE 1284.4 (packetized, we only use it as a plain
// OUT pipe).
static int proto_rank(int proto)
{
    return proto == 2 ? 3 : proto == 1 ? 2 : proto == 3 ? 1 : 0;
}

// Finds the printer-class interface, preferring the bidirectional protocol.
static bool find_printer_interface(const usb_config_desc_t *cfg)
{
    int offset = 0, best_rank = -1;
    uint8_t cur_intf = 0, cur_alt = 0, cur_out = 0, cur_in = 0;
    bool in_printer = false;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;

    // commits the interface collected so far if it beats the current best
    #define COMMIT()                                                                        \
        do {                                                                                \
            if (in_printer && cur_out && proto_rank(cur_proto) > best_rank) {               \
                best_rank = proto_rank(cur_proto);                                          \
                s_p.intf = cur_intf;                                                        \
                s_p.alt = cur_alt;                                                          \
                s_p.ep_out = cur_out;                                                       \
                s_p.ep_in = cur_proto == 2 ? cur_in : 0;                                    \
            }                                                                               \
        } while (0)

    int cur_proto = 0;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != NULL) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            COMMIT();
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)d;
            in_printer = intf->bInterfaceClass == USB_CLASS_PRINTER && intf->bInterfaceSubClass == 1;
            cur_intf = intf->bInterfaceNumber;
            cur_alt = intf->bAlternateSetting;
            cur_proto = intf->bInterfaceProtocol;
            cur_out = cur_in = 0;
        } else if (in_printer && d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)d;
            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK) {
                if (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK)
                    cur_in = ep->bEndpointAddress;
                else
                    cur_out = ep->bEndpointAddress;
            }
        }
    }
    COMMIT();
    #undef COMMIT
    return best_rank >= 0;
}

// Logs every interface and endpoint (useful when a new device shows up).
static void log_interfaces(const usb_config_desc_t *cfg)
{
    int offset = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != NULL) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
            ESP_LOGI(TAG, "  interface %d alt %d: class %02x/%02x/%02x", i->bInterfaceNumber, i->bAlternateSetting,
                     i->bInterfaceClass, i->bInterfaceSubClass, i->bInterfaceProtocol);
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)d;
            static const char *const types[] = {"control", "isoc", "bulk", "interrupt"};
            ESP_LOGI(TAG, "    endpoint 0x%02x %s, max packet %d", ep->bEndpointAddress,
                     types[ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK], ep->wMaxPacketSize & 0x7ff);
        }
    }
}

// The scanner function: a vendor-specific interface with a bulk IN and a
// bulk OUT endpoint (SANE's pixma backend uses interface 0 of the MG2500).
static bool find_scanner_interface(const usb_config_desc_t *cfg)
{
    int offset = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    bool cand = false, found = false;
    uint8_t intf = 0, in = 0, out = 0;
    uint16_t mps = 0;
    for (;;) {
        d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset);
        if (!d || d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            if (cand && in && out && !found) {
                found = true;
                s_p.scan_intf = intf;
                s_p.scan_ep_in = in;
                s_p.scan_ep_out = out;
                s_p.scan_mps = mps;
            }
            if (!d)
                break;
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
            cand = i->bInterfaceClass == USB_CLASS_VENDOR && i->bAlternateSetting == 0 &&
                   i->bInterfaceNumber != s_p.intf;
            intf = i->bInterfaceNumber;
            in = out = 0;
        } else if (cand && d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)d;
            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_BULK)
                continue;
            if (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
                in = ep->bEndpointAddress;
                mps = ep->wMaxPacketSize & 0x7ff;
            } else {
                out = ep->bEndpointAddress;
            }
        }
    }
    return found && s_p.scan_mps > 0;
}

static void open_device(uint8_t addr)
{
    if (s_p.dev) {
        ESP_LOGW(TAG, "ignoring device %d: a printer is already attached", addr);
        return;
    }
    ESP_LOGI(TAG, "new USB device at address %d", addr);
    if (usb_host_device_open(s_p.client, addr, &s_p.dev) != ESP_OK) {
        s_p.dev = NULL;
        return;
    }
    const usb_device_desc_t *dd;
    const usb_config_desc_t *cfg;
    usb_device_info_t di;
    usb_host_get_device_descriptor(s_p.dev, &dd);
    usb_host_get_active_config_descriptor(s_p.dev, &cfg);
    usb_host_device_info(s_p.dev, &di);

    usb_printer_info_t info = {.vid = dd->idVendor, .pid = dd->idProduct};
    str_desc_ascii(di.str_desc_manufacturer, info.manufacturer, sizeof(info.manufacturer));
    str_desc_ascii(di.str_desc_product, info.product, sizeof(info.product));
    str_desc_ascii(di.str_desc_serial_num, info.serial, sizeof(info.serial));
    ESP_LOGI(TAG, "%04x:%04x \"%s\" \"%s\" serial \"%s\"", info.vid, info.pid, info.manufacturer, info.product,
             info.serial);

    if (!find_printer_interface(cfg)) {
        ESP_LOGW(TAG, "not a printer (no printer-class interface with a bulk OUT endpoint), ignoring");
        usb_host_device_close(s_p.client, s_p.dev);
        s_p.dev = NULL;
        return;
    }
    info.bidirectional = s_p.ep_in != 0;
    log_interfaces(cfg);
    ESP_LOGI(TAG, "printer interface %d alt %d, bulk OUT 0x%02x, bulk IN 0x%02x", s_p.intf, s_p.alt, s_p.ep_out,
             s_p.ep_in);

    esp_err_t err = usb_host_transfer_alloc(CTRL_XFER_SIZE, 0, &s_p.ctrl);
    for (int i = 0; i < OUT_XFER_COUNT && err == ESP_OK; i++)
        err = usb_host_transfer_alloc(OUT_XFER_SIZE, 0, &s_p.out[i]);
    if (err == ESP_OK)
        err = usb_host_transfer_alloc(IN_XFER_SIZE, 0, &s_p.in);
    if (err == ESP_OK)
        err = usb_host_interface_claim(s_p.client, s_p.dev, s_p.intf, s_p.alt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup failed: %s", esp_err_to_name(err));
        release_device();
        return;
    }
    s_p.claimed = true;
    s_p.scan_stuck = false;
    if (find_scanner_interface(cfg)) {
        err = usb_host_interface_claim(s_p.client, s_p.dev, s_p.scan_intf, 0);
        s_p.scan_claimed = err == ESP_OK;
        if (s_p.scan_claimed)
            ESP_LOGI(TAG, "scanner interface %d, bulk OUT 0x%02x, bulk IN 0x%02x (max packet %d)", s_p.scan_intf,
                     s_p.scan_ep_out, s_p.scan_ep_in, s_p.scan_mps);
        else
            ESP_LOGW(TAG, "cannot claim scanner interface %d: %s", s_p.scan_intf, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "no scanner interface");
    }
    s_p.ctrl->callback = ctrl_cb;
    for (int i = 0; i < OUT_XFER_COUNT; i++) {
        s_p.out[i]->device_handle = s_p.dev;
        s_p.out[i]->bEndpointAddress = s_p.ep_out;
        s_p.out[i]->callback = out_cb;
        xQueueSend(s_p.out_free, &s_p.out[i], 0);
    }
    s_p.in->device_handle = s_p.dev;
    s_p.in->bEndpointAddress = s_p.ep_in;
    s_p.in->callback = in_cb;

    if (s_p.alt != 0) {
        uint8_t req = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                      USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
        if (ctrl_request(req, USB_B_REQUEST_SET_INTERFACE, s_p.alt, s_p.intf, 0, NULL, NULL) != ESP_OK)
            ESP_LOGW(TAG, "SET_INTERFACE alt %d failed", s_p.alt);
    }

    // IEEE 1284 Device ID: 2-byte big-endian length (including itself) + text
    static uint8_t idbuf[1024];
    size_t got = 0;
    uint8_t req = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    if (ctrl_request(req, PRN_REQ_GET_DEVICE_ID, cfg->bConfigurationValue - 1, (s_p.intf << 8) | s_p.alt,
                     sizeof(idbuf) - 1, idbuf, &got) == ESP_OK && got > 2) {
        size_t len = ((size_t)idbuf[0] << 8 | idbuf[1]);
        if (len < 2 || len > got)
            len = got;
        len -= 2;
        if (len >= sizeof(info.device_id))
            len = sizeof(info.device_id) - 1;
        memcpy(info.device_id, idbuf + 2, len);
        info.device_id[len] = 0;
        ESP_LOGI(TAG, "IEEE 1284 ID: %s", info.device_id);
    } else {
        ESP_LOGW(TAG, "GET_DEVICE_ID failed");
    }
    uint8_t st;
    if (usb_printer_port_status(&st) == ESP_OK)
        ESP_LOGI(TAG, "port status 0x%02x (%s%s%s)", st, (st & 0x20) ? "paper empty " : "",
                 (st & 0x10) ? "selected " : "", (st & 0x08) ? "no error" : "ERROR");

    if (info.vid != CANON_VID)
        ESP_LOGW(TAG, "not a Canon printer: Canon raster jobs will print garbage on it");

    xSemaphoreTake(s_p.lock, portMAX_DELAY);
    s_p.info = info;
    s_p.resp_len = 0;
    s_p.resp[0] = 0;
    xSemaphoreGive(s_p.lock);
    s_p.msg_len = 0;
    s_p.last_status[0] = 0;
    memset(&s_p.status, 0, sizeof(s_p.status));
    s_p.in_stopped = false;
    s_p.connected = true;
    in_submit();
    if (s_p.conn_cb)
        s_p.conn_cb(true, &info);
}

static void abort_endpoint(uint8_t ep)
{
    if (!ep || !s_p.dev)
        return;
    usb_host_endpoint_halt(s_p.dev, ep);
    usb_host_endpoint_flush(s_p.dev, ep);
    usb_host_endpoint_clear(s_p.dev, ep);
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
        s_p.pending_addr = msg->new_dev.address;
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE && msg->dev_gone.dev_hdl == s_p.dev)
        s_p.gone = true;
}

static void client_task(void *arg)
{
    bool teardown = false;
    while (1) {
        usb_host_client_handle_events(s_p.client, pdMS_TO_TICKS(50));
        if (s_p.pending_addr && !teardown) {   // a re-plug waits for the old teardown
            uint8_t addr = s_p.pending_addr;
            s_p.pending_addr = 0;
            open_device(addr);
        }
        if (s_p.connected && !s_p.gone && !s_p.in_inflight && !s_p.scan_active &&
            xTaskGetTickCount() - s_p.in_last >= pdMS_TO_TICKS(s_p.job_active ? IN_POLL_JOB_MS : IN_POLL_IDLE_MS))
            in_submit();
        if (s_p.gone && !teardown) {
            ESP_LOGW(TAG, "printer disconnected");
            teardown = true;
            s_p.connected = false;
            s_p.cancel = true;
            if (s_p.conn_cb)
                s_p.conn_cb(false, NULL);
        }
        // free everything once the job and all transfers are finished
        if (teardown && !s_p.job_active) {
            if (atomic_load(&s_p.out_inflight) > 0)
                abort_endpoint(s_p.ep_out);
            if (s_p.in_inflight)
                abort_endpoint(s_p.ep_in);
            if (atomic_load(&s_p.out_inflight) == 0 && !s_p.in_inflight) {
                release_device();
                s_p.gone = false;
                s_p.cancel = false;
                teardown = false;
                ESP_LOGI(TAG, "printer resources released");
            }
        }
    }
}

static void lib_task(void *arg)
{
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
    }
}

// ---------------------------------------------------------------------------
// public API

esp_err_t usb_printer_start(usb_printer_conn_cb_t cb)
{
    s_p.conn_cb = cb;
    s_p.lock = xSemaphoreCreateMutex();
    s_p.ctrl_mutex = xSemaphoreCreateMutex();
    s_p.ctrl_sem = xSemaphoreCreateBinary();
    s_p.out_free = xQueueCreate(OUT_XFER_COUNT, sizeof(usb_transfer_t *));
    s_p.scan_out_sem = xSemaphoreCreateBinary();
    s_p.scan_in_sem = xSemaphoreCreateBinary();
    if (!s_p.lock || !s_p.ctrl_mutex || !s_p.ctrl_sem || !s_p.out_free || !s_p.scan_out_sem || !s_p.scan_in_sem)
        return ESP_ERR_NO_MEM;

    const usb_host_config_t host_cfg = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
    ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install");
    if (xTaskCreate(lib_task, "usb_lib", 4096, NULL, 6, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;

    const usb_host_client_config_t client_cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {.client_event_callback = client_event_cb, .callback_arg = NULL},
    };
    ESP_RETURN_ON_ERROR(usb_host_client_register(&client_cfg, &s_p.client), TAG, "client register");
    if (xTaskCreate(client_task, "usb_client", 6144, NULL, 5, &s_p.client_task) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "USB host started, waiting for a printer");
    return ESP_OK;
}

bool usb_printer_connected(void)
{
    return s_p.connected && !s_p.gone;
}

bool usb_printer_job_active(void)
{
    return s_p.job_active;
}

bool usb_printer_get_status(usb_printer_status_t *out)
{
    if (!usb_printer_connected())
        return false;
    xSemaphoreTake(s_p.lock, portMAX_DELAY);
    *out = s_p.status;
    xSemaphoreGive(s_p.lock);
    return out->valid;
}

bool usb_printer_get_info(usb_printer_info_t *out)
{
    if (!usb_printer_connected())
        return false;
    xSemaphoreTake(s_p.lock, portMAX_DELAY);
    *out = s_p.info;
    xSemaphoreGive(s_p.lock);
    return true;
}

// Claims the device for a print job or a scan.
static bool claim_device(void)
{
    xSemaphoreTake(s_p.lock, portMAX_DELAY);
    bool ok = !s_p.job_active;
    s_p.job_active = true;
    xSemaphoreGive(s_p.lock);
    return ok;
}

esp_err_t usb_printer_job_begin(void)
{
    if (!usb_printer_connected())
        return ESP_ERR_INVALID_STATE;
    if (!claim_device())
        return ESP_ERR_INVALID_STATE;
    // transfers cancelled by a previous job may still be returning
    for (int i = 0; i < 100 && uxQueueMessagesWaiting(s_p.out_free) < OUT_XFER_COUNT; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (uxQueueMessagesWaiting(s_p.out_free) < OUT_XFER_COUNT) {
        s_p.job_active = false;
        return ESP_ERR_INVALID_STATE;
    }
    s_p.cancel = false;
    s_p.out_status = USB_TRANSFER_STATUS_COMPLETED;
    s_p.fill = NULL;
    xSemaphoreTake(s_p.lock, portMAX_DELAY);
    s_p.resp_len = 0;
    s_p.resp[0] = 0;
    xSemaphoreGive(s_p.lock);
    s_p.job_active = true;
    return ESP_OK;
}

static esp_err_t job_check(void)
{
    if (s_p.gone || !s_p.connected)
        return ESP_ERR_INVALID_STATE;
    if (s_p.cancel)
        return ESP_ERR_INVALID_STATE;
    if (s_p.out_status != USB_TRANSFER_STATUS_COMPLETED)
        return ESP_FAIL;
    return ESP_OK;
}

static void log_stall(TickType_t since)
{
    uint32_t secs = (xTaskGetTickCount() - since) / configTICK_RATE_HZ;
    uint8_t st = 0;
    if (usb_printer_port_status(&st) == ESP_OK)
        ESP_LOGW(TAG, "printer not accepting data for %lus, port status 0x%02x%s", (unsigned long)secs, st,
                 (st & 0x20) ? " (PAPER EMPTY?)" : "");
    else
        ESP_LOGW(TAG, "printer not accepting data for %lus", (unsigned long)secs);
}

// Waits for a free OUT transfer. The printer NAKs while its buffer is full,
// which can last as long as it takes to print (or until paper is loaded).
static esp_err_t get_fill_buffer(void)
{
    TickType_t since = xTaskGetTickCount(), last_log = since;
    while (!s_p.fill) {
        ESP_RETURN_ON_ERROR(job_check(), TAG, "job aborted");
        if (xQueueReceive(s_p.out_free, &s_p.fill, pdMS_TO_TICKS(200)) == pdTRUE) {
            s_p.fill->num_bytes = 0;
            break;
        }
        if (xTaskGetTickCount() - last_log > pdMS_TO_TICKS(15000)) {
            last_log = xTaskGetTickCount();
            log_stall(since);
        }
    }
    return ESP_OK;
}

static esp_err_t submit_fill(void)
{
    usb_transfer_t *x = s_p.fill;
    s_p.fill = NULL;
    if (x->num_bytes == 0) {
        xQueueSend(s_p.out_free, &x, 0);
        return ESP_OK;
    }
    atomic_fetch_add(&s_p.out_inflight, 1);
    esp_err_t err = usb_host_transfer_submit(x);
    if (err != ESP_OK) {
        atomic_fetch_sub(&s_p.out_inflight, 1);
        xQueueSend(s_p.out_free, &x, 0);
    }
    return err;
}

esp_err_t usb_printer_write(const uint8_t *data, size_t len)
{
    while (len > 0) {
        ESP_RETURN_ON_ERROR(get_fill_buffer(), TAG, "no buffer");
        size_t room = OUT_XFER_SIZE - s_p.fill->num_bytes;
        size_t n = len < room ? len : room;
        memcpy(s_p.fill->data_buffer + s_p.fill->num_bytes, data, n);
        s_p.fill->num_bytes += n;
        data += n;
        len -= n;
        if (s_p.fill->num_bytes == OUT_XFER_SIZE)
            ESP_RETURN_ON_ERROR(submit_fill(), TAG, "submit");
    }
    return ESP_OK;
}

esp_err_t usb_printer_flush(void)
{
    if (s_p.fill)
        ESP_RETURN_ON_ERROR(submit_fill(), TAG, "submit");
    TickType_t since = xTaskGetTickCount(), last_log = since;
    while (uxQueueMessagesWaiting(s_p.out_free) < OUT_XFER_COUNT) {
        ESP_RETURN_ON_ERROR(job_check(), TAG, "job aborted");
        vTaskDelay(pdMS_TO_TICKS(10));
        if (xTaskGetTickCount() - last_log > pdMS_TO_TICKS(15000)) {
            last_log = xTaskGetTickCount();
            log_stall(since);
        }
    }
    return job_check();
}

esp_err_t usb_printer_wait_response(const char *needle, uint32_t timeout_ms)
{
    if (!s_p.ep_in)
        return ESP_ERR_NOT_SUPPORTED;   // unidirectional printer: nothing to wait for
    TickType_t start = xTaskGetTickCount();
    do {
        xSemaphoreTake(s_p.lock, portMAX_DELAY);
        bool found = strstr(s_p.resp, needle) != NULL;
        xSemaphoreGive(s_p.lock);
        if (found)
            return ESP_OK;
        ESP_RETURN_ON_ERROR(job_check(), TAG, "job aborted");
        vTaskDelay(pdMS_TO_TICKS(50));
    } while (xTaskGetTickCount() - start < pdMS_TO_TICKS(timeout_ms));
    return ESP_ERR_TIMEOUT;
}

void usb_printer_job_end(void)
{
    if (s_p.fill) {
        usb_transfer_t *x = s_p.fill;
        s_p.fill = NULL;
        xQueueSend(s_p.out_free, &x, 0);
    }
    // transfers still in flight after a cancel are reaped by abort_endpoint
    if (s_p.cancel && atomic_load(&s_p.out_inflight) > 0 && !s_p.gone)
        abort_endpoint(s_p.ep_out);
    s_p.job_active = false;
}

void usb_printer_cancel(void)
{
    if (!s_p.job_active)
        return;
    ESP_LOGW(TAG, "cancelling job");
    s_p.cancel = true;
}

esp_err_t usb_printer_soft_reset(void)
{
    uint8_t req = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    return ctrl_request(req, PRN_REQ_SOFT_RESET, 0, s_p.intf, 0, NULL, NULL);
}

esp_err_t usb_printer_port_status(uint8_t *status)
{
    uint8_t req = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    size_t got = 0;
    esp_err_t err = ctrl_request(req, PRN_REQ_GET_PORT_STATUS, 0, s_p.intf, 1, status, &got);
    return (err == ESP_OK && got != 1) ? ESP_FAIL : err;
}

// ---------------------------------------------------------------------------
// scanner

static void scan_cb(usb_transfer_t *xfer)
{
    xSemaphoreGive((SemaphoreHandle_t)xfer->context);
}

bool usb_scanner_present(void)
{
    return usb_printer_connected() && s_p.scan_claimed && !s_p.scan_stuck;
}

bool usb_scanner_active(void)
{
    return s_p.scan_active;
}

esp_err_t usb_scanner_begin(void)
{
    if (!usb_scanner_present())
        return ESP_ERR_NOT_FOUND;
    if (!claim_device())
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = usb_host_transfer_alloc(SCAN_OUT_SIZE, 0, &s_p.scan_out);
    if (err == ESP_OK)
        err = usb_host_transfer_alloc(SCAN_IN_SIZE, 0, &s_p.scan_in);
    if (err != ESP_OK) {
        free_scan_transfers();
        s_p.job_active = false;
        return err;
    }
    s_p.scan_out->device_handle = s_p.scan_in->device_handle = s_p.dev;
    s_p.scan_out->bEndpointAddress = s_p.scan_ep_out;
    s_p.scan_in->bEndpointAddress = s_p.scan_ep_in;
    s_p.scan_out->callback = s_p.scan_in->callback = scan_cb;
    s_p.scan_out->context = s_p.scan_out_sem;
    s_p.scan_in->context = s_p.scan_in_sem;
    s_p.scan_active = true;
    return ESP_OK;
}

// Runs one transfer; bytes transferred or USB_SCAN_TIMEOUT / USB_SCAN_ERROR.
static int scan_xfer(usb_transfer_t *x, size_t num_bytes, uint32_t timeout_ms)
{
    if (!s_p.scan_active || s_p.gone || s_p.scan_stuck)
        return USB_SCAN_ERROR;
    SemaphoreHandle_t sem = x->context;
    xSemaphoreTake(sem, 0);
    x->num_bytes = (int)num_bytes;
    if (usb_host_transfer_submit(x) != ESP_OK)
        return USB_SCAN_ERROR;
    bool timed_out = xSemaphoreTake(sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE;
    if (timed_out) {
        // stop it: the callback still comes, with a cancelled status
        abort_endpoint(x->bEndpointAddress);
        if (xSemaphoreTake(sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "scanner transfer on 0x%02x did not come back", x->bEndpointAddress);
            s_p.scan_stuck = true;
            return USB_SCAN_ERROR;
        }
    }
    if (x->status == USB_TRANSFER_STATUS_COMPLETED)
        return x->actual_num_bytes;     // also when it completed just as it timed out
    if (timed_out)
        return USB_SCAN_TIMEOUT;
    ESP_LOGW(TAG, "scanner transfer on 0x%02x failed, status %d", x->bEndpointAddress, x->status);
    return USB_SCAN_ERROR;
}

int usb_scanner_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (len > SCAN_OUT_SIZE || !s_p.scan_out)
        return USB_SCAN_ERROR;
    memcpy(s_p.scan_out->data_buffer, data, len);
    return scan_xfer(s_p.scan_out, len, timeout_ms);
}

int usb_scanner_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    if (!s_p.scan_in)
        return USB_SCAN_ERROR;
    if (len > SCAN_IN_SIZE)
        len = SCAN_IN_SIZE;
    // IN transfers must be a multiple of the max packet size
    size_t n = (len + s_p.scan_mps - 1) / s_p.scan_mps * s_p.scan_mps;
    if (n > SCAN_IN_SIZE)
        n = SCAN_IN_SIZE / s_p.scan_mps * s_p.scan_mps;
    int r = scan_xfer(s_p.scan_in, n, timeout_ms);
    if (r > (int)len) {
        ESP_LOGW(TAG, "scanner sent %d bytes, %u expected: rest dropped", r, (unsigned)len);
        r = (int)len;
    }
    if (r > 0)
        memcpy(buf, s_p.scan_in->data_buffer, (size_t)r);
    return r;
}

void usb_scanner_end(void)
{
    if (!s_p.scan_active)
        return;
    s_p.scan_active = false;
    if (!s_p.scan_stuck)
        free_scan_transfers();
    s_p.in_last = xTaskGetTickCount();     // resume the printer's status polling later
    s_p.job_active = false;
}
