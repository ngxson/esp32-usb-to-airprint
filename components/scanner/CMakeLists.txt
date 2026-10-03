idf_component_register(SRCS "pixma_scan.c" "jpeg_enc.c" "fake_scanner.c" "shared_buf.c" "scan_service.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES airprint pthread log)
