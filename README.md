# ESP32-S3 AirPrint server for the Canon PIXMA MG2500

Turns a USB-only Canon PIXMA MG2500-series printer into a wireless printer
and scanner. An ESP32-S3 board plugs into the printer's USB port and joins
your WiFi:

- **Print** from iPhone, iPad and Mac (AirPrint), Android (Mopria) and Linux,
  with no drivers or apps.
- **Scan** from any browser, or from Image Capture and Preview on a Mac.

## What you need

- An ESP32-S3 dev board with two USB-C ports (ESP32-S3-N16R8: 16 MB flash,
  8 MB PSRAM), labelled **COM** and **USB**.
- A Canon PIXMA MG2500-series printer (MG2500, MG2520, MG2540, MG2550, …).
- A USB-C to USB-B printer cable.
- A 2.4 GHz WiFi network.
- A computer with [PlatformIO](https://platformio.org/) to build and flash.

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

- **Print:** on an iPhone, Share → Print and pick the printer. On a Mac, add
  it in System Settings → Printers & Scanners.
- **Scan in a browser:** put the page on the glass and open
  `http://canon-mg2500.local/scan` (your `HOSTNAME`).
  - *Live to this browser* (150, 300 or 600 dpi): the page appears as it is
    scanned; press **Save** at the end.
  - *Keep on the printer* (150 dpi): you can close the page and come back
    for the scan later.
- **Scan on a Mac:** open Image Capture (or Preview → File → Import from
  Scanner) and pick the printer.
- **Status page** (ink levels, recent jobs): `http://canon-mg2500.local/`.

The printer does one thing at a time: a print waits while it scans.

The board's LED: blinking yellow = no printer, green = ready, blinking
green = printing or scanning, red = the last test page failed, blue =
"Identify" in the printer settings. The BOOT button prints a test page;
holding it 2 s cancels what is running.

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

Technical details are in [AGENTS.md](AGENTS.md).
