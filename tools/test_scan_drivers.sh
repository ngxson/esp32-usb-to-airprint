#!/bin/bash
# The scan service drives the scanner of the attached printer's driver, on
# every path: host/airprint_host per driver; for each scan, the protocol that
# answered (from the emulated scanner's log, no protocol violations) and the
# JPEG's size against the scan's own status.
#   tools/test_scan_drivers.sh        (after make -C host airprint_host)
set -uo pipefail
cd "$(dirname "$0")/.."
OUT=$(mktemp -d)
PORT=${PORT:-18741}
W=http://127.0.0.1:$PORT
fail=0
say() { echo "$1"; case "$1" in *FAIL*) fail=1 ;; esac; }

# the last scan's JPEG ($1) against /scan/status, and the log's newest "scanner closed" line
judge() {   # jpeg, protocol, what, log
    local st size want closed
    st=$(curl -s $W/scan/status)
    want=$(echo "$st" | python3 -c "import json,sys; s=json.load(sys.stdin); print(s['width'], s['height'], s['state'])")
    size=$(djpeg "$1" 2>/dev/null | head -2 | tail -1)
    closed=$(grep 'scanner closed' "$4" | tail -1)
    if [ "$closed" != "host: $2 scanner closed (0 protocol violations)" ]; then
        say "  $3: FAIL: ${closed:-no scan}"
    elif [ "$size done" != "$want" ]; then
        say "  $3: FAIL: JPEG $size, status $want"
    elif [ -n "${5:-}" ] && ! echo "$st" | python3 -c '
import json,sys
s=json.load(sys.stdin)
sys.exit(not (s["id"] and s["id"] != sys.argv[1] and s["source"] == "button"
              and s["dpi"] == 150 and s["paper"] == "a4"
              and (s["width"], s["height"]) == (1240, 1754)))' "$5"; then
        say "  $3: FAIL: no new button A4 150 dpi scan"
    else
        say "  $3: ok ($size)"
    fi
}

wait_done() {
    for _ in $(seq 600); do
        curl -s $W/scan/status | grep -q '"state":"scanning"' || return 0
        sleep 0.1
    done
}

wait_new_button() {
    for _ in $(seq 50); do
        curl -s $W/scan/status | python3 -c '
import json,sys
s=json.load(sys.stdin)
sys.exit(not (s["id"] and s["id"] != sys.argv[1] and s["source"] == "button"))' "$1" && return 0
        sleep 0.1
    done
    return 1
}

check() {   # driver, expected scanner ("imageCLASS", "PIXMA" or "none"), all paths (1) or one
    local log=$OUT/$1.log
    PRINTER_DRIVER=$1 FAKE_SCANNER_KBPS=0 host/airprint_host $PORT 2>"$log" &
    local pid=$!
    for _ in $(seq 50); do curl -s -o /dev/null $W/api/state && break; sleep 0.1; done
    echo "$1 ($2):"
    if [ "$2" = none ]; then
        curl -s $W/api/state | grep -q '"scanner":false' && say "  no scanner: ok" || say "  FAIL: scanner reported"
    else
        curl -s -m 120 -o $OUT/s.jpg "$W/scan/stream.jpg?dpi=150&paper=a4"
        judge $OUT/s.jpg $2 "stream A4 150 dpi" $log
        if [ "${3:-}" = 1 ]; then
            curl -s -m 120 -o $OUT/s.jpg "$W/scan/stream.jpg?dpi=600&paper=card"
            judge $OUT/s.jpg $2 "stream card 600 dpi" $log
            curl -s -o /dev/null -X POST "$W/scan/last?dpi=300&paper=letter"
            sleep 0.2; wait_done
            curl -s -o $OUT/s.jpg $W/scan/last.jpg
            judge $OUT/s.jpg $2 "kept Letter 300 dpi" $log
            local previous; previous=$(curl -s $W/scan/status | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')
            kill -USR1 $pid      # the button: a kept 150 dpi A4 scan
            if wait_new_button "$previous"; then
                wait_done
                curl -s -o $OUT/s.jpg $W/scan/last.jpg
                judge $OUT/s.jpg $2 "button" $log "$previous"
            else
                say "  button: FAIL: no new button scan"
            fi
            # eSCL: a 5 x 4 in region at 300 dpi
            local set='<scan:ScanSettings xmlns:scan="http://schemas.hp.com/imaging/escl/2011/05/03" xmlns:pwg="http://www.pwg.org/schemas/2010/12/sm"><pwg:Version>2.0</pwg:Version><pwg:ScanRegions><pwg:ScanRegion><pwg:ContentRegionUnits>escl:ThreeHundredthsOfInches</pwg:ContentRegionUnits><pwg:Width>1500</pwg:Width><pwg:Height>1200</pwg:Height><pwg:XOffset>300</pwg:XOffset><pwg:YOffset>150</pwg:YOffset></pwg:ScanRegion></pwg:ScanRegions><pwg:DocumentFormat>image/jpeg</pwg:DocumentFormat><pwg:InputSource>Platen</pwg:InputSource><scan:ColorMode>RGB24</scan:ColorMode><scan:XResolution>300</scan:XResolution><scan:YResolution>300</scan:YResolution></scan:ScanSettings>'
            local job; job=$(curl -s -D - -o /dev/null --data "$set" $W/eSCL/ScanJobs | tr -d '\r' | awk '/^Location:/ {print $2}')
            curl -s -m 120 -o $OUT/s.jpg $W$job/NextDocument
            judge $OUT/s.jpg $2 "eSCL region 300 dpi" $log
        fi
    fi
    kill $pid; wait $pid 2>/dev/null
}
check canon-mf220 imageCLASS 1
check canon-mg2500 PIXMA
check canon-p200 none
rm -rf "$OUT"
exit $fail
