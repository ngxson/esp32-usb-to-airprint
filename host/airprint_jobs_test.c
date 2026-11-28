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

// Both fake drivers accept an opaque token, so a dispatch regression is
// reported by counters instead of interpreting another driver's allocation.
static airprint_config_t *switch_cfg;
static printer_driver_t replacement;
static int switch_at, token, calls[6], wrong_calls;

static void check_owner(void *job, int stage)
{
    if (job != &token)
        wrong_calls++;
    calls[stage]++;
    if (switch_at == stage)
        switch_cfg->driver = &replacement;
}

static void *owner_begin(const printer_driver_t *d, const printer_io_t *io, int dpi, int paper, int sides)
{
    check_owner(&token, 0);
    return &token;
}
static int owner_page(void *job, int w, int h, int bpp) { check_owner(job, 1); return 0; }
static int owner_line(void *job, int y, const uint8_t *px) { check_owner(job, 2); return 0; }
static int owner_page_end(void *job) { check_owner(job, 3); return 0; }
static void owner_stats(void *job, char *buf, size_t n) { check_owner(job, 4); buf[0] = 0; }
static int owner_end(void *job) { check_owner(job, 5); return 0; }
static int wrong_page(void *job, int w, int h, int bpp) { wrong_calls++; return 0; }
static int wrong_line(void *job, int y, const uint8_t *px) { wrong_calls++; return 0; }
static int wrong_end(void *job) { wrong_calls++; return 0; }
static void wrong_stats(void *job, char *buf, size_t n) { wrong_calls++; buf[0] = 0; }

static int driver_switch_tests(airprint_config_t *cfg)
{
    printer_driver_t owner = canon_mf220_driver;
    owner.job_begin = owner_begin;
    owner.page_begin = owner_page;
    owner.page_line = owner_line;
    owner.page_end = owner_page_end;
    owner.job_stats = owner_stats;
    owner.job_end = owner_end;
    replacement = owner;
    replacement.duplex = false;
    replacement.resolutions[0] = 300;
    replacement.nresolutions = 1;
    replacement.page_begin = wrong_page;
    replacement.page_line = wrong_line;
    replacement.page_end = replacement.job_end = wrong_end;
    replacement.job_stats = wrong_stats;
    switch_cfg = cfg;
    int failed = 0;
    for (switch_at = 0; switch_at <= 4; switch_at++) {
        memset(calls, 0, sizeof(calls));
        wrong_calls = closes = 0;
        cfg->driver = &owner;
        job_t pending = {.id = 10 + switch_at, .state = JOB_PENDING, .sides = PRINTER_SIDES_LONG_EDGE};
        s_active = &pending;
        conn_t c = tiny_urf();
        c.buf[11] = 2;
        memcpy(c.buf + 46, c.buf + 12, 34);
        c.len = c.remaining = 80;
        int status = print_document(&c, &pending);
        if (status != IPP_OK || pending.state != JOB_COMPLETED || s_active || closes != 1 || wrong_calls ||
            calls[0] != 1 || calls[1] != 2 || calls[2] != 2 || calls[3] != 2 || calls[4] != 1 || calls[5] != 1) {
            fprintf(stderr, "FAIL driver switch at callback %d: status %x, wrong %d, callbacks %d/%d/%d/%d/%d/%d\n",
                    switch_at, status, wrong_calls, calls[0], calls[1], calls[2], calls[3], calls[4], calls[5]);
            failed = 1;
        }
    }
    if (!failed)
        puts("driver changes: all pages, statistics and cleanup use the allocating driver");
    return failed;
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
    return driver_switch_tests(&cfg);
}
