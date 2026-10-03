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
    trap - EXIT
fi

if [ -n "${QEMU_XTENSA:-}" ]; then
    echo "== firmware dry run in QEMU"
    tools/qemu_selftest.sh
fi

echo "== firmware build"
pio run -e esp32s3 | grep -E "RAM:|Flash:|SUCCESS|FAILED"
