// Status screen layout and a small 5 x 7 bitmap font.
#include "oled_ui.h"

#include <stdio.h>
#include <string.h>

// 5 x 7 font, ASCII 0x20-0x7e, then 0x7f = ellipsis. One byte per column,
// bit 0 at the top (the classic HD44780-style glyphs; g j p q y use the
// eighth row for their descenders).
static const uint8_t FONT[96][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5f, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7f, 0x14, 0x7f, 0x14}, {0x24, 0x2a, 0x7f, 0x2a, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x00, 0x07, 0x00, 0x00}, {0x00, 0x1c, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1c, 0x00}, {0x2a, 0x1c, 0x7f, 0x1c, 0x2a}, {0x08, 0x08, 0x3e, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4b, 0x31}, {0x18, 0x14, 0x12, 0x7f, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1e}, {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06}, {0x32, 0x49, 0x79, 0x41, 0x3e},
    {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36}, {0x3e, 0x41, 0x41, 0x41, 0x22},
    {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41}, {0x7f, 0x09, 0x09, 0x09, 0x01},
    {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f}, {0x00, 0x41, 0x7f, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41}, {0x7f, 0x40, 0x40, 0x40, 0x40},
    {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f}, {0x3e, 0x41, 0x41, 0x41, 0x3e},
    {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e}, {0x7f, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01}, {0x3f, 0x40, 0x40, 0x40, 0x3f},
    {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7f, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7f, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7f, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20}, {0x38, 0x44, 0x44, 0x48, 0x7f},
    {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7e, 0x09, 0x01, 0x02}, {0x98, 0xa4, 0xa4, 0xa4, 0x7c},
    {0x7f, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7d, 0x40, 0x00}, {0x40, 0x80, 0x84, 0x7d, 0x00},
    {0x7f, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7f, 0x40, 0x00}, {0x7c, 0x04, 0x18, 0x04, 0x78},
    {0x7c, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0xfc, 0x24, 0x24, 0x24, 0x18},
    {0x18, 0x24, 0x24, 0x24, 0xfc}, {0x7c, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3f, 0x44, 0x40, 0x20}, {0x3c, 0x40, 0x40, 0x20, 0x7c}, {0x1c, 0x20, 0x40, 0x20, 0x1c},
    {0x3c, 0x40, 0x30, 0x40, 0x3c}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x9c, 0xa0, 0xa0, 0xa0, 0x7c},
    {0x44, 0x64, 0x54, 0x4c, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00}, {0x00, 0x00, 0x7f, 0x00, 0x00},
    {0x00, 0x41, 0x36, 0x08, 0x00}, {0x08, 0x04, 0x08, 0x10, 0x08}, {0x40, 0x00, 0x40, 0x00, 0x40},
};
#define ELLIPSIS 0x7f

#define CELL_W 6
#define LINE_H 8

// rows (top of each text line)
#define Y_NAME 0
#define Y_IP 9
#define Y_RULE 19
#define Y_HEAD 23
#define Y_LINE1 41
#define Y_LINE2 53
#define HEAD_SCALE 2

void oled_clear(uint8_t *fb)
{
    memset(fb, 0, OLED_FB_SIZE);
}

void oled_pixel(uint8_t *fb, int x, int y, bool on)
{
    if (x < 0 || y < 0 || x >= OLED_WIDTH || y >= OLED_HEIGHT)
        return;
    uint8_t *b = &fb[(y >> 3) * OLED_WIDTH + x];
    uint8_t bit = (uint8_t)(1u << (y & 7));
    *b = on ? (uint8_t)(*b | bit) : (uint8_t)(*b & ~bit);
}

void oled_fill(uint8_t *fb, int x, int y, int w, int h, bool on)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            oled_pixel(fb, i, j, on);
}

// the glyph for a byte of UTF-8 text: '?' for a character outside ASCII, -1
// for the continuation bytes that follow its first byte
static int glyph(unsigned char c)
{
    if (c >= 0x80)
        return c >= 0xc0 ? '?' - 0x20 : -1;
    if (c < 0x20)
        return '?' - 0x20;
    return c - 0x20;
}

static int draw_glyph(uint8_t *fb, int x, int y, int g, int scale)
{
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 8; row++)
            if (FONT[g][col] >> row & 1)
                oled_fill(fb, x + col * scale, y + row * scale, scale, scale, true);
    return x + CELL_W * scale;
}

int oled_text(uint8_t *fb, int x, int y, const char *s, int scale)
{
    for (; *s; s++) {
        int g = glyph((unsigned char)*s);
        if (g >= 0)
            x = draw_glyph(fb, x, y, g, scale);
    }
    return x;
}

int oled_text_width(const char *s, int scale)
{
    int n = 0;
    for (; *s; s++)
        n += glyph((unsigned char)*s) >= 0;
    return n * CELL_W * scale;
}

