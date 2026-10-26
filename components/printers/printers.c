// The printer drivers built into the firmware. A new driver: add its
// declaration and its entry here, its source to CMakeLists.txt (and to
// host/Makefile for the host build), then select it with CONFIG_PRINTER_DRIVER.
#include <string.h>

#include "printer_driver.h"

extern const printer_driver_t canon_mg2500_driver;

const printer_driver_t *const printer_drivers[] = {
    &canon_mg2500_driver,
    NULL,
};

const printer_driver_t *printer_driver_find(const char *id)
{
    for (int i = 0; printer_drivers[i]; i++)
        if (id && !strcmp(printer_drivers[i]->id, id))
            return printer_drivers[i];
    return NULL;
}
