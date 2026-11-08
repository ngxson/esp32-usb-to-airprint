// Reads the symbol table of tools/gen_symbols.py (format described there).
#include "symtab.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_rom_crc.h"

#define MAGIC "ESYM"
#define VERSION 1
#define HDR_SIZE 64

typedef struct {
    char magic[4];
    uint16_t version, hdr_size;
    uint32_t count, names_size, crc;
    uint8_t elf_sha256[32];
    uint8_t reserved[12];
} sym_hdr_t;

_Static_assert(sizeof(sym_hdr_t) == HDR_SIZE, "symbol table header");

static bool in_code(uint32_t a)
{
    return (a >= 0x40000000 && a < 0x40060000) || (a >= 0x40370000 && a < 0x403E0000) ||
           (a >= 0x42000000 && a < 0x44000000) || (a >= 0x600FE000 && a < 0x60100000);
}

static esp_err_t read_hdr(const esp_partition_t *part, size_t *offset, size_t *size, sym_hdr_t *h)
{
    if (!part || part->type != ESP_PARTITION_TYPE_APP)
        return ESP_ERR_NOT_FOUND;
    esp_partition_pos_t pos = {.offset = part->address, .size = part->size};
    esp_image_metadata_t md;
    if (esp_image_get_metadata(&pos, &md) != ESP_OK || md.image_len + HDR_SIZE > part->size)
        return ESP_ERR_NOT_FOUND;
    if (esp_partition_read(part, md.image_len, h, sizeof(*h)) != ESP_OK || memcmp(h->magic, MAGIC, 4) ||
        h->version != VERSION || h->hdr_size != HDR_SIZE)
        return ESP_ERR_NOT_FOUND;
    size_t total = HDR_SIZE + (size_t)h->count * 6 + h->names_size;
    if (h->count > 100000 || h->names_size > 4 * 1024 * 1024 || md.image_len + total > part->size)
        return ESP_ERR_NOT_FOUND;
    // the table of this build, not a leftover
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK ||
        memcmp(desc.app_elf_sha256, h->elf_sha256, sizeof(h->elf_sha256)))
        return ESP_ERR_NOT_FOUND;
    *offset = md.image_len;
    *size = total;
    return ESP_OK;
}

esp_err_t symtab_locate(const esp_partition_t *part, size_t *offset, size_t *size)
{
    sym_hdr_t h;
    return read_hdr(part, offset, size, &h);
}

esp_err_t symtab_open(const esp_partition_t *part, symtab_t *t)
{
    memset(t, 0, sizeof(*t));
    sym_hdr_t h;
    size_t offset, size;
    esp_err_t err = read_hdr(part, &offset, &size, &h);
    if (err != ESP_OK)
        return err;
    const void *p;
    err = esp_partition_mmap(part, offset, size, ESP_PARTITION_MMAP_DATA, &p, &t->handle);
    if (err != ESP_OK)
        return err;
    const uint8_t *body = (const uint8_t *)p + HDR_SIZE;
    if (esp_rom_crc32_le(0, body, (uint32_t)(size - HDR_SIZE)) != h.crc) {
        esp_partition_munmap(t->handle);
        t->handle = 0;
        return ESP_ERR_INVALID_CRC;
    }
    t->count = h.count;
    t->addrs = (const uint32_t *)body;
    t->sizes = (const uint16_t *)(body + 4 * h.count);
    t->names = (const char *)(body + 6 * h.count);
    return ESP_OK;
}

void symtab_close(symtab_t *t)
{
    if (t->handle)
        esp_partition_munmap(t->handle);
    memset(t, 0, sizeof(*t));
}

const char *symtab_name(const symtab_t *t, uint32_t addr, char *out, size_t n)
{
    snprintf(out, n, "?");
    if (!t->count || !in_code(addr))
        return out;
    uint32_t lo = 0, hi = t->count;     // first entry above addr
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (t->addrs[mid] <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return out;
    uint32_t i = lo - 1, off = addr - t->addrs[i], size = t->sizes[i];
    if ((size && size < 0xFFFF && off >= size) || (!size && off >= 0x1000))
        return out;
    const char *name = t->names;
    for (uint32_t k = 0; k < i; k++)
        name += strlen(name) + 1;
    snprintf(out, n, "%s+0x%lx", name, (unsigned long)off);
    return out;
}

const esp_partition_t *symtab_find_app(const char *sha_hex)
{
    if (!sha_hex || !sha_hex[0])
        return esp_ota_get_running_partition();
    size_t len = strlen(sha_hex);
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    const esp_partition_t *found = NULL;
    for (; it && !found; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        esp_app_desc_t desc;
        if (esp_ota_get_partition_description(p, &desc) != ESP_OK)
            continue;
        char hex[65];
        for (int i = 0; i < 32; i++)
            snprintf(hex + 2 * i, 3, "%02x", desc.app_elf_sha256[i]);
        if (len <= 64 && !strncmp(hex, sha_hex, len))
            found = p;
    }
    esp_partition_iterator_release(it);
    return found;
}
