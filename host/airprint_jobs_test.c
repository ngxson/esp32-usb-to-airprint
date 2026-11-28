// Exercise a pending job across an attached-driver change without sockets or hardware.
#include "../components/airprint/airprint.c"

extern const printer_driver_t canon_mf220_driver;
extern const printer_driver_t canon_mg2500_driver;

static size_t written;
static int closes;
static int capture_write(void *ctx, const uint8_t *data, size_t len)
{
    written += len;
    return 0;
}
static void capture_close(void *ctx, bool ok)
{
    closes++;
}

static conn_t tiny_urf(void)
{
    conn_t c = {.len = 46, .remaining = 46};
    memcpy(c.buf, "UNIRAST\0", 8);
    c.buf[11] = 1; // one page
    c.buf[12] = 8; // gray
    c.buf[27] = 1; // width
    c.buf[31] = 1; // height
    c.buf[34] = 2;
    c.buf[35] = 88; // 600 dpi
    c.buf[45] = 128; // one white line
    return c;
}

int main(void)
{
    airprint_config_t cfg = {.driver = &canon_mf220_driver, .job_write = capture_write, .job_close = capture_close};
    s_cfg = &cfg;
    for (int change = 0; change < 3; change++) {
        job_t pending = {.id = change + 1, .state = JOB_PENDING,
                         .sides = change == 2 ? PRINTER_SIDES_ONE : PRINTER_SIDES_LONG_EDGE};
        s_active = &pending;
        cfg.driver = change ? &canon_mg2500_driver : &canon_mf220_driver;
        written = 0;
        closes = 0;
        conn_t c = tiny_urf();
        int status = print_document(&c, &pending);
        if (s_active || closes != 1 ||
            (change == 1 ? status == IPP_OK || pending.state != JOB_ABORTED || written != 0
                    : status != IPP_OK || pending.state != JOB_COMPLETED || written == 0)) {
            fprintf(stderr, "FAIL pending duplex, changed=%d: status %x, state %d, bytes %zu, closes %d, active %d\n",
                    change, status, pending.state, written, closes, s_active != NULL);
            return 1;
        }
    }
    puts("pending duplex: supported job prints; simplex change aborts before output and releases slot");
    return 0;
}
