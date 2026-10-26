#!/bin/bash
# Everything that can be verified without hardware.
#   tools/run_tests.sh               host unit tests + test page decode
#   GUTENPRINT=1 tools/run_tests.sh  + byte-for-byte comparison with Gutenprint
#   QEMU_XTENSA=... tools/run_tests.sh  + firmware dry run on emulated ESP32-S3
#   AIRPRINT=1 tools/run_tests.sh    + AirPrint server on this Mac: IPP conformance
#                                      (ipptool) and printing URF / PWG documents
#                                      rendered by macOS's own filters, the web
#                                      UI's live state and the button scan
#   WEB=1 tools/run_tests.sh         + the web UI in headless Chrome against the
#                                      host server (print, scan, sync, Process)
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=host/out
mkdir -p $OUT
OUT=$(cd $OUT && pwd)

echo "== host unit tests"
make -C host -s
host/bjl_host selftest
host/scan_host selftest 2>/dev/null | tail -1
host/oled_host selftest

echo "== status screen (all screens in $OUT/oled.png)"
rm -f $OUT/oled-*.pbm
host/oled_host render $OUT
python3 tools/oled_preview.py $OUT $OUT/oled.png

echo "== scans from the emulated scanner (JPEGs in $OUT/)"
for s in "150 a4" "300 a4" "300 letter" "600 letter"; do
    set -- $s
    host/scan_host scan $1 $2 $OUT/scan$1-$2.jpg $OUT/scan$1-$2.ppm 2>/dev/null | sed 's/, 0 protocol violations//'
    python3 tools/check_jpeg.py $OUT/scan$1-$2.jpg --ref $OUT/scan$1-$2.ppm --dpi $1 --min-psnr 38
    rm -f $OUT/scan$1-$2.ppm
done

