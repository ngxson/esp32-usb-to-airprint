// Canon PIXMA MG2500 series: BJ raster in IVEC XML (components/canon_bjl),
// with its status from the IVEC GetStatusResponse back channel.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bjl_render.h"
#include "canon_bjl.h"
#include "printer_driver.h"
#include "testpage.h"

typedef struct {
    printer_io_t io;
    bjl_job_t bjl;
    bjl_render_t render;
} canon_job_t;

static int sink(void *user, const uint8_t *data, size_t len)
{
    const printer_io_t *io = user;
    return io->write(io->ctx, data, len) < 0 ? -1 : 0;
}

// Canon's driver waits for each IVEC reply (Gutenprint never does): the
// platform waits a while and carries on without one.
static int sync_io(const printer_io_t *io, const char *what)
{
    return io->sync ? io->sync(io->ctx, what) : 0;
}

static bjl_mode_id_t mode_for(int dpi)
{
    return dpi == 300 ? BJL_MODE_300DPI : BJL_MODE_600DPI;
}

// the job header: StartJob and ModeShift, each followed by the handshake
static void start_job(bjl_job_t *bjl, const printer_io_t *io)
{
    bjl_emit_start_job(bjl);
    if (!bjl->err && sync_io(io, "StartJob") < 0)
        bjl->err = -1;
    if (!bjl->err)
        bjl_emit_mode_shift(bjl);
    if (!bjl->err && sync_io(io, "ModeShift") < 0)
        bjl->err = -1;
}

static void *job_begin(const printer_io_t *io, int dpi, int paper)
{
    canon_job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->io = *io;
    if (bjl_job_init(&j->bjl, mode_for(dpi), (bjl_paper_id_t)paper, sink, &j->io)) {
        free(j);
        return NULL;
    }
    if (bjl_render_init(&j->render, &j->bjl)) {
        bjl_job_free(&j->bjl);
        free(j);
        return NULL;
    }
    uint64_t prof[4];
    bjl_render_profile(prof, true);
    start_job(&j->bjl, &j->io);
    return j;
}

static int page_begin(void *job, int src_w, int src_h, int bytes_pp)
{
    canon_job_t *j = job;
    bjl_page_begin(&j->bjl);
    bjl_render_page(&j->render, src_w, src_h, bytes_pp);
    return j->bjl.err ? -1 : 0;
}

static int page_line(void *job, int y, const uint8_t *px)
{
    canon_job_t *j = job;
    return bjl_render_line(&j->render, y, px) ? -1 : 0;
}

static int page_end(void *job)
{
    canon_job_t *j = job;
    return bjl_page_end(&j->bjl) ? -1 : 0;
}

static int job_end(void *job)
{
    canon_job_t *j = job;
    // whatever happened (cancel, bad data, client gone): eject a partly
    // printed sheet and close the Canon job
    if (j->bjl.in_page && !j->bjl.err)
        bjl_page_end(&j->bjl);
    if (!j->bjl.err)
        bjl_emit_end_job(&j->bjl);
    if (!j->bjl.err)
        sync_io(&j->io, "EndJob");     // answered once the page is out
    int ret = j->bjl.err ? -1 : 0;
    bjl_render_free(&j->render);
    bjl_job_free(&j->bjl);
    free(j);
    return ret;
}

static void job_stats(void *job, char *buf, size_t n)
{
    uint64_t prof[4];
    bjl_render_profile(prof, false);
#ifdef ESP_PLATFORM
    snprintf(buf, n, "render cycles: convert %.2f s, dither %.2f s, compress+emit %.2f s over %llu inked lines",
             prof[0] / 240e6, prof[1] / 240e6, prof[2] / 240e6, (unsigned long long)prof[3]);
#else
    buf[0] = 0;
#endif
}

