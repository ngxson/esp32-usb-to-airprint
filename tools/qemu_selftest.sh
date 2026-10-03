#!/bin/bash
# Runs the firmware on Espressif QEMU (emulated ESP32-S3):
#  1. dry-run test page jobs: stream CRC per mode must match the host harness
#  2. the AirPrint/IPP server over QEMU's emulated Ethernet: IPP conformance
#     tests and URF print jobs, whose printer stream CRC must match the host
#     build of the same server
#  3. scanning from the emulated scanner: streamed scans (through a ring much
#     smaller than the image) must be the host build's JPEGs, and a kept scan
#     that does not fit must fail cleanly
#  4. the debug server: info, log (HTTP and live stream), OTA updates: bad
#     images refused, an update rebooted before it confirmed itself rolled
#     back by the bootloader, an update that confirms itself kept (with
#     tools/remote.py); a crash: core dump, summary, decoded by remote.py
#
#   QEMU_XTENSA=/path/to/qemu-system-xtensa [QEMU_LIB_PATH=dir:dir] tools/qemu_selftest.sh
#
# QEMU builds: https://github.com/espressif/qemu/releases (esp-develop-9.0.0+).
# QEMU_LIB_PATH is exported as DYLD_FALLBACK_LIBRARY_PATH just for QEMU (macOS
# strips DYLD_* variables before they reach this script).
set -euo pipefail
cd "$(dirname "$0")/.."
QEMU=${QEMU_XTENSA:-qemu-system-xtensa}
PY=~/.platformio/penv/bin/python
ESPTOOL=~/.platformio/packages/tool-esptoolpy/esptool.py
B=.pio/build/qemu
OUT=$(mktemp -d)

pio run -e qemu >/dev/null
make -C host -s
# app offset = first app partition in partitions.csv
APP_OFS=$(awk -F, '$2 ~ /app/ {gsub(/ /, "", $4); print $4; exit}' partitions.csv)
$PY $ESPTOOL --chip esp32s3 merge_bin --fill-flash-size 16MB -o "$OUT/flash.bin" \
    --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0 $B/bootloader.bin 0x8000 $B/partitions.bin $APP_OFS $B/firmware.bin >/dev/null

# emulated Ethernet; host ports 18631, 18000, 18080, 12323 -> ESP32 631 (IPP), 80 (web), 8080 (debug), 2323 (log)
( DYLD_FALLBACK_LIBRARY_PATH=${QEMU_LIB_PATH:-} "$QEMU" -nographic -machine esp32s3 -drive file="$OUT/flash.bin",if=mtd,format=raw \
    -nic user,model=open_eth,hostfwd=tcp::18631-:631,hostfwd=tcp::18080-:8080,hostfwd=tcp::12323-:2323,hostfwd=tcp::18000-:80 \
    -serial file:"$OUT/serial.log" -monitor none & echo $! > "$OUT/pid"; wait ) &
sleep 1
for i in $(seq 1 1800); do
    sleep 1
    kill -0 "$(cat "$OUT/pid")" 2>/dev/null || { echo "QEMU exited early"; break; }
    grep -q "QEMU_SELFTEST ipp ready" "$OUT/serial.log" 2>/dev/null && break
    grep -qE "abort\(\)|Guru Meditation|panic|no IP address" "$OUT/serial.log" 2>/dev/null && break
done
status=0

