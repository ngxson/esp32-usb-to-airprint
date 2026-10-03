#!/bin/bash
# Builds Gutenprint 5.3.5 (core library only) and renders the reference
# streams for the MG2500 series that tools/compare_gutenprint.py checks
# against: build/ref300.prn and build/ref600.prn.
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
