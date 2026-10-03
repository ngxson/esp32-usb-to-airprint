# Native build of the portable printing code for testing on the development machine.
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined
# same float evaluation as the firmware build, so the test page CRC matches
CFLAGS += -ffp-contract=off
BJL = ../components/canon_bjl/canon_bjl.c ../components/canon_bjl/testpage.c ../components/canon_bjl/bjl_render.c
AIRPRINT = ../components/airprint/airprint.c ../components/airprint/ipp.c ../components/airprint/raster.c
INC = -I../components/canon_bjl/include -I../components/airprint/include -I../components/airprint

all: bjl_host airprint_host

bjl_host: bjl_host.c $(BJL) ../components/canon_bjl/include/*.h
	$(CC) $(CFLAGS) $(INC) bjl_host.c $(BJL) -lm -o $@

airprint_host: airprint_host.c $(BJL) $(AIRPRINT) ../components/airprint/include/*.h ../components/airprint/*.h
	$(CC) $(CFLAGS) $(INC) airprint_host.c $(BJL) $(AIRPRINT) -lm -lpthread -o $@

clean:
	rm -rf bjl_host airprint_host out *.prn *.png *.raw
