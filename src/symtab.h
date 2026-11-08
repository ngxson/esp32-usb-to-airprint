// Function names of a firmware build, from the table tools/gen_symbols.py
// appends after the app image in firmware.bin (the image itself is unchanged).
// Each OTA slot holds the table of its own build, so a crash of either build
// can be named without its ELF.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"

typedef struct {
    esp_partition_mmap_handle_t handle;
    const uint32_t *addrs;
    const uint16_t *sizes;
    const char *names;
    uint32_t count;
} symtab_t;

// Where the table of the image in `part` is (offset in the partition, size).
// ESP_ERR_NOT_FOUND: no image, no table, or the table of another build.
esp_err_t symtab_locate(const esp_partition_t *part, size_t *offset, size_t *size);
// Maps the table (checked: CRC, build); symtab_close() unmaps it.
esp_err_t symtab_open(const esp_partition_t *part, symtab_t *t);
void symtab_close(symtab_t *t);
// "name+0x1c" for a code address, "?" if no function holds it; returns out.
const char *symtab_name(const symtab_t *t, uint32_t addr, char *out, size_t n);
// The app partition whose build's ELF SHA-256 starts with the hex string
// (as in a core dump), or NULL. NULL or "" means the running one.
const esp_partition_t *symtab_find_app(const char *sha_hex);