echo "== web UI unit tests (web/tests)"
if [ -d web/node_modules ]; then
    (cd web && node --test tests/*.test.ts 2>&1 | grep -E "^ℹ (pass|fail)|^✖" | tr '\n' ' '; echo)
    # the browser's URF, decoded by the independent Python decoder
    python3 - <<'PY'
import sys, numpy as np
sys.path.insert(0, 'tools')
from raster_to_png import pages
src = np.fromfile('host/out/web-color.rgba', dtype=np.uint8).reshape(500, 333, 4)[:, :, :3]
(img, w, h, bpp, dpi), = list(pages(open('host/out/web-color.urf', 'rb').read()))
print('URF from the browser encoder, decoded by raster_to_png.py:',
      'identical' if (img.reshape(h, w, 3) == src).all() else 'DIFFERENT')
PY
else
    echo "skipped: no web/node_modules (cd web && npm ci)"
fi

echo "== test page streams (decoded previews in $OUT/)"
for dpi in 300 600; do
    host/bjl_host testpage $dpi $OUT/testpage$dpi.prn
    python3 tools/bjl_decode.py $OUT/testpage$dpi.prn --png $OUT/testpage$dpi.png --quiet | tail -6
done

if [ -n "${GUTENPRINT:-}" ]; then
    echo "== comparison with Gutenprint's MG2500 driver"
    [ -f tools/gutenprint_ref/build/ref600.prn ] || tools/gutenprint_ref/build_ref.sh
    R=tools/gutenprint_ref/build
    [ -f $R/ref300_2pages.prn ] || tools/gutenprint_ref/build_ref.sh
    python3 tools/compare_gutenprint.py $R/ref300.prn 300
    python3 tools/compare_gutenprint.py $R/ref600.prn 600
    python3 tools/compare_gutenprint.py $R/ref300_letter.prn 300 --paper letter
    python3 tools/compare_gutenprint.py $R/ref300_2pages.prn 300
fi

if [ -n "${AIRPRINT:-}" ]; then
    echo "== AirPrint server (host build)"
    PORT=8631
    URI=ipp://localhost:$PORT/ipp/print
    pkill -f "airprint_host 863[12]" 2>/dev/null || true     # stale servers from an aborted run
    PPD=tools/testdata/raster_test.ppd
    DOCS=/usr/share/cups/ipptool
    (cd host && exec ./airprint_host $PORT > out/airprint.log 2>&1) &
    SERVER=$!
    trap 'kill $SERVER 2>/dev/null' EXIT
    sleep 1
    for t in ipp-1.1.test ipp-2.0.test ipp-everywhere.test; do
        r=$(ipptool -I -t $URI $t 2>&1) || true    # exits 1 on any failed test
        echo "$t: $(grep -c '\[PASS\]' <<<"$r") pass, $(grep -c '\[FAIL\]' <<<"$r") fail"
        grep -A3 '\[FAIL\]' <<<"$r" | grep -E "FAIL|EXPECTED" || true
    done
    echo "(expected: ipp-everywhere fails only on image/jpeg, which AirPrint never uses)"
    rm -f host/out/job-*.prn
    cupsfilter -p $PPD -m image/urf $DOCS/document-a4.pdf > $OUT/doc-a4.urf 2>/dev/null
    cupsfilter -p $PPD -m image/pwg-raster $DOCS/document-a4.pdf > $OUT/doc-a4.pwg 2>/dev/null
    cupsfilter -p $PPD -m image/urf -o PageSize=Letter -o Resolution=300dpi $DOCS/onepage-letter.pdf > $OUT/letter300.urf 2>/dev/null
    cupsfilter -p $PPD -m image/urf -o ColorModel=Gray -o Resolution=300dpi $DOCS/onepage-a4.pdf > $OUT/gray300.urf 2>/dev/null
    for f in doc-a4.urf doc-a4.pwg letter300.urf gray300.urf; do
        type=image/urf; [[ $f == *.pwg ]] && type=image/pwg-raster
        echo "print $f: $(ipptool -t -f $OUT/$f -d filetype=$type $URI print-job.test 2>&1 | grep -oE 'PASS|FAIL')"
    done
    JOBS=($(ls host/out/job-*.prn | sort -t- -k2 -n))  # in print order: urf, pwg, letter, gray
    cmp -s ${JOBS[0]} ${JOBS[1]} && echo "URF and PWG of the same document: identical printer data" \
        || echo "URF and PWG outputs DIFFER"
    for j in host/out/job-*.prn; do
        python3 tools/bjl_decode.py $j --png ${j%.prn}.png --quiet | grep -E "^page|DECODE ERROR"
    done
    kill $SERVER; wait $SERVER 2>/dev/null || true
    echo "== cancel while printing (slow printer emulation)"
    PORT=8632
    URI=ipp://localhost:$PORT/ipp/print
    (cd host && AIRPRINT_HOST_SLOW_US=20000 exec ./airprint_host $PORT > out/airprint-cancel.log 2>&1) &
    SERVER=$!
    sleep 1
    ipptool -t -f $OUT/doc-a4.urf -d filetype=image/urf $URI print-job.test > $OUT/printjob.log 2>&1 &
    CLIENT=$!
    sleep 3
    curl -s -o $OUT/oled-live-1-printing.pbm http://localhost:$PORT/dev/oled.pbm
    ipptool -t $URI tools/testdata/cancel-active-job.test 2>&1 | grep -E "PASS|FAIL|EXPECTED" || true
    wait $CLIENT || true
    curl -s -o $OUT/oled-live-2-canceled.pbm http://localhost:$PORT/dev/oled.pbm
    echo "original Print-Job client: $(grep -oE 'PASS|FAIL' $OUT/printjob.log)"
    kill $SERVER; wait $SERVER 2>/dev/null || true

    echo "== scanning over HTTP (emulated scanner)"
    PORT=8633
    S=http://localhost:$PORT/scan
    # 64 KB buffer: a streamed 300 dpi page wraps the ring several times
    (cd host && FAKE_SCANNER_KBPS=0 SCAN_BUF_KB=64 exec ./airprint_host $PORT > out/airprint-scan.log 2>&1) &
    SERVER=$!
    sleep 1
    curl -s -o $OUT/http300.jpg "$S/stream.jpg?dpi=300&paper=a4"
    cmp -s $OUT/http300.jpg $OUT/scan300-a4.jpg && echo "stream 300 dpi through a 64 KB ring: identical to the direct scan" \
        || echo "stream 300 dpi DIFFERS from the direct scan"
    curl -s -X POST "$S/last" > /dev/null; sleep 1
    echo "kept scan that does not fit: $(curl -s $S/status | grep -oE '"error":"[^"]*"'), last.jpg $(curl -s -o /dev/null -w '%{http_code}' $S/last.jpg)"
    kill $SERVER; wait $SERVER 2>/dev/null || true
    echo "calibration: $(grep -c 'ready after .*(calibrated)' host/out/airprint-scan.log) scan(s) calibrated, $(grep -c 'no calibration' host/out/airprint-scan.log) without (once per server run)"
    (cd host && FAKE_SCANNER_KBPS=4000 exec ./airprint_host $PORT >> out/airprint-scan.log 2>&1) &
    SERVER=$!
    sleep 1
    curl -s -X POST "$S/last?paper=a4" > /dev/null
    echo "second scan while one runs: $(curl -s -o /dev/null -w '%{http_code}' "$S/stream.jpg?dpi=150")"
    for i in $(seq 1 60); do curl -s $S/status | grep -q '"state":"scanning"' || break; sleep 1; done
    curl -s -o $OUT/last150.jpg $S/last.jpg
    cmp -s $OUT/last150.jpg $OUT/scan150-a4.jpg && echo "kept 150 dpi scan: identical to the direct scan" \
        || echo "kept 150 dpi scan DIFFERS from the direct scan"
    curl -s -o /dev/null --max-time 3 "$S/stream.jpg?dpi=300" || true    # client leaves mid-scan
    sleep 2
    echo "client left mid-scan: $(curl -s $S/status | grep -oE '"state":"[a-z]+"')"
    curl -s -o $OUT/http150.jpg "$S/stream.jpg?dpi=150" & CLIENT=$!
    sleep 3; curl -s -X POST $S/cancel > /dev/null; wait $CLIENT || true
    echo "canceled: $(curl -s $S/status | grep -oE '"state":"[a-z]+"'), $(python3 tools/check_jpeg.py $OUT/http150.jpg --size 1240x1754 | cut -d: -f2-)"
    kill $SERVER; wait $SERVER 2>/dev/null || true

    echo "== eSCL (as macOS Image Capture uses it)"
    (cd host && FAKE_SCANNER_KBPS=0 exec ./airprint_host $PORT >> out/airprint-scan.log 2>&1) &
    SERVER=$!
    sleep 1
    E=http://localhost:$PORT/eSCL
    python3 -c "import sys, xml.etree.ElementTree as t; t.fromstring(sys.stdin.read())" < <(curl -s $E/ScannerCapabilities) \
        && echo "capabilities: well-formed XML" || echo "capabilities: BAD XML"
    # a 5 x 4 in region at 300 dpi, 1 in from the left, 0.5 in from the top
    SETTINGS='<scan:ScanSettings xmlns:scan="http://schemas.hp.com/imaging/escl/2011/05/03" xmlns:pwg="http://www.pwg.org/schemas/2010/12/sm"><pwg:Version>2.0</pwg:Version><scan:Intent>TextAndGraphic</scan:Intent><pwg:ScanRegions pwg:MustHonor="true"><pwg:ScanRegion><pwg:ContentRegionUnits>escl:ThreeHundredthsOfInches</pwg:ContentRegionUnits><pwg:Width>1500</pwg:Width><pwg:Height>1200</pwg:Height><pwg:XOffset>300</pwg:XOffset><pwg:YOffset>150</pwg:YOffset></pwg:ScanRegion></pwg:ScanRegions><pwg:DocumentFormat>image/jpeg</pwg:DocumentFormat><pwg:InputSource>Platen</pwg:InputSource><scan:ColorMode>RGB24</scan:ColorMode><scan:XResolution>300</scan:XResolution><scan:YResolution>300</scan:YResolution></scan:ScanSettings>'
    JOB=$(curl -s -D - -o /dev/null --data "$SETTINGS" $E/ScanJobs | tr -d '\r' | awk '/^Location:/ {print $2}')
    curl -s -o $OUT/escl.jpg http://localhost:$PORT$JOB/NextDocument
    host/scan_host scan 300 a4 $OUT/escl-full.jpg $OUT/escl-full.ppm >/dev/null 2>&1
    python3 -c "from PIL import Image; Image.open('$OUT/escl-full.ppm').crop((300, 150, 1800, 1350)).save('$OUT/escl-ref.ppm')"
    echo "region scan: $(python3 tools/check_jpeg.py $OUT/escl.jpg --ref $OUT/escl-ref.ppm --size 1500x1200 --dpi 300 --min-psnr 38 | cut -d: -f2-)"
    rm -f $OUT/escl-full.ppm $OUT/escl-ref.ppm $OUT/escl-full.jpg
    echo "second NextDocument: $(curl -s -o /dev/null -w '%{http_code}' http://localhost:$PORT$JOB/NextDocument) (404: one page)"
    echo "job state: $(curl -s $E/ScannerStatus | grep -o '<pwg:JobState>[A-Za-z]*' | head -1 | cut -d'>' -f2)"
    JOB=$(curl -s -D - -o /dev/null --data "$SETTINGS" $E/ScanJobs | tr -d '\r' | awk '/^Location:/ {print $2}')
    echo "cancel a queued job: $(curl -s -o /dev/null -w '%{http_code}' -X DELETE http://localhost:$PORT$JOB)," \
         "then its document: $(curl -s -o /dev/null -w '%{http_code}' http://localhost:$PORT$JOB/NextDocument)"
    echo "document feeder: $(curl -s -o /dev/null -w '%{http_code}' --data "${SETTINGS/Platen/Feeder}" $E/ScanJobs) (409)"
    kill $SERVER; wait $SERVER 2>/dev/null || true

    echo "== web UI: live state, events, the button"
    (cd host && FAKE_SCANNER_KBPS=4000 exec ./airprint_host $PORT >> out/airprint-scan.log 2>&1) &
    SERVER=$!
    sleep 1
    W=http://localhost:$PORT
    js=$(curl -s --compressed $W/ | grep -oE '/assets/index-[^"]+\.js' | head -1)
    etag=$(curl -s -D - -o /dev/null $W/ | tr -d '\r' | awk '/^ETag:/ {print $2}')
    echo "page $(curl -s -o /dev/null -w '%{http_code}' $W/), app $(curl -s --compressed -o /dev/null -w '%{http_code}' $W$js)," \
         "again with its ETag $(curl -s -o /dev/null -w '%{http_code}' -H "If-None-Match: $etag" $W/)"
    curl -s -N --max-time 60 $W/events > $OUT/events.txt &
    EVENTS=$!
    sleep 1
    kill -USR1 $SERVER      # the button: a kept 150 dpi A4 scan
    sleep 1
    ID=$(curl -s $W/scan/status | python3 -c "import json,sys; print(json.load(sys.stdin)['id'])")
    curl -s -o $OUT/oled-live-3-scanning.pbm $W/dev/oled.pbm
    FROM=0; : > $OUT/button.jpg
    for i in $(seq 1 120); do   # read it while it is scanned, as the page does
        st=$(curl -s -D $OUT/h.txt -o $OUT/piece.bin "$W/scan/last.jpg?id=$ID&from=$FROM"; tr -d '\r' < $OUT/h.txt | awk '/^X-Scan-State:/ {print $2}')
        cat $OUT/piece.bin >> $OUT/button.jpg; FROM=$(wc -c < $OUT/button.jpg | tr -d ' ')
        [ "$st" != scanning ] && break
        sleep 0.5
    done
    cmp -s $OUT/button.jpg $OUT/scan150-a4.jpg && echo "button scan read while scanned ($i reads): identical to the direct scan" \
        || echo "button scan DIFFERS from the direct scan"
    curl -s -o $OUT/oled-live-4-scanned.pbm $W/dev/oled.pbm
    sleep 2; kill $EVENTS 2>/dev/null; wait $EVENTS 2>/dev/null || true
    python3 - $OUT/events.txt <<'PY'
import json, sys
ev = [json.loads(l[6:]) for l in open(sys.argv[1]) if l.startswith('data: ')]
seen = [e['scan']['state'] for e in ev]
print('events: %d snapshots, scan %s, last %s from the %s' % (len(ev), ' -> '.join(dict.fromkeys(seen)),
      ev[-1]['scan']['last_id'] == ev[-1]['scan']['id'] and 'kept' or 'NOT kept', ev[-1]['scan']['source']))
PY
    kill $SERVER; wait $SERVER 2>/dev/null || true
    grep -h "protocol violations" host/out/airprint-scan.log | sort | uniq -c | sed 's/^ */  scanner closed: /'
    # the screen as the server's state drew it: printing, canceled, scanning, scanned
    python3 - $OUT <<'PY'
import sys
from PIL import Image
names = ['1-printing', '2-canceled', '3-scanning', '4-scanned']
imgs = [Image.open(f'{sys.argv[1]}/oled-live-{n}.pbm').convert('1') for n in names]
lit = [sum(1 for v in i.crop((0, 23, 128, 37)).getdata() if v == 0) for i in imgs]  # headline row
distinct = len({i.tobytes() for i in imgs})
print('status screen from the live state: %d of 4 screens with a headline, %d distinct' % (sum(n > 50 for n in lit), distinct))
PY
    python3 tools/oled_preview.py $OUT $OUT/oled.png > /dev/null
    trap - EXIT
fi

if [ -n "${WEB:-}" ]; then
    echo "== web UI in headless Chrome (screenshots in $OUT/ui-*.png)"
    PORT=8641
    mkdir -p $OUT/e2e
    gunzip -c /usr/share/cups/ipptool/document-a4.pdf > $OUT/e2e/document-a4.pdf
    gunzip -c /usr/share/cups/ipptool/color.jpg > $OUT/e2e/color.jpg
    pkill -f "airprint_host $PORT" 2>/dev/null || true
    rm -f host/out/job-*.prn
    (cd host && FAKE_SCANNER_KBPS=4000 exec ./airprint_host $PORT > out/airprint-web.log 2>&1) &
    SERVER=$!
    trap 'kill $SERVER 2>/dev/null' EXIT
    sleep 1
    (cd web && BASE=http://localhost:$PORT SERVER_PID=$SERVER node tests/e2e.mjs 2>&1 | grep -E "^- |^e2e|Error|rror:" | head -20)
    for j in host/out/job-*.prn; do
        python3 tools/bjl_decode.py $j --png ${j%.prn}.png --scale 4 --quiet | grep -E "^page|DECODE ERROR" | sed "s|^|  $(basename $j): |"
    done
    kill $SERVER; wait $SERVER 2>/dev/null || true
    trap - EXIT
fi

if [ -n "${QEMU_XTENSA:-}" ]; then
    echo "== firmware dry run in QEMU"
    tools/qemu_selftest.sh
fi

echo "== firmware build"
pio run -e esp32s3 | grep -E "RAM:|Flash:|SUCCESS|FAILED"
