/*
 * cache.h — Cache eviction and memory prefault helpers
 *
 * flush_caches(n) : stream through n bytes to evict LLCC residue
 * prefault(p, n)  : touch every page to fault in physical memory
 */

#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

/*
 * Stream through a buffer larger than the LLCC to evict any residual
 * working-set from prior measurements.  The static buffer is allocated
 * once and reused; pass flush_bytes > LLCC size (use 24 MB for Oryon V2).
 */
static void flush_caches(size_t flush_bytes)
{
    static void  *scrub    = NULL;
    static size_t scrub_sz = 0;
    if (!scrub || scrub_sz < flush_bytes) {
        free(scrub);
        scrub = aligned_alloc(CACHE_LINE, flush_bytes);
        if (!scrub) return;
        scrub_sz = flush_bytes;
        memset(scrub, 0x55, flush_bytes);
    }
    volatile uint64_t sink = 0;
    volatile uint64_t *p   = (volatile uint64_t *)scrub;
    for (size_t i = 0; i < flush_bytes / sizeof(uint64_t); i += 8)
        sink ^= p[i];
    (void)sink;
}

/* Touch every page to fault in physical memory before measuring */
static inline void prefault(void *buf, size_t sz)
{
    volatile char *p = buf;
    for (size_t i = 0; i < sz; i += 4096) p[i] = 0;
}
