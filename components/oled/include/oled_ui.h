// The device's status screen: a 128 x 64 monochrome OLED (SSD1306 / SH1106).
// Portable: the platform gathers the state and sends the frame buffer.
//
//   canon-mg2500       ▂▄▆█    host name, WiFi signal
//   192.168.2.173              IP address (or "Connecting to WiFi", or the
//                              setup network's address)
//   ────────────────────────
//   PRINTING                   headline, double size
//   Invoice.pdf                two lines of details
//   Page 2  ███████░░░░  62%   (or ink levels when idle)
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airprint.h"
#include "scan_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
// SSD1306 layout: 8 pages of 8 rows; byte x of page p holds column x of rows
// 8p..8p+7, bit 0 at the top
#define OLED_FB_SIZE (OLED_WIDTH * OLED_HEIGHT / 8)

// how long a finished job or scan stays on the screen
#define OLED_RESULT_S 60

typedef struct {
    const char *name;           // host name
    bool wifi_configured;       // false: no network to join, the printer works locally only
    const char *setup_ap;       // the WiFi setup network's name while it runs, else NULL
    const char *setup_ip;       // its page's address
    char ip[16];                // "" while not connected
    int rssi;                   // dBm, 0 = unknown
    bool started;               // the print server runs (else only print.status.connected counts)
    airprint_activity_t print;
    scan_info_t scan;           // state NULL = no scanner service
} oled_state_t;

// Draws the screen for this state into fb (OLED_FB_SIZE bytes).
void oled_ui_render(const oled_state_t *st, uint8_t *fb);

// Drawing primitives (also for tests). Text is 5 x 7 in 6 x 8 cells; scale 2
// doubles it. Everything is clipped to the screen.
void oled_clear(uint8_t *fb);
void oled_pixel(uint8_t *fb, int x, int y, bool on);
void oled_fill(uint8_t *fb, int x, int y, int w, int h, bool on);
// Returns the x after the text.
int oled_text(uint8_t *fb, int x, int y, const char *s, int scale);
int oled_text_width(const char *s, int scale);

#ifdef __cplusplus
}
#endif
