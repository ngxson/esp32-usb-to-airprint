// The status screen on the host: renders each state the device can show
// into out/oled-<name>.pbm (tools/oled_preview.py makes one PNG of them).
//   oled_host selftest     drawing checks
//   oled_host render DIR   the screens
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oled_ui.h"

static int failures;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                \
        }                                                              \
    } while (0)

static bool px(const uint8_t *fb, int x, int y)
{
    return fb[(y >> 3) * OLED_WIDTH + x] >> (y & 7) & 1;
}

static int count(const uint8_t *fb, int x0, int y0, int w, int h)
{
    int n = 0;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            n += px(fb, x, y);
    return n;
}

static void selftest(void)
{
    uint8_t fb[OLED_FB_SIZE];
    oled_clear(fb);
    oled_pixel(fb, 0, 0, true);
    oled_pixel(fb, 127, 63, true);
    oled_pixel(fb, 3, 9, true);
    CHECK(fb[0] == 0x01 && fb[OLED_FB_SIZE - 1] == 0x80 && fb[OLED_WIDTH + 3] == 0x02);
    oled_pixel(fb, -1, 0, true);        // clipped
    oled_pixel(fb, 128, 64, true);
    CHECK(count(fb, 0, 0, OLED_WIDTH, OLED_HEIGHT) == 3);
    oled_pixel(fb, 3, 9, false);
    CHECK(count(fb, 0, 0, OLED_WIDTH, OLED_HEIGHT) == 2);

    // "H": columns 0 and 4 full, the bar in row 3
    oled_clear(fb);
    CHECK(oled_text(fb, 0, 0, "H", 1) == 6);
    CHECK(count(fb, 0, 0, 1, 7) == 7 && count(fb, 4, 0, 1, 7) == 7 && count(fb, 1, 3, 3, 1) == 3);
    CHECK(count(fb, 0, 0, OLED_WIDTH, OLED_HEIGHT) == 17);
    oled_clear(fb);
    CHECK(oled_text(fb, 0, 0, "H", 2) == 12);
    CHECK(count(fb, 0, 0, OLED_WIDTH, OLED_HEIGHT) == 17 * 4);
    // UTF-8: one '?' per character, whatever its length
    CHECK(oled_text_width("caf\xc3\xa9 \xe2\x82\xac", 1) == 6 * 6);
    // text past the edge is clipped, not wrapped
    oled_clear(fb);
    oled_text(fb, 120, 60, "WWWW", 2);
    CHECK(count(fb, 0, 0, 120, OLED_HEIGHT) == 0 && count(fb, 120, 0, 8, 60) == 0);
}

typedef struct {
    const char *name;
    oled_state_t st;
} scenario_t;

static oled_state_t base(void)
{
    oled_state_t s = {0};
    s.name = "canon-mg2500";
    s.wifi_configured = true;
    strcpy(s.ip, "192.168.2.173");
    s.rssi = -58;
    s.started = true;
    airprint_status_t *p = &s.print.status;
    p->connected = true;
    p->nsupplies = 2;
    p->supply[0].name = "Black";
    p->supply[0].level = -2;
    p->supply[1].name = "Color";
    p->supply[1].level = 0;
    p->reasons[p->nreasons++] = "marker-supply-low-warning";
    strcpy(p->message, "Ink low");
    s.print.state = 3;
    s.print.job.percent = -1;
    s.print.job.ended_ago = -1;
    s.scan.scanner = true;
    s.scan.state = "idle";
    s.scan.source = "";
    s.scan.paper = "a4";
    s.scan.ended_ago = -1;
    return s;
}

