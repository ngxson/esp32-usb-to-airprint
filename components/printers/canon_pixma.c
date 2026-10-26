// Canon PIXMA MG2500 series and its relatives: BJ raster in IVEC XML
// (components/canon_bjl), with the status from the IVEC GetStatusResponse
// back channel. One driver per model series (the list at the end); they only
// differ in their names, USB ids, scanner and stream variant (bjl_model_t).
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

static bjl_model_id_t model_of(const printer_driver_t *d)
{
    return *(const bjl_model_id_t *)d->model;
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

static void *job_begin(const printer_driver_t *d, const printer_io_t *io, int dpi, int paper)
{
    canon_job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->io = *io;
    if (bjl_job_init(&j->bjl, model_of(d), mode_for(dpi), (bjl_paper_id_t)paper, sink, &j->io)) {
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

static int test_page(const printer_driver_t *d, const printer_io_t *io, int dpi, const char *label)
{
    bjl_job_t job;
    testpage_t tp;
    if (bjl_job_init(&job, model_of(d), mode_for(dpi), BJL_PAPER_A4, sink, (void *)io) != 0)
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

// borders 10/10/9/15 pt (Gutenprint's MG2400 caps, the same for every model
// here) in 1/100 mm
static const printer_paper_t PAPERS[] = {
    {"A4", "iso_a4_210x297mm", 21000, 29700},           // = BJL_PAPER_A4
    {"Letter", "na_letter_8.5x11in", 21590, 27940},     // = BJL_PAPER_LETTER
};

static const bjl_model_id_t MODELS[BJL_MODEL_COUNT] = {
    BJL_MODEL_MG2400, BJL_MODEL_MG2900, BJL_MODEL_IP2800, BJL_MODEL_E3100, BJL_MODEL_MP230, BJL_MODEL_IP2700,
};

// mdl: the IEEE 1284 MDL; pid: the USB product id (from SANE's pixma
// backend), 0 when unknown (matched by mdl); scanner: a pixma flatbed that
// works like the MG2500's up to 600 dpi (SANE generation 4 or 5).
#define CANON_PIXMA(var, id_, mdl, pid, bjl_model, has_scanner)                                                       \
    const printer_driver_t var = {                                                                                     \
        .id = id_,                                                                                                     \
        .make_model = "Canon " mdl,                                                                                    \
        .usb_mfg = "Canon",                                                                                            \
        .usb_mdl = mdl,                                                                                                \
        .usb_vid = (pid) ? 0x04a9 : 0,                                                                                 \
        .usb_pid = pid,                                                                                                \
        .color = true,                                                                                                 \
        .scanner = has_scanner,                                                                                        \
        .resolutions = {300, 600},                                                                                     \
        .nresolutions = 2,                                                                                             \
        .papers = PAPERS,                                                                                              \
        .npapers = 2,                                                                                                  \
        .margin_left = 353,                                                                                            \
        .margin_right = 353,                                                                                           \
        .margin_top = 318,                                                                                             \
        .margin_bottom = 529,                                                                                          \
        .model = &MODELS[bjl_model],                                                                                   \
        .job_begin = job_begin,                                                                                        \
        .page_begin = page_begin,                                                                                      \
        .page_line = page_line,                                                                                        \
        .page_end = page_end,                                                                                          \
        .job_end = job_end,                                                                                            \
        .job_stats = job_stats,                                                                                        \
        .test_page = test_page,                                                                                        \
        .status = status,                                                                                              \
    }

// Tested on the hardware: the MG2500 series. The others send what Gutenprint
// sends for them (tools/compare_gutenprint.py --model), untested on paper.
CANON_PIXMA(canon_mg2500_driver, "canon-mg2500", "MG2500 series", 0x176d, BJL_MODEL_MG2400, true);
CANON_PIXMA(canon_mg2400_driver, "canon-mg2400", "MG2400 series", 0x176c, BJL_MODEL_MG2400, true);
CANON_PIXMA(canon_p200_driver, "canon-p200", "P200 series", 0, BJL_MODEL_MG2400, false);
CANON_PIXMA(canon_mg2900_driver, "canon-mg2900", "MG2900 series", 0x1780, BJL_MODEL_MG2900, true);
CANON_PIXMA(canon_mg3000_driver, "canon-mg3000", "MG3000 series", 0x180b, BJL_MODEL_MG2900, true);
CANON_PIXMA(canon_e470_driver, "canon-e470", "E470 series", 0x180c, BJL_MODEL_MG2900, true);
CANON_PIXMA(canon_e460_driver, "canon-e460", "E460 series", 0x1788, BJL_MODEL_MG2900, true);
CANON_PIXMA(canon_ip2800_driver, "canon-ip2800", "iP2800 series", 0, BJL_MODEL_IP2800, false);
CANON_PIXMA(canon_e3100_driver, "canon-e3100", "E3100 series", 0x1828, BJL_MODEL_E3100, true);
CANON_PIXMA(canon_ts3100_driver, "canon-ts3100", "TS3100 series", 0x1827, BJL_MODEL_E3100, true);
CANON_PIXMA(canon_mp230_driver, "canon-mp230", "MP230 series", 0x175f, BJL_MODEL_MP230, true);
CANON_PIXMA(canon_ip2700_driver, "canon-ip2700", "iP2700 series", 0, BJL_MODEL_IP2700, false);
CANON_PIXMA(canon_mp250_driver, "canon-mp250", "MP250 series", 0x173a, BJL_MODEL_IP2700, true);
CANON_PIXMA(canon_mp270_driver, "canon-mp270", "MP270 series", 0x173b, BJL_MODEL_IP2700, true);
CANON_PIXMA(canon_mp280_driver, "canon-mp280", "MP280 series", 0x1746, BJL_MODEL_IP2700, true);
CANON_PIXMA(canon_mp490_driver, "canon-mp490", "MP490 series", 0x173c, BJL_MODEL_IP2700, true);
CANON_PIXMA(canon_mp493_driver, "canon-mp493", "MP493 series", 0x1757, BJL_MODEL_IP2700, true);
CANON_PIXMA(canon_mp495_driver, "canon-mp495", "MP495 series", 0x1747, BJL_MODEL_IP2700, true);
CANON_PIXMA(canon_mx340_driver, "canon-mx340", "MX340 series", 0x1741, BJL_MODEL_IP2700, true);
CANON_PIXMA(canon_mx350_driver, "canon-mx350", "MX350 series", 0x1742, BJL_MODEL_IP2700, true);
CANON_PIXMA(canon_mx360_driver, "canon-mx360", "MX360 series", 0x174d, BJL_MODEL_IP2700, true);
CANON_PIXMA(canon_mx410_driver, "canon-mx410", "MX410 series", 0x174e, BJL_MODEL_IP2700, true);
CANON_PIXMA(canon_mx420_driver, "canon-mx420", "MX420 series", 0x174f, BJL_MODEL_IP2700, true);
