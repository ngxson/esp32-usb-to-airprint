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
- A computer with [PlatformIO](https://platformio.org/) and
  [Node.js](https://nodejs.org/) 22 or later to build and flash (the build
  installs the web app's packages with `npm ci` the first time).

## Setup

1. **Hardware.** Bridge the **USB-OTG** pads on the back of the board with a
   solder blob, so the **USB** port powers the printer's USB connection.
   Connect the printer to the board's **USB** port.
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
   `pio run -t upload`. If the board is on another serial port, change
   `upload_port` in `platformio.ini`.

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
- **The button on the board** (BOOT): press it to scan the page on the
  glass (A4, 150 dpi). Every open web app shows the scan and saves it to
  its Files. Press twice for a test page; hold 2 s to cancel what is
  running.
- **Print from a phone or a Mac:** on an iPhone, Share → Print and pick the
  printer. On a Mac, add it in System Settings → Printers & Scanners.
- **Scan on a Mac:** open Image Capture (or Preview → File → Import from
  Scanner) and pick the printer.

The printer does one thing at a time: a print waits while it scans.

The board's LED: blinking yellow = no printer, green = ready, blinking
green = printing or scanning, red = the last test page or a button scan
failed, blue = "Identify" in the printer settings.

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
