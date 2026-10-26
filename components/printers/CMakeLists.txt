# Printer drivers (printer_driver.h): one source file per printer model.
idf_component_register(SRCS "printers.c" "canon_mg2500.c"
                       REQUIRES airprint canon_bjl)
