# Printer drivers (printer_driver.h): one source file per printer family.
idf_component_register(SRCS "printers.c" "canon_pixma.c" "kyocera_kpsl.c" "jbig/jbig.c" "jbig/jbig_ar.c"
                       PRIV_INCLUDE_DIRS "jbig"
                       REQUIRES airprint canon_bjl)
