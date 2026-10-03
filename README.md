# ESP32-S3 AirPrint server for the Canon PIXMA MG2500

Turns a USB-only Canon PIXMA MG2500-series printer into a wireless AirPrint
printer. An ESP32-S3 board plugs into the printer's USB port and joins your
WiFi. iPhone, iPad and Mac then find the printer and print to it natively, no
drivers or apps needed. Android (Mopria) and Linux (CUPS, IPP Everywhere)
work too.

## What you need

- An ESP32-S3 dev board with two USB-C ports (ESP32-S3-N16R8: 16 MB flash,
  8 MB PSRAM), labelled **COM** and **USB**.
- A Canon PIXMA MG2500-series printer (MG2500, MG2520, MG2540, MG2550, …).
- A USB-C to USB-B printer cable.
- A 2.4 GHz WiFi network.
- A computer with [PlatformIO](https://platformio.org/) to build and flash.

## Setup

1. **Hardware.** Bridge the **USB-OTG** pads on the back of the board with a
   solder blob, so the **USB** port supplies 5 V to the printer. Connect the
   printer to the board's **USB** port.
2. **Settings.** Build once (`pio run`): this creates `.env` from
   `.env.example`. Edit `.env`:
   ```
   WIFI_SSID=your-network
   WIFI_PASSWORD=your-password
   PRINTER_NAME=Canon MG2500 (ESP32)
   HOSTNAME=canon-mg2500
   MODE=normal
   ```
   `.env` is git-ignored, so your WiFi password stays local.
3. **Flash.** Connect the board's **COM** port to your computer, then run:
   ```
   pio run -t upload && pio device monitor
   ```
   If your board shows up on another serial port, change `upload_port` and
   `monitor_port` in `platformio.ini`.
4. **Print.** On an iPhone: Share → Print → pick the printer name you set.
   On a Mac it appears in System Settings → Printers & Scanners → Add.

The status page, with ink levels and recent jobs, is at
`http://canon-mg2500.local:631/` (your `HOSTNAME`).

## Using it

Status LED on the board:

| LED | Meaning |
|---|---|
| blinking yellow | ready, no printer connected |
| solid green | printer connected, ready |
| blinking green | printing |
| fast red | the last test page failed |
| blue flashes | "Identify" was pressed in the printer settings |

BOOT button:
- **press**: print an A4 colour test page
- **hold 2 s while printing**: cancel the current job

Supported: A4 and US Letter on plain paper, colour or grayscale, one-sided.

## Trying it without the printer

Set `MODE=dev` in `.env` and flash again. Print jobs are processed in full
but nothing is sent to the printer, so no paper is used. Useful to check
WiFi, discovery and printing from your devices first.
