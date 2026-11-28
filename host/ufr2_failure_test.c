// Fault injection for the UFR encoder's band allocation and empty payloads.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static int allocation_calls;
static void *fail_realloc(void *ptr, size_t len)
{
    (void)ptr;
    (void)len;
    allocation_calls++;
    return NULL;
}
#define realloc fail_realloc
#include "../components/printers/canon_ufr2.c"
#undef realloc

static int empty_writes;
static int count_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (!data || !len)
        empty_writes++;
    return 0;
}

int main(void)
{
    job_t failed = {0};
    band_byte(&failed, 0x12);
    band_byte(&failed, 0x34);
    if (!failed.err || allocation_calls != 1 || failed.band_len) {
        fprintf(stderr, "failed band allocation retried: %d allocations\n", allocation_calls);
        return 1;
    }
    // A failed stripe must not touch any source line or attempt a flush.
    failed.n = 1;
    failed.lb = 1;
    encode_stripe(&failed);
    if (allocation_calls != 1)
        return 1;
    job_t empty = {.io = {.write = count_write}};
    cpca(&empty, 0x19, 0, NULL, 0);
    if (empty_writes || empty.bytes != 20) {
        fprintf(stderr, "empty CPCA payload reached write callback\n");
        return 1;
    }
    puts("ufr2 failure tests: ok");
    return 0;
}
