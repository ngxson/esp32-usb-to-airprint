#!/bin/bash
# Builds Gutenprint 5.3.5 (core library only) and renders the reference
# streams that tools/compare_gutenprint.py checks against: for the MG2500
# series build/ref300.prn, ref600.prn, ref300_letter.prn, ref300_2pages.prn,
# and the same set for a Gutenprint model of each other canon_bjl.h stream
# variant: build/<variant>/ref*.prn.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build && cd build
V=5.3.5
if [ ! -d gutenprint-$V ]; then
    curl -sSL -o gp.tar.xz "https://downloads.sourceforge.net/project/gimp-print/gutenprint-5.3/$V/gutenprint-$V.tar.xz"
    tar xf gp.tar.xz
fi
GP=$PWD/gutenprint-$V
if [ ! -f $GP/src/main/.libs/libgutenprint.a ]; then
    (cd $GP && ./configure --disable-nls --without-gimp2 --without-cups --disable-libgutenprintui2 \
        --without-doc --disable-escputil --disable-test --disable-samples --disable-shared --enable-static \
        >/dev/null 2>&1 && make -C src/main -j8 >/dev/null 2>&1 && make -C src/xml -j8 >/dev/null 2>&1)
fi
cc -O1 -I$GP/include -I$GP ../gp_ref.c $GP/src/main/.libs/libgutenprint.a -lm -o gp_ref
STP_DATA_PATH=$GP/src/xml ./gp_ref 300x300dpi ref300.prn
STP_DATA_PATH=$GP/src/xml ./gp_ref 600x600dpi ref600.prn
STP_DATA_PATH=$GP/src/xml ./gp_ref 300x300dpi ref300_letter.prn Letter
STP_DATA_PATH=$GP/src/xml ./gp_ref 300x300dpi ref300_2pages.prn A4 2
ls -la ref*.prn
# canon_bjl.h model -> a Gutenprint driver using that stream
for m in MG2900:bjc-MG2900-series iP2800:bjc-iP2800-series E3100:bjc-E3100-series MP230:bjc-MULTIPASS-MP230 \
         iP2700:bjc-PIXMA-iP2700; do
    mkdir -p ${m%%:*}
    export GP_DRIVER=${m#*:}
    STP_DATA_PATH=$GP/src/xml ./gp_ref 300x300dpi ${m%%:*}/ref300.prn 2>/dev/null
    STP_DATA_PATH=$GP/src/xml ./gp_ref 600x600dpi ${m%%:*}/ref600.prn 2>/dev/null
    STP_DATA_PATH=$GP/src/xml ./gp_ref 300x300dpi ${m%%:*}/ref300_letter.prn Letter 2>/dev/null
    STP_DATA_PATH=$GP/src/xml ./gp_ref 300x300dpi ${m%%:*}/ref300_2pages.prn A4 2 2>/dev/null
    unset GP_DRIVER
done
ls -la */ref*.prn
