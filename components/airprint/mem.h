// Where the server's buffers and hot code go on the ESP32 (the same rules as
// canon_bjl.h); plain malloc elsewhere.
#pragma once

#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
// hot loops run from internal RAM (no flash cache misses)
#define AP_HOT IRAM_ATTR
// per-line working buffers: internal RAM (PSRAM is several times slower here)
#define AP_MALLOC_FAST(n) heap_caps_malloc((n), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
// sequentially written buffers: PSRAM when available
#define AP_MALLOC_BULK(n) heap_caps_malloc_prefer((n), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)
#else
#define AP_HOT
#define AP_MALLOC_FAST(n) malloc(n)
#define AP_MALLOC_BULK(n) malloc(n)
#endif