static void render(const char *dir)
{
    scenario_t sc[16];
    int n = 0;
    oled_state_t s;

    s = base();
    s.ip[0] = 0;
    s.print.status.connected = false;
    s.started = false;
    sc[n++] = (scenario_t){"booting", s};

    s = base();
    s.print.status.connected = false;
    s.print.status.nreasons = 0;
    s.print.status.message[0] = 0;
    sc[n++] = (scenario_t){"no-printer", s};

    s = base();
    s.print.status.nreasons = 0;
    s.print.status.message[0] = 0;
    s.print.status.supply[0].level = 70;
    s.print.status.supply[1].level = 35;
    s.rssi = -80;
    sc[n++] = (scenario_t){"ready", s};

    s = base();
    sc[n++] = (scenario_t){"ready-ink-low", s};

    s = base();
    s.print.status.reasons[s.print.status.nreasons++] = "media-empty-error";
    strcpy(s.print.status.message, "Out of paper");
    sc[n++] = (scenario_t){"no-paper", s};

    s = base();
    s.print.state = 4;
    s.print.job.id = 3;
    strcpy(s.print.job.name, "Quarterly report 2026 (final, v2).pdf");
    s.print.job.state = "processing";
    s.print.job.pages = 1;
    s.print.job.percent = 62;
    sc[n++] = (scenario_t){"printing", s};

    s = base();
    s.print.job.id = 3;
    strcpy(s.print.job.name, "Photo \xe2\x80\x94 \xc3\xa9t\xc3\xa9.jpg");
    s.print.job.state = "completed";
    s.print.job.pages = 3;
    s.print.job.ended_ago = 5;
    sc[n++] = (scenario_t){"printed", s};

    s = base();
    s.print.status.busy = true;
    sc[n++] = (scenario_t){"test-page", s};

    s = base();
    s.scan.state = "scanning";
    s.scan.source = "button";
    s.scan.dpi = 150;
    s.scan.height = 1754;
    sc[n++] = (scenario_t){"scan-warmup", s};

    s = base();
    s.scan.state = "scanning";
    s.scan.source = "escl";
    s.scan.paper = "custom";
    s.scan.dpi = 300;
    s.scan.lines = 1000;
    s.scan.height = 3508;
    sc[n++] = (scenario_t){"scanning", s};

    s = base();
    s.scan.state = "done";
    s.scan.source = "button";
    s.scan.dpi = 150;
    s.scan.bytes = 403456;
    s.scan.ended_ago = 2;
    sc[n++] = (scenario_t){"scanned", s};

    s = base();
    s.scan.state = "failed";
    s.scan.dpi = 600;
    strcpy(s.scan.error, "the scanner did not answer (timeout)");
    s.scan.ended_ago = 2;
    sc[n++] = (scenario_t){"scan-failed", s};

    s = base();
    s.wifi_configured = false;
    s.ip[0] = 0;
    s.started = false;
    sc[n++] = (scenario_t){"no-wifi", s};

    s = base();
    s.wifi_configured = false;
    s.ip[0] = 0;
    s.started = false;
    s.setup_ap = "Printer-Setup-A1B2";
    s.setup_ip = "192.168.4.1";
    sc[n++] = (scenario_t){"wifi-setup", s};

    uint8_t fb[OLED_FB_SIZE];
    for (int i = 0; i < n; i++) {
        oled_ui_render(&sc[i].st, fb);
        char path[512];
        snprintf(path, sizeof(path), "%s/oled-%02d-%s.pbm", dir, i, sc[i].name);
        FILE *f = fopen(path, "wb");
        if (!f) {
            perror(path);
            exit(1);
        }
        fprintf(f, "P4\n%d %d\n", OLED_WIDTH, OLED_HEIGHT);
        for (int y = 0; y < OLED_HEIGHT; y++) {
            for (int x = 0; x < OLED_WIDTH; x += 8) {
                uint8_t b = 0;
                for (int k = 0; k < 8; k++)
                    b |= (uint8_t)(px(fb, x + k, y) << (7 - k));
                fputc(b, f);
            }
        }
        fclose(f);
    }
    printf("%d screens in %s/oled-*.pbm\n", n, dir);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "selftest")) {
        selftest();
        printf("oled: %s\n", failures ? "FAILED" : "all tests passed");
        return failures != 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "render")) {
        render(argv[2]);
        return 0;
    }
    fprintf(stderr, "usage: oled_host selftest | render DIR\n");
    return 2;
}