// Text that ends with an ellipsis when it is wider than max_w.
static int text_fit(uint8_t *fb, int x, int y, const char *s, int max_w)
{
    if (oled_text_width(s, 1) <= max_w)
        return oled_text(fb, x, y, s, 1);
    int end = x + max_w - CELL_W;
    for (; *s; s++) {
        int g = glyph((unsigned char)*s);
        if (g < 0)
            continue;
        if (x + CELL_W > end)
            break;
        x = draw_glyph(fb, x, y, g, 1);
    }
    return draw_glyph(fb, x, y, ELLIPSIS - 0x20, 1);
}

static void text_right(uint8_t *fb, int right, int y, const char *s)
{
    oled_text(fb, right - oled_text_width(s, 1) + 1, y, s, 1);
}

// a bar of w x 7 pixels, filled to percent
static void bar(uint8_t *fb, int x, int y, int w, int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    oled_fill(fb, x, y, w, 1, true);
    oled_fill(fb, x, y + 6, w, 1, true);
    oled_fill(fb, x, y, 1, 7, true);
    oled_fill(fb, x + w - 1, y, 1, 7, true);
    oled_fill(fb, x + 2, y + 2, (w - 4) * percent / 100, 3, true);
}

// a progress bar from x to the right edge with the percentage after it
static void progress(uint8_t *fb, int x, int y, int percent)
{
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", percent < 0 ? 0 : percent > 100 ? 100 : percent);
    int right = OLED_WIDTH - 4 * CELL_W;    // room for "100%"
    bar(fb, x, y, right - x - 3, percent);
    text_right(fb, OLED_WIDTH - 1, y, pct);
}

// four bars, like a phone's WiFi signal
static void wifi_icon(uint8_t *fb, int right, int y, bool connected, int rssi)
{
    int bars = !connected ? 0 : rssi == 0 || rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        int x = right - (3 - i) * 4 - 2, h = 2 * (i + 1);
        if (i < bars)
            oled_fill(fb, x, y + 8 - h, 3, h, true);
        else
            oled_fill(fb, x, y + 7, 3, 1, true);
    }
}

static const char *paper_label(const char *paper)
{
    if (!paper)
        return "";
    if (!strcmp(paper, "a4"))
        return "A4";
    if (!strcmp(paper, "letter"))
        return "Letter";
    if (!strcmp(paper, "card"))
        return "ID card";
    return "Area";
}

static const char *source_label(const char *source)
{
    if (!source)
        return "";
    if (!strcmp(source, "button"))
        return "button";
    if (!strcmp(source, "escl"))
        return "AirScan";
    if (!strcmp(source, "web") || !strcmp(source, "stream"))
        return "web";
    return source;
}

static void format_size(char *out, size_t n, unsigned long bytes)
{
    if (bytes >= 1024 * 1024)
        snprintf(out, n, "%lu.%lu MB", bytes / (1024 * 1024), bytes % (1024 * 1024) * 10 / (1024 * 1024));
    else
        snprintf(out, n, "%lu KB", (bytes + 1023) / 1024);
}

static bool has_reason(const airprint_status_t *st, const char *reason)
{
    for (int i = 0; i < st->nreasons; i++)
        if (st->reasons[i] && !strcmp(st->reasons[i], reason))
            return true;
    return false;
}

// a printer problem that stops printing, as a headline (fits 10 big letters)
static const char *problem(const airprint_status_t *st)
{
    if (has_reason(st, "media-jam-error"))
        return "PAPER JAM";
    if (has_reason(st, "cover-open-error"))
        return "COVER OPEN";
    if (has_reason(st, "media-empty-error"))
        return "NO PAPER";
    if (has_reason(st, "marker-supply-empty-error"))
        return "NO INK";
    return NULL;
}

static int supply_level(const airprint_status_t *st, const char *name)
{
    for (int i = 0; i < st->nsupplies; i++)
        if (st->supply[i].name && !strcmp(st->supply[i].name, name))
            return st->supply[i].level;
    return -2;
}

// "Ink K [====  ] C [==    ]"; "?" for a level the printer does not know
static void ink_levels(uint8_t *fb, int y, const airprint_status_t *st)
{
    int x = oled_text(fb, 0, y, "Ink", 1) + 4;
    const char *labels[2] = {"K", "C"};
    const char *names[2] = {"Black", "Color"};
    for (int i = 0; i < 2; i++) {
        x = oled_text(fb, x, y, labels[i], 1) + 2;
        int level = supply_level(st, names[i]);
        if (level >= 0)
            bar(fb, x, y, 40, level);
        else
            oled_text(fb, x + 17, y, "?", 1);
        x += 46;
    }
}

static void headline(uint8_t *fb, const char *s)
{
    oled_text(fb, 0, Y_HEAD, s, HEAD_SCALE);
}

static void line(uint8_t *fb, int y, const char *s)
{
    text_fit(fb, 0, y, s, OLED_WIDTH);
}

static bool is(const char *a, const char *b)
{
    return a && !strcmp(a, b);
}

static bool recent(long ended_ago)
{
    return ended_ago >= 0 && ended_ago < OLED_RESULT_S;
}

