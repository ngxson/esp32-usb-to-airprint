# Native build of the portable printing code for testing on the development machine.
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined
# same float evaluation as the firmware build, so the test page CRC matches
CFLAGS += -ffp-contract=off
BJL = ../components/canon_bjl/canon_bjl.c ../components/canon_bjl/testpage.c ../components/canon_bjl/bjl_render.c
AIRPRINT = ../components/airprint/airprint.c ../components/airprint/ipp.c ../components/airprint/raster.c \
           ../components/airprint/events.c ../components/airprint/printer_driver.c
PRINTERS = ../components/printers/printers.c ../components/printers/canon_pixma.c ../components/printers/canon_ufr2.c
WEBUI = ../components/webui/webui.c ../components/webui/web_assets.c
SCAN_CORE = ../components/scanner/pixma_scan.c ../components/scanner/iclass_scan.c ../components/scanner/jpeg_enc.c ../components/scanner/fake_scanner.c
SCANNER = $(SCAN_CORE) ../components/scanner/shared_buf.c ../components/scanner/scan_service.c
OLED = ../components/oled/oled_ui.c
INC = -I../components/canon_bjl/include -I../components/airprint/include -I../components/airprint \
      -I../components/scanner/include -I../components/scanner -I../components/webui/include \
      -I../components/oled/include -I../components/printers

all: bjl_host airprint_host scan_host oled_host ufr2_host airprint_jobs_test ufr2_failure_test

bjl_host: bjl_host.c $(BJL) $(PRINTERS) ../components/airprint/printer_driver.c ../components/canon_bjl/include/*.h \
          ../components/airprint/include/printer_driver.h
	$(CC) $(CFLAGS) $(INC) bjl_host.c $(BJL) $(PRINTERS) ../components/airprint/printer_driver.c -lm -o $@

airprint_host: airprint_host.c fake_iclass.c fake_iclass.h $(BJL) $(AIRPRINT) $(PRINTERS) $(SCANNER) $(WEBUI) $(OLED) ../components/airprint/include/*.h \
               ../components/airprint/*.h ../components/scanner/include/*.h ../components/webui/include/*.h \
               ../components/oled/include/*.h
	$(CC) $(CFLAGS) $(INC) airprint_host.c fake_iclass.c $(BJL) $(AIRPRINT) $(PRINTERS) $(SCANNER) $(WEBUI) $(OLED) -lm -lpthread -o $@

# the web UI (web/), built and packed when its sources changed
../components/webui/web_assets.c: FORCE
	@python3 ../tools/gen_web.py
FORCE:
.PHONY: all clean FORCE

ufr2_host: ufr2_host.c $(BJL) $(PRINTERS) ../components/airprint/printer_driver.c ../components/printers/*.h \
           ../components/airprint/include/printer_driver.h
	$(CC) $(UFR2_CFLAGS) $(INC) ufr2_host.c $(PRINTERS) $(BJL) ../components/airprint/printer_driver.c -lm -o $@
# without the sanitizers: they make its 70-megapixel round trips take half an hour
UFR2_CFLAGS = $(filter-out -fsanitize=%,$(CFLAGS))

scan_host: scan_host.c fake_iclass.c fake_iclass.h $(SCAN_CORE) ../components/scanner/include/*.h
	$(CC) $(CFLAGS) $(INC) scan_host.c fake_iclass.c $(SCAN_CORE) -lm -o $@

oled_host: oled_host.c $(OLED) ../components/oled/include/*.h ../components/airprint/include/*.h \
           ../components/scanner/include/*.h
	$(CC) $(CFLAGS) $(INC) oled_host.c $(OLED) -o $@

airprint_jobs_test: airprint_jobs_test.c $(AIRPRINT) $(PRINTERS) $(BJL) ../components/airprint/include/*.h
	$(CC) $(CFLAGS) $(INC) airprint_jobs_test.c $(filter-out ../components/airprint/airprint.c,$(AIRPRINT)) $(PRINTERS) $(BJL) -lm -lpthread -o $@

ufr2_failure_test: ufr2_failure_test.c ../components/printers/canon_ufr2.c ../components/printers/canon_ufr2.h ../components/airprint/include/printer_driver.h
	$(CC) $(CFLAGS) $(INC) ufr2_failure_test.c -o $@

clean:
	rm -rf bjl_host airprint_host scan_host oled_host ufr2_host airprint_jobs_test ufr2_failure_test out *.prn *.png
