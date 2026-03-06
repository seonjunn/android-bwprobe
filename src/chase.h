/*
 * chase.h — Random pointer-chase workload
 *
 * build_chase(sz)            : allocate a Fisher-Yates shuffled cyclic chain
 * run_chase(start, secs)     : chase for `secs` seconds, return hop count
 * chase_latency_ns(start, n) : time exactly n hops via CNTVCT_EL0, return ns/hop
 *
 * The chain is one pointer per cache line (8 bytes at offset 0 in each 64 B line).
 * Each hop is sequentially dependent on the previous result — no MLP, defeats
 * the hardware prefetcher, forces true memory-latency measurement.
 */

#pragma once
#include <stdint.h>
#include <stdlib.h>
#include "cpu.h"
#include "timer.h"

/*
 * Build a cyclic pointer chain through sz bytes (must be a multiple of CACHE_LINE).
 * Fisher-Yates shuffle ensures each hop visits a random cache line.
 * Returns the chain start pointer (buf[0] always starts the chain).
 */
static inline uintptr_t *build_chase(size_t sz)
{
    size_t n = sz / CACHE_LINE;
    uintptr_t *buf = aligned_alloc(CACHE_LINE, n * CACHE_LINE);
    if (!buf) return NULL;
    size_t *perm = malloc(n * sizeof(*perm));
    if (!perm) { free(buf); return NULL; }

    for (size_t i = 0; i < n; i++) perm[i] = i;
    unsigned seed = 0xdeadbeef;
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = (size_t)rand_r(&seed) % (i + 1);
        size_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    for (size_t i = 0; i < n - 1; i++)
        buf[perm[i] * 8] = (uintptr_t)&buf[perm[i + 1] * 8];
    buf[perm[n - 1] * 8] = (uintptr_t)&buf[perm[0] * 8];
    free(perm);
    return buf;
}

/*
 * Chase the pointer chain for `secs` seconds.
 * Returns the total hop count.  Uses now_s() (CLOCK_MONOTONIC) for the time
 * check; the syscall overhead (~22 ns every 8 hops) dominates at cache-resident
 * WS sizes but is negligible at DRAM-resident WS.
 */
static inline uint64_t run_chase(uintptr_t *start, double secs)
{
    volatile uintptr_t *p = (volatile uintptr_t *)start;
    double t0 = now_s();
    uint64_t h = 0;
    while (now_s() - t0 < secs) {
        p = (volatile uintptr_t *)(*p); p = (volatile uintptr_t *)(*p);
        p = (volatile uintptr_t *)(*p); p = (volatile uintptr_t *)(*p);
        p = (volatile uintptr_t *)(*p); p = (volatile uintptr_t *)(*p);
        p = (volatile uintptr_t *)(*p); p = (volatile uintptr_t *)(*p);
        h += 8;
    }
    (void)p;
    return h;
}

/*
 * Time exactly nhops pointer dereferences using CNTVCT_EL0 (no syscall).
 * nhops should be large (e.g. 1M) so total elapsed >> counter resolution (~26 ns).
 * Returns nanoseconds per hop.
 */
static inline double chase_latency_ns(uintptr_t *start, uint64_t nhops)
{
    volatile uintptr_t *p = (volatile uintptr_t *)start;
    uint64_t t0 = rdcnt();
    for (uint64_t i = 0; i < nhops / 8; i++) {
        p = (volatile uintptr_t *)(*p); p = (volatile uintptr_t *)(*p);
        p = (volatile uintptr_t *)(*p); p = (volatile uintptr_t *)(*p);
        p = (volatile uintptr_t *)(*p); p = (volatile uintptr_t *)(*p);
        p = (volatile uintptr_t *)(*p); p = (volatile uintptr_t *)(*p);
    }
    uint64_t t1 = rdcnt();
    (void)p;
    return cntdelta_ns(t0, t1) / (double)nhops;
}
