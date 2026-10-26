// The status screen: a 128 x 64 I2C OLED (SSD1306 or SH1106, address 0x3C or
// 0x3D) on CONFIG_OLED_SDA_GPIO / CONFIG_OLED_SCL_GPIO. Shows the network,
// the printer and what it is doing (components/oled/oled_ui.h). The bus is
// probed once at boot: without a display the pins are released and no task
// runs. A display that stops answering later is looked for every 5 s.
// The screen turns off after CONFIG_OLED_SLEEP_MIN minutes without change
// (OLEDs burn in); a change or the button turns it back on.
#pragma once

#include <stdbool.h>

void oled_start(const char *hostname);
// A button was pressed: keeps the screen on, or turns it back on. Returns
// true if it was off (the press only wakes it), false otherwise (also without
// a display).
bool oled_wake(void);
