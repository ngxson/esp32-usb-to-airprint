# ESP32-S3 AirPrint server for the Canon PIXMA MG2500

Turns a USB-only Canon PIXMA MG2500-series printer into a wireless printer
and scanner. An ESP32-S3 board plugs into the printer's USB port and joins
your WiFi:

- **Print** from iPhone, iPad and Mac (AirPrint), Android (Mopria) and Linux,
  with no drivers or apps.
- **Scan** from any browser, from Image Capture and Preview on a Mac, or with
  the button on the board.
- **A web app** on your phone or computer: print PDFs and pictures, scan,
  keep the files in the browser, and put pages together (rotate, crop, ID
  card copies) into a PDF.

## What you need

- An ESP32-S3 dev board with two USB-C ports (ESP32-S3-N16R8: 16 MB flash,
  8 MB PSRAM), labelled **COM** and **USB**.
- A Canon PIXMA MG2500-series printer (MG2500, MG2520, MG2540, MG2550, …).
- A USB-C to USB-B printer cable.
- A 2.4 GHz WiFi network.
- Optional: a 128x64 I2C OLED screen (0.96" SSD1306 or 1.3" SH1106) for
  the status, and up to two push buttons.
- A computer with [PlatformIO](https://platformio.org/) and
  [Node.js](https://nodejs.org/) 22 or later to build and flash (the build
  installs the web app's packages with `npm ci` the first time).

## Setup

1. **Hardware.** Bridge the **USB-OTG** pads on the back of the board with a
   solder blob, so the **USB** port powers the printer's USB connection.
   Connect the printer to the board's **USB** port.
   Optional screen: VCC to **3V3**, GND to **GND**, SCL to **GPIO41**, SDA
   to **GPIO42**. For a 1.3" SH1106 screen, add `CONFIG_OLED_SH1106=y` to
   `sdkconfig.defaults` and delete `sdkconfig.esp32s3` (the other screen
   settings, `OLED_*` in `src/Kconfig.projbuild`, work the same way). The
   board looks for the screen when it starts: connect it before powering on.
   Optional buttons, each between a pin and **GND** (no resistor needed):
   **GPIO4** works like the BOOT button; **GPIO5** is a spare button with
   no action yet, there for your own feature (`on_user_button` in
   `src/main.c`). The pins are `PRINT_BUTTON2_GPIO` and `USER_BUTTON_GPIO`
   in `src/Kconfig.projbuild` (-1 turns one off).
2. **Settings.** Build once (`pio run`): this creates `.env`. Edit it:
   ```
   WIFI_SSID=your-network
   WIFI_PASSWORD=your-password
   PRINTER_NAME=Canon MG2500 (ESP32)
   HOSTNAME=canon-mg2500
   MODE=debug
   ```
   `MODE=debug` lets you update the board over WiFi later (see below);
   `MODE=normal` does without. `.env` is git-ignored, so your WiFi password
   stays local.
3. **Flash.** Connect the board's **COM** port to your computer and run
   `pio run -t upload`. PlatformIO finds the board's serial port; if
   several are connected, add `--upload-port /dev/cu.usbmodem…`.

## Using it

- **The web app:** open `http://canon-mg2500.local/` (your `HOSTNAME`) on
  a phone or a computer. On a phone it opens on a menu; pick what to do.
  - *Printer*: status, ink levels, recent jobs; upload a file and print it.
  - *Scanner*: A4, Letter or an ID card at 150, 300 or 600 dpi. The page
    appears as it is scanned and is saved to *Files*.
  - *Files*: your scans and uploads (PDFs and pictures), kept in this
    browser only (nothing is stored on the printer). Print, download,
    rename, delete. The print dialog has copies, pages, paper, fit / fill /
    actual size, orientation, colour or black and white, and quality.
  - *Process*: put pages from Files together, rotate and crop them, make an
    ID card copy (front and back on one sheet at real size), and save the
    result as a PDF in Files.
- **The button on the board** (BOOT, or one on GPIO4): press it to scan
  the page on the glass (A4, 150 dpi). Every open web app shows the scan
  and saves it to its Files. Press twice for a test page; hold 2 s to
  cancel what is running. While the screen is off, a press only turns it
  on.
- **Print from a phone or a Mac:** on an iPhone, Share → Print and pick the
  printer. On a Mac, add it in System Settings → Printers & Scanners.
- **Scan on a Mac:** open Image Capture (or Preview → File → Import from
  Scanner) and pick the printer.

The printer does one thing at a time: a print waits while it scans.

The board's LED: yellow = no printer, green = ready, blinking
green = printing or scanning, red = the last test page or a button scan
failed, blue = "Identify" in the printer settings.

The screen, if there is one, shows the board's IP address and WiFi signal,
the printer's state and ink, and the progress of a print or scan (a
finished one stays for a minute). It turns off after 10 minutes without
change, so the pixels don't burn in; any activity turns it back on, and so
does a button press (that press does nothing else: press again to scan).

## Updating over WiFi (MODE=debug)

Once flashed with `MODE=debug`, the board no longer needs the computer:

```
pio run -t ota      # build and install the new firmware over WiFi
pio run -t logs     # watch the board's log
```

A new firmware that doesn't come back online is undone automatically, so a
bad build can't lock you out. Anyone on your WiFi can update the board in
this mode.

## Trying it without the printer

Set `MODE=dev` and flash: print jobs are processed but not printed, and
scans show a built-in test page.

On the computer, without the board: `make -C host && cd host &&
./airprint_host`, then open http://localhost:8631/ (an emulated printer and
scanner). To work on the web app with live reloading: `cd web && npm run
dev` (it talks to that server; `DEVICE=http://<board IP> npm run dev` talks
to the board).

Technical details are in [AGENTS.md](AGENTS.md).