static int test_page(const printer_io_t *io, int dpi, const char *label)
{
    bjl_job_t job;
    testpage_t tp;
    if (bjl_job_init(&job, mode_for(dpi), BJL_PAPER_A4, sink, (void *)io) != 0)
        return -1;
    char info1[64];
    testpage_format_mode(&job, info1, sizeof(info1));
    if (testpage_init(&tp, &job, info1, label) != 0) {
        bjl_job_free(&job);
        return -1;
    }
    start_job(&job, io);
    if (!job.err && bjl_page_begin(&job) == 0) {
        for (int y = 0; y < job.height; y++) {
            if (bjl_page_add_line(&job, testpage_render_line(&tp, y)) != 0)
                break;
            if ((y & 63) == 63 && io->progress)
                io->progress(io->ctx, y + 1, job.height);
        }
        bjl_page_end(&job);
        bjl_emit_end_job(&job);
    }
    if (!job.err)
        sync_io(io, "EndJob");
    int ret = job.err ? -1 : 0;
    testpage_free(&tp);
    bjl_job_free(&job);
    return ret;
}

// ---------------------------------------------------------------------------
// status: the IVEC GetStatusResponse fields

static void add_reason(airprint_status_t *st, const char *reason)
{
    if (st->nreasons < (int)(sizeof(st->reasons) / sizeof(st->reasons[0])))
        st->reasons[st->nreasons++] = reason;
}

// "CIR:CL=000,BK=UK" -> remaining ink in percent, -2 = unknown
static int ink_level(const char *data, const char *key)
{
    const char *cir = strstr(data, "CIR:");
    if (!cir)
        return -2;
    const char *p = strstr(cir, key);
    const char *end = strchr(cir, ';');
    if (!p || (end && p > end))
        return -2;
    p += strlen(key);
    if (*p < '0' || *p > '9')
        return -2;      // "UK": unknown (e.g. refilled cartridge)
    int v = atoi(p);
    return v > 100 ? 100 : v;
}

static void status(const char *state, const char *detail, const char *data, airprint_status_t *st)
{
    const char *d = detail ? detail : "";
    if (strstr(d, "MarkerSupplyEmpty") || strstr(d, "MarkerSupplyMissing")) {
        add_reason(st, "marker-supply-empty-error");
        snprintf(st->message, sizeof(st->message), "Ink cartridge empty or missing");
    } else if (strstr(d, "MarkerSupplyAttention") || strstr(d, "MarkerSupplyLow")) {
        add_reason(st, "marker-supply-low-warning");
        snprintf(st->message, sizeof(st->message), "Ink low");
    }
    if (strstr(d, "MediaEmpty") || strstr(d, "MediaNeeded")) {
        add_reason(st, "media-empty-error");
        snprintf(st->message, sizeof(st->message), "Out of paper");
    }
    if (strstr(d, "MediaJam")) {
        add_reason(st, "media-jam-error");
        snprintf(st->message, sizeof(st->message), "Paper jam");
    }
    if (strstr(d, "DoorOpen") || strstr(d, "CoverOpen")) {
        add_reason(st, "cover-open-error");
        snprintf(st->message, sizeof(st->message), "Cover open");
    }
    st->nsupplies = 2;
    st->supply[0].name = "Black";
    st->supply[0].color = "#000000";
    st->supply[0].level = data ? ink_level(data, "BK=") : -2;
    st->supply[1].name = "Color";
    st->supply[1].color = "#00FFFF#FF00FF#FFFF00";
    st->supply[1].level = data ? ink_level(data, "CL=") : -2;
}

// ---------------------------------------------------------------------------

// borders 10/10/9/15 pt (Gutenprint's MG2400 caps) in 1/100 mm
static const printer_paper_t PAPERS[] = {
    {"A4", "iso_a4_210x297mm", 21000, 29700},           // = BJL_PAPER_A4
    {"Letter", "na_letter_8.5x11in", 21590, 27940},     // = BJL_PAPER_LETTER
};

const printer_driver_t canon_mg2500_driver = {
    .id = "canon-mg2500",
    .make_model = "Canon MG2500 series",
    .usb_mfg = "Canon",
    .usb_mdl = "MG2500 series",
    .usb_vid = 0x04a9,
    .usb_pid = 0x176d,
    .color = true,
    .scanner = true,
    .resolutions = {300, 600},
    .nresolutions = 2,
    .papers = PAPERS,
    .npapers = 2,
    .margin_left = 353,
    .margin_right = 353,
    .margin_top = 318,
    .margin_bottom = 529,
    .job_begin = job_begin,
    .page_begin = page_begin,
    .page_line = page_line,
    .page_end = page_end,
    .job_end = job_end,
    .job_stats = job_stats,
    .test_page = test_page,
    .status = status,
};
