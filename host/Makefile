# Native build of the portable printing code for testing on the development machine.
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined
# same float evaluation as the firmware build, so the test page CRC matches
CFLAGS += -ffp-contract=off
BJL = ../components/canon_bjl/canon_bjl.c ../components/canon_bjl/testpage.c ../components/canon_bjl/bjl_render.c
AIRPRINT = ../components/airprint/airprint.c ../components/airprint/ipp.c ../components/airprint/raster.c
SCAN_CORE = ../components/scanner/pixma_scan.c ../components/scanner/jpeg_enc.c ../components/scanner/fake_scanner.c
SCANNER = $(SCAN_CORE) ../components/scanner/shared_buf.c ../components/scanner/scan_service.c
INC = -I../components/canon_bjl/include -I../components/airprint/include -I../components/airprint \
      -I../components/scanner/include -I../components/scanner

all: bjl_host airprint_host scan_host

bjl_host: bjl_host.c $(BJL) ../components/canon_bjl/include/*.h
	$(CC) $(CFLAGS) $(INC) bjl_host.c $(BJL) -lm -o $@

airprint_host: airprint_host.c $(BJL) $(AIRPRINT) $(SCANNER) ../components/airprint/include/*.h ../components/airprint/*.h \
               ../components/scanner/include/*.h ../components/scanner/*.h
	$(CC) $(CFLAGS) $(INC) airprint_host.c $(BJL) $(AIRPRINT) $(SCANNER) -lm -lpthread -o $@

scan_host: scan_host.c $(SCAN_CORE) ../components/scanner/include/*.h
	$(CC) $(CFLAGS) $(INC) scan_host.c $(SCAN_CORE) -lm -o $@

clean:
	rm -rf bjl_host airprint_host scan_host out *.prn *.png *.raw
