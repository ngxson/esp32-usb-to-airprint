// Remote debugging and firmware updates for a board that is not plugged into
// a computer (MODE=debug):
//   - log: everything ESP_LOGx prints is also kept in a RAM ring, served at
//     http://<host>:8080/log and streamed live on TCP port 2323 (nc <host> 2323).
//     The last few KB survive a crash or reboot (RTC memory) and are shown
//     at the top of the next boot's log.
//   - crashes: a core dump goes to the "coredump" flash partition; its summary
//     is logged at the next boot and the dump is served at /coredump.
//   - OTA: POST a firmware image to /ota. The new image must confirm itself
//     (debug_ota_confirm) or the bootloader goes back to the previous one.
//   - /info: version, uptime, reset reason, memory, tasks, OTA state.
// tools/remote.py drives all of it.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define DEBUG_HTTP_PORT 8080
#define DEBUG_LOG_PORT 2323

// Starts capturing the log into a ring of ring_size bytes (PSRAM if
// available). Call early: everything before is only on the UART.
void debug_log_start(size_t ring_size);
// HTTP server and log stream. busy() (optional) refuses OTA while it returns
// true (printing, scanning).
esp_err_t debug_server_start(bool (*busy)(void));

// For a freshly updated image (pending verification): waits until healthy()
// has held for settle_s seconds, then marks the image valid; rolls back and
// reboots if that does not happen within timeout_s. Returns at once for an
// image that is already valid, or once something else marks it valid. Run it
// from its own task.
void debug_ota_confirm(bool (*healthy)(void), int settle_s, int timeout_s);