static void scan_settings(char *out, size_t n, const scan_info_t *sc)
{
    snprintf(out, n, "%s %d dpi", paper_label(sc->paper), sc->dpi);
}

static void render_scan(uint8_t *fb, const scan_info_t *sc)
{
    char buf[48];
    scan_settings(buf, sizeof(buf), sc);
    if (is(sc->state, "scanning")) {
        headline(fb, "SCANNING");
        oled_text(fb, 0, Y_LINE1, buf, 1);
        text_right(fb, OLED_WIDTH - 1, Y_LINE1, source_label(sc->source));
        if (sc->lines <= 0 || sc->height <= 0)
            line(fb, Y_LINE2, "Warming up...");
        else
            progress(fb, 0, Y_LINE2, (int)(100LL * sc->lines / sc->height));
    } else if (is(sc->state, "done")) {
        headline(fb, "SCANNED");
        line(fb, Y_LINE1, buf);
        char size[16];
        format_size(size, sizeof(size), sc->bytes);
        snprintf(buf, sizeof(buf), "%s JPEG", size);
        line(fb, Y_LINE2, buf);
    } else if (is(sc->state, "canceled")) {
        headline(fb, "CANCELED");
        line(fb, Y_LINE1, "Scan canceled");
    } else {
        headline(fb, "FAILED");
        line(fb, Y_LINE1, "Scan failed:");
        line(fb, Y_LINE2, sc->error[0] ? sc->error : "unknown error");
    }
}

static void render_job(uint8_t *fb, const airprint_activity_t *pa)
{
    char buf[32];
    const char *state = pa->job.state;
    if (is(state, "pending") || is(state, "processing")) {
        headline(fb, "PRINTING");
        line(fb, Y_LINE1, pa->job.name);
        if (is(state, "pending")) {
            line(fb, Y_LINE2, "Receiving...");
        } else {
            snprintf(buf, sizeof(buf), "Page %d", pa->job.pages + 1);
            int x = oled_text(fb, 0, Y_LINE2, buf, 1) + 4;
            progress(fb, x, Y_LINE2, pa->job.percent);
        }
    } else if (is(state, "completed")) {
        headline(fb, "PRINTED");
        line(fb, Y_LINE1, pa->job.name);
        snprintf(buf, sizeof(buf), "%d page%s", pa->job.pages, pa->job.pages == 1 ? "" : "s");
        line(fb, Y_LINE2, buf);
    } else if (is(state, "canceled")) {
        headline(fb, "CANCELED");
        line(fb, Y_LINE1, pa->job.name);
        line(fb, Y_LINE2, "Print canceled");
    } else {
        headline(fb, "FAILED");
        line(fb, Y_LINE1, pa->job.name);
        line(fb, Y_LINE2, "Print failed");
    }
}

void oled_ui_render(const oled_state_t *st, uint8_t *fb)
{
    oled_clear(fb);
    const airprint_activity_t *pa = &st->print;
    const scan_info_t *sc = &st->scan;

    // network
    text_fit(fb, 0, Y_NAME, st->name ? st->name : "", OLED_WIDTH - 20);
    if (st->wifi_configured)
        wifi_icon(fb, OLED_WIDTH - 1, Y_NAME, st->ip[0] != 0, st->rssi);
    if (st->setup_ap) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Setup: %s", st->setup_ip ? st->setup_ip : "");
        line(fb, Y_IP, buf);
    } else {
        line(fb, Y_IP, st->ip[0] ? st->ip : st->wifi_configured ? "Connecting to WiFi..." : "No WiFi configured");
    }
    oled_fill(fb, 0, Y_RULE, OLED_WIDTH, 1, true);

    // what the device does, most important first
    bool job_running = is(pa->job.state, "pending") || is(pa->job.state, "processing");
    bool job_recent = pa->job.id && recent(pa->job.ended_ago);
    bool scan_recent = sc->state && !is(sc->state, "idle") && recent(sc->ended_ago);
    if (is(sc->state, "scanning")) {
        render_scan(fb, sc);
    } else if (job_running) {
        render_job(fb, pa);
    } else if (pa->status.busy) {
        headline(fb, "PRINTING");       // the button's test page
        line(fb, Y_LINE1, "Test page");
    } else if (st->setup_ap) {
        headline(fb, "WIFI SETUP");
        line(fb, Y_LINE1, "Join this WiFi:");
        line(fb, Y_LINE2, st->setup_ap);
    } else if (scan_recent && (!job_recent || sc->ended_ago <= pa->job.ended_ago)) {
        render_scan(fb, sc);
    } else if (job_recent) {
        render_job(fb, pa);
    } else if (!pa->status.connected) {
        headline(fb, "NO PRINTER");
        line(fb, Y_LINE1, "Turn on the printer,");
        line(fb, Y_LINE2, "check the USB cable");
    } else {
        const char *p = problem(&pa->status);
        headline(fb, p ? p : "READY");
        line(fb, Y_LINE1, pa->status.message[0] ? pa->status.message : "Press BOOT to scan");
        ink_levels(fb, Y_LINE2, &pa->status);
    }
}
