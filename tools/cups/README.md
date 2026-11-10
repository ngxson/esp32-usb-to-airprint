# Printing through a Linux CUPS server

The firmware's printer drivers also work without the ESP32: `rastertoprinter`
(`host/cups_filter.c`) is a CUPS filter that turns CUPS raster into a driver's
printer data. CUPS keeps doing the rest (AirPrint, PDF to raster, scaling).
Set up here for the Grozziie TP732H 80 mm thermal label printer, which then no longer
needs its Windows driver.

```
iPhone --AirPrint--> CUPS (pdftopdf, gstoraster, rastertoprinter) --socket :9100-->
    host with the printer (tp732h.socket: cat > /dev/tp732h) --USB--> TP732H
```

The CUPS server and the printer's host can be the same machine
(`socket://localhost:9100`) or two (e.g. CUPS in a container, the printer on
another machine).

## Files

- `tp732h.ppd`: the queue: 80 x 130 mm labels at 203 dpi, 8-bit gray raster into
  `rastertoprinter`.
- `70-tp732h.rules`, `tp732h.socket`, `tp732h@.service`: on the host the printer
  is plugged into. The kernel's `usblp` driver gives the printer a device;
  udev names it `/dev/tp732h` and starts the socket, which copies each
  connection on port 9100 to it (only from `localhost` and the private address ranges:
  narrow `IPAddressAllow` to your network). With the printer off, the port is closed: CUPS's socket
  backend retries, and the job prints when the printer is back.

## Install

On the printer's host (root):

```
cp 70-tp732h.rules /etc/udev/rules.d/
cp tp732h.socket tp732h@.service /etc/systemd/system/
systemctl daemon-reload && udevadm control --reload
```

Unplug and plug the printer in (or switch it off and on); `systemctl status
tp732h.socket` should show it listening.

On the CUPS server: build the filter (`make -C host rastertoprinter CFLAGS=-O2`;
it only needs libc), then

```
install -m 755 host/rastertoprinter /usr/lib/cups/filter/
lpadmin -p label-print -E -v socket://<printer host>:9100 -P tools/cups/tp732h.ppd \
        -o print-scaling-default=fit -o printer-is-shared=true
```

`print-scaling-default=fit` scales documents of another size (e.g. an A6
shipping label) onto the label.

## Checking without the printer

`tools/run_tests.sh` runs a PDF through CUPS's own chain and the filter when
`cupsfilter` is installed. By hand:

```
cupsfilter -p tools/cups/tp732h.ppd -m application/vnd.cups-raster -o print-scaling=fit doc.pdf > doc.ras
host/rastertoprinter 1 me doc 1 "" doc.ras > doc.prn
tools/tp732h_decode.py doc.prn --png doc.png
```

A real queue can also print into a file on another machine: point it at
`socket://<machine>:9100` and run `nc -l -p 9100 > job.prn` there.
