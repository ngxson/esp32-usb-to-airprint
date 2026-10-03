#!/bin/bash
# Everything that can be verified without hardware.
#   tools/run_tests.sh               host unit tests + test page decode
#   GUTENPRINT=1 tools/run_tests.sh  + byte-for-byte comparison with Gutenprint
#   QEMU_XTENSA=... tools/run_tests.sh  + firmware dry run on emulated ESP32-S3
#   AIRPRINT=1 tools/run_tests.sh    + AirPrint server on this Mac: IPP conformance
#                                      (ipptool) and printing URF / PWG documents
#                                      rendered by macOS's own filters
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=host/out
mkdir -p $OUT
OUT=$(cd $OUT && pwd)

echo "== host unit tests"
make -C host -s
host/bjl_host selftest
host/scan_host selftest 2>/dev/null | tail -1

echo "== scans from the emulated scanner (JPEGs in $OUT/)"
for s in "150 a4" "300 a4" "300 letter" "600 letter"; do
    set -- $s
    host/scan_host scan $1 $2 $OUT/scan$1-$2.jpg $OUT/scan$1-$2.ppm 2>/dev/null | sed 's/, 0 protocol violations//'
    python3 tools/check_jpeg.py $OUT/scan$1-$2.jpg --ref $OUT/scan$1-$2.ppm --dpi $1 --min-psnr 38
    rm -f $OUT/scan$1-$2.ppm
done

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
    ipptool -t $URI tools/testdata/cancel-active-job.test 2>&1 | grep -E "PASS|FAIL|EXPECTED" || true
    wait $CLIENT || true
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
    grep -h "protocol violations" host/out/airprint-scan.log | sort | uniq -c | sed 's/^ */  scanner closed: /'
    trap - EXIT
fi

if [ -n "${QEMU_XTENSA:-}" ]; then
    echo "== firmware dry run in QEMU"
    tools/qemu_selftest.sh
fi

echo "== firmware build"
pio run -e esp32s3 | grep -E "RAM:|Flash:|SUCCESS|FAILED"
