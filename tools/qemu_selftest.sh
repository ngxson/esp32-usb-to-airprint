#!/bin/bash
# Runs the firmware on Espressif QEMU (emulated ESP32-S3):
#  1. dry-run test page jobs: stream CRC per mode must match the host harness
#  2. the AirPrint/IPP server over QEMU's emulated Ethernet: IPP conformance
#     tests and URF print jobs, whose printer stream CRC must match the host
#     build of the same server
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

# emulated Ethernet; host port 18631 -> ESP32 port 631
( DYLD_FALLBACK_LIBRARY_PATH=${QEMU_LIB_PATH:-} "$QEMU" -nographic -machine esp32s3 -drive file="$OUT/flash.bin",if=mtd,format=raw \
    -nic user,model=open_eth,hostfwd=tcp::18631-:631 \
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
else
    echo "IPP server did not come up"; status=1
fi
kill "$(cat "$OUT/pid")" 2>/dev/null || true
wait 2>/dev/null || true

grep -E "QEMU_SELFTEST mode|Guru|abort\(\)|panic|stack overflow" "$OUT/serial.log" | sed 's/\x1b\[[0-9;]*m//g'

for dpi in 300 600; do
    host=$(host/bjl_host testpage $dpi "$OUT/host$dpi.prn" | sed -E 's/.*crc32 ([0-9a-f]+).*/\1/')
    dev=$(grep "QEMU_SELFTEST mode=${dpi}x${dpi}dpi" "$OUT/serial.log" | sed -E 's/.*crc32=([0-9a-f]+).*/\1/' || true)
    if [ -n "$dev" ] && [ "$dev" = "$host" ]; then
        echo "MATCH ${dpi}dpi: esp32s3 $dev == host $host"
    else
        echo "MISMATCH ${dpi}dpi: esp32s3 '${dev}' vs host $host"; status=1
    fi
done
rm -rf "$OUT"
exit $status