# --- IPP server on the emulated ESP32
if grep -q "QEMU_SELFTEST ipp ready" "$OUT/serial.log"; then
    URI=ipp://localhost:18631/ipp/print
    for t in ipp-1.1.test ipp-2.0.test ipp-everywhere.test; do
        r=$(ipptool -I -T 60 -t $URI $t 2>&1) || true
        echo "esp32s3 $t: $(grep -c '\[PASS\]' <<<"$r") pass, $(grep -c '\[FAIL\]' <<<"$r") fail"
        grep -A3 '\[FAIL\]' <<<"$r" | grep -E "FAIL|EXPECTED" | grep -v image/jpeg || true
    done
    # print the same documents on the ESP32 and on the host build, compare
    PPD=tools/testdata/raster_test.ppd
    cupsfilter -p $PPD -m image/urf -o Resolution=300dpi /usr/share/cups/ipptool/onepage-a4.pdf > "$OUT/a4-300.urf" 2>/dev/null
    cupsfilter -p $PPD -m image/urf /usr/share/cups/ipptool/onepage-a4.pdf > "$OUT/a4-600.urf" 2>/dev/null
    mkdir -p "$OUT/host/out"
    (cd "$OUT/host" && exec "$OLDPWD/host/airprint_host" 18632 > hostsrv.log 2>&1) &
    HOSTSRV=$!
    sleep 1
    n=0
    for doc in a4-300.urf a4-600.urf; do
        ipptool -T 120 -t -f "$OUT/$doc" -d filetype=image/urf $URI print-job.test > "$OUT/esp.log" 2>&1 || true
        ipptool -t -f "$OUT/$doc" -d filetype=image/urf ipp://localhost:18632/ipp/print print-job.test >/dev/null 2>&1 || true
        n=$((n + 1))
        # the job just printed is the last one the ESP32 logged
        dev=$(grep "QEMU_JOB" "$OUT/serial.log" | tail -1 | sed -E 's/.*crc32=([0-9a-f]+).*/\1/')
        host=$(python3 -c "import zlib,sys; print('%08x' % zlib.crc32(open(sys.argv[1],'rb').read()))" "$OUT/host/out/job-$n.prn")
        if [ -n "$dev" ] && [ "$dev" = "$host" ]; then
            echo "MATCH print $doc: esp32s3 $dev == host $host ($(grep -oE 'PASS|FAIL' "$OUT/esp.log"))"
        else
            echo "MISMATCH print $doc: esp32s3 '$dev' vs host $host"; status=1
        fi
    done
    kill $HOSTSRV 2>/dev/null || true
    grep "QEMU_JOB" "$OUT/serial.log" | tail -2 | sed -E 's/.*(bytes=.*)/  esp32s3 job: \1/'

    # --- web pages on port 80
    if curl -s -m 10 http://localhost:18000/ | grep -q 'href="/scan"' &&
       curl -s -m 10 http://localhost:18000/scan | grep -q '<title>Scan</title>'; then
        echo "MATCH web pages on port 80: status and scan"
    else
        echo "MISMATCH web pages on port 80"; status=1
    fi

    # --- scanning
    S=http://localhost:18000/scan
    same() {    # same <what> <esp32 file> <host file>
        if cmp -s "$2" "$3"; then
            echo "MATCH $1: $(wc -c < "$2" | tr -d ' ') bytes, identical to the host build"
        else
            echo "MISMATCH $1: esp32s3 $(wc -c < "$2" 2>/dev/null | tr -d ' ') bytes vs host $(wc -c < "$3" | tr -d ' ')"; status=1
        fi
    }
    host/scan_host scan 150 a4 "$OUT/ref150.jpg" "$OUT/ref150.ppm" >/dev/null 2>&1
    host/scan_host scan 300 letter "$OUT/ref300.jpg" "$OUT/ref300.ppm" >/dev/null 2>&1
    curl -s -m 900 -o "$OUT/esp150.jpg" "$S/stream.jpg?dpi=150&paper=a4" || true
    same "scan stream 150 dpi A4" "$OUT/esp150.jpg" "$OUT/ref150.jpg"
    curl -s -m 1800 -o "$OUT/esp300.jpg" "$S/stream.jpg?dpi=300&paper=letter" || true
    same "scan stream 300 dpi Letter (ring wraps)" "$OUT/esp300.jpg" "$OUT/ref300.jpg"
    # the 48 KB buffer cannot hold a kept scan: it must fail cleanly
    curl -s -m 10 -X POST "$S/last?paper=a4" >/dev/null || true
    for i in $(seq 1 900); do
        curl -s -m 10 "$S/status" | grep -q '"state":"scanning"' || break
        sleep 1
    done
    if curl -s -m 10 "$S/status" | grep -q "does not fit"; then
        echo "MATCH scan kept 150 dpi: refused cleanly (does not fit in the 48 KB buffer)"
    else
        echo "MISMATCH scan kept 150 dpi: $(curl -s -m 10 "$S/status")"; status=1
    fi
    grep "QEMU_SCAN" "$OUT/serial.log" | sed -E 's/.*QEMU_SCAN /  esp32s3 scan: /'
    if grep "QEMU_SCAN" "$OUT/serial.log" | grep -vq "violations=0 "; then
        echo "emulated scanner saw protocol violations"; status=1
    fi

    # --- debug server and OTA
    D=http://localhost:18080
    field() { curl -s -m 10 $D/info | awk -v k="$1" '$1 == k {print $2}'; }
    boots() { grep -ac "QEMU_SELFTEST ipp ready" "$OUT/serial.log"; }
    wait_boot() {   # until the IPP server has come up $1 times
        for i in $(seq 1 600); do [ "$(boots)" -ge "$1" ] && sleep 1 && return 0; sleep 1; done; return 1
    }
    check() {       # check <what> <got> <expected>
        if [ "$2" = "$3" ]; then echo "MATCH $1: $2"; else echo "MISMATCH $1: '$2', expected '$3'"; status=1; fi
    }
    # (the rollback bootloader marks the image it first boots valid)
    check "debug /info partition" "$(field partition) $(field ota_state)" "ota_0 valid"
    # (the 16 KB ring has long dropped the boot; the scans' last lines are recent)
    check "debug /log has the scans" "$(curl -s -m 10 $D/log | grep -ac 'QEMU_SCAN')" 3
    check "debug live log (port 2323)" "$(nc -w 3 localhost 12323 | grep -ac 'QEMU_SCAN')" 3
    head -c 200000 /dev/urandom > "$OUT/junk.bin"
    check "OTA of a non-image" "$(curl -s -m 60 --data-binary @"$OUT/junk.bin" $D/ota)" \
        "OTA failed: not an ESP firmware image (use firmware.bin, not the .elf)"
    head -c 300000 $B/firmware.bin > "$OUT/cut.bin"
    check "OTA of a truncated image" "$(curl -s -m 120 --data-binary @"$OUT/cut.bin" $D/ota)" \
        "OTA failed: image is corrupted (validation failed)"
    check "OTA upload" "$(curl -s -m 300 --data-binary @$B/firmware.bin $D/ota)" "OK, rebooting into the new firmware"
    wait_boot 2
    check "after OTA" "$(field partition) $(field ota_state)" "ota_1 pending"
    curl -s -m 10 -X POST $D/reboot >/dev/null    # reset before it confirmed itself
    wait_boot 3
    check "rebooted before confirming: rolled back" "$(field partition)" "ota_0"
    # the second time with the tool: uploads, waits for the reboot and the confirmation
    R="env REMOTE_HTTP_PORT=18080 REMOTE_LOG_PORT=12323 python3 tools/remote.py --host localhost"
    check "remote.py upload" "$($R upload $B/firmware.bin 2>&1 | tail -1 | cut -d' ' -f1,4)" "done: confirmed"
    check "update confirmed" "$(field partition) $(field ota_state)" "ota_1 valid"
    check "log keeps the previous boot's end" \
        "$(curl -s -m 10 $D/log | grep -ac "end of the previous boot's log")" 1
    # a crash: core dump in flash, summary in the next boot's log, decoded by remote.py
    $R panic >/dev/null
    wait_boot 5
    check "reset reason after the crash" "$(curl -s -m 10 $D/info | awk '$1 == "reset" {print $2}')" panic
    check "crash summary in the log" "$(curl -s -m 10 $D/log | grep -ac 'a previous run crashed')" 1
    $R crash --erase --elf $B/firmware.elf > "$OUT/crash.txt" 2>&1 || true
    check "core dump decoded" "$(grep -c 'panic_cb' "$OUT/crash.txt" | tr -d ' ' | sed 's/^[1-9][0-9]*$/found/')" found
    check "core dump erased" "$(field coredump)" none
else
    echo "IPP server did not come up"; status=1
fi
kill "$(cat "$OUT/pid")" 2>/dev/null || true
wait 2>/dev/null || true

grep -E "QEMU_SELFTEST mode|Guru|abort\(\)|panic|stack overflow" "$OUT/serial.log" | sed 's/\x1b\[[0-9;]*m//g'

for dpi in 300 600; do
    host=$(host/bjl_host testpage $dpi "$OUT/host$dpi.prn" | sed -E 's/.*crc32 ([0-9a-f]+).*/\1/')
    dev=$(grep -a "QEMU_SELFTEST mode=${dpi}x${dpi}dpi" "$OUT/serial.log" | head -1 | sed -E 's/.*crc32=([0-9a-f]+).*/\1/' || true)
    if [ -n "$dev" ] && [ "$dev" = "$host" ]; then
        echo "MATCH ${dpi}dpi: esp32s3 $dev == host $host"
    else
        echo "MISMATCH ${dpi}dpi: esp32s3 '${dev}' vs host $host"; status=1
    fi
done
rm -rf "$OUT"
exit $status
