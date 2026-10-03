idf_component_register(SRCS "airprint.c" "ipp.c" "raster.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES canon_bjl pthread lwip log)
