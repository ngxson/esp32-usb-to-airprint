// The printer drivers built into the firmware. A new driver: add its
// declaration and its entry here, its source to CMakeLists.txt (and to
// host/Makefile for the host build). The firmware picks the one that matches
// the attached printer, else CONFIG_PRINTER_DRIVER.
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "printer_driver.h"

extern const printer_driver_t canon_mg2500_driver, canon_mg2400_driver, canon_p200_driver, canon_mg2900_driver,
    canon_mg3000_driver, canon_e470_driver, canon_e460_driver, canon_ip2800_driver, canon_e3100_driver,
    canon_ts3100_driver, canon_mp230_driver, canon_ip2700_driver, canon_mp250_driver, canon_mp270_driver,
    canon_mp280_driver, canon_mp490_driver, canon_mp493_driver, canon_mp495_driver, canon_mx340_driver,
    canon_mx350_driver, canon_mx360_driver, canon_mx410_driver, canon_mx420_driver;

const printer_driver_t *const printer_drivers[] = {
    &canon_mg2500_driver, &canon_mg2400_driver, &canon_p200_driver, &canon_mg2900_driver, &canon_mg3000_driver,
    &canon_e470_driver, &canon_e460_driver, &canon_ip2800_driver, &canon_e3100_driver, &canon_ts3100_driver,
    &canon_mp230_driver, &canon_ip2700_driver, &canon_mp250_driver, &canon_mp270_driver, &canon_mp280_driver,
    &canon_mp490_driver, &canon_mp493_driver, &canon_mp495_driver, &canon_mx340_driver, &canon_mx350_driver,
    &canon_mx360_driver, &canon_mx410_driver, &canon_mx420_driver,
    NULL,
};

const printer_driver_t *printer_driver_find(const char *id)
{
    for (int i = 0; printer_drivers[i]; i++)
        if (id && !strcmp(printer_drivers[i]->id, id))
            return printer_drivers[i];
    return NULL;
}

// The value of a Device ID key ("MDL" or its long form "MODEL") into out
static void device_id_field(const char *id, const char *key, const char *long_key, char *out, size_t n)
{
    out[0] = 0;
    for (const char *p = id; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL) {
        while (*p == ' ')
            p++;
        size_t k = strlen(key), lk = strlen(long_key);
        const char *v = !strncmp(p, key, k) && p[k] == ':'            ? p + k + 1
                        : !strncmp(p, long_key, lk) && p[lk] == ':' ? p + lk + 1
                                                                      : NULL;
        if (!v)
            continue;
        size_t len = strcspn(v, ";");
        snprintf(out, n, "%.*s", (int)len, v);
        return;
    }
}

// Model names compared without case and without a trailing " series"
static bool same_model(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la > 7 && !strcasecmp(a + la - 7, " series"))
        la -= 7;
    if (lb > 7 && !strcasecmp(b + lb - 7, " series"))
        lb -= 7;
    return la && la == lb && !strncasecmp(a, b, la);
}

const printer_driver_t *printer_driver_match(uint16_t vid, uint16_t pid, const char *device_id)
{
    for (int i = 0; printer_drivers[i]; i++)
        if (printer_drivers[i]->usb_vid && printer_drivers[i]->usb_vid == vid && printer_drivers[i]->usb_pid == pid)
            return printer_drivers[i];
    char mfg[32], mdl[64];
    device_id_field(device_id, "MFG", "MANUFACTURER", mfg, sizeof(mfg));
    device_id_field(device_id, "MDL", "MODEL", mdl, sizeof(mdl));
    for (int i = 0; printer_drivers[i] && mdl[0]; i++) {
        const printer_driver_t *d = printer_drivers[i];
        if (d->usb_mdl && same_model(d->usb_mdl, mdl) && (!mfg[0] || !d->usb_mfg || !strcasecmp(d->usb_mfg, mfg)))
            return d;
    }
    return NULL;
}
