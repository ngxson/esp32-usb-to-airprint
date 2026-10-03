# ESP32-S3 AirPrint server for the Canon PIXMA MG2500

Turns a USB-only Canon PIXMA MG2500-series printer into a wireless AirPrint
printer. An ESP32-S3 board plugs into the printer's USB port and joins your
WiFi. iPhone, iPad and Mac then find the printer and print to it natively, no
drivers or apps needed. Android (Mopria) and Linux (CUPS, IPP Everywhere)
work too. The scanner works from any browser, and on a Mac from Image
Capture and Preview (AirScan / eSCL).

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
`http://canon-mg2500.local/` (your `HOSTNAME`). Scanning is at
`http://canon-mg2500.local/scan`.

## Using it

Status LED on the board:

| LED | Meaning |
|---|---|
| blinking yellow | ready, no printer connected |
| solid green | printer connected, ready |
| blinking green | printing or scanning |
| fast red | the last test page failed |
| blue flashes | "Identify" was pressed in the printer settings |

BOOT button:
- **press**: print an A4 colour test page
- **hold 2 s while printing or scanning**: cancel the current job

Supported: A4 and US Letter on plain paper, colour or grayscale, one-sided.

### Scanning

Put the page on the glass and open `/scan` in a browser. Two modes:

- **Live to this browser** (150, 300 or 600 dpi): the page appears while it
  is scanned. Keep the browser page open until it is done, then press
  **Save**. 600 dpi takes about four times as long as 300 dpi.
- **Keep on the printer** (150 dpi): the scan is stored on the board, so you
  can close the page and come back for it. It stays there until the next scan.

Scans are colour JPEGs. The printer does one thing at a time: print jobs
wait while it scans, and a scan can't start while it prints.

On a Mac, the scanner also shows up in **Image Capture** and in Preview
(File → Import from Scanner) under the printer's name, with no driver to
install. Linux (sane-airscan) and Windows find it the same way.

## Leaving the board at the printer (MODE=debug)

With `MODE=debug` the board can be updated and debugged over WiFi, so it no
longer needs to be plugged into a computer. Flash it once over USB with
`MODE=debug` (this also installs the bootloader that makes updates safe),
then:

```
pio run -t ota          # build and update the firmware over WiFi
pio run -t logs         # follow the board's log (Ctrl-C to stop)
tools/remote.py info    # version, uptime, memory, tasks, last reset
tools/remote.py crash   # after a crash: download and decode the core dump
```

The board is found as `<HOSTNAME>.local`; use `--host` with an IP address if
that does not resolve. An update that does not come back online (WiFi and the
debug server working for 20 s) is undone at the next restart, so a bad build
cannot lock you out. The board also serves http://canon-mg2500.local:8080/
(the debug server) and the live log on port 2323 (`nc canon-mg2500.local 2323`).

Anyone on your network can update the firmware in this mode. Switching back
to `MODE=normal` over WiFi turns the WiFi updates off; the next update then
needs USB.

## Trying it without the printer

Set `MODE=dev` in `.env` and flash again. Print jobs are processed in full
but nothing is sent to the printer, so no paper is used. Scans come from a
built-in test page instead of the scanner. Useful to check WiFi, discovery,
printing and scanning from your devices first.
