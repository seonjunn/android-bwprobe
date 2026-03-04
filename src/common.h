/*
 * common.h — Shared helpers for Android PMU measurement tools
 *
 * Included by step1_probe.c, step6_cpu_calibrate.c, step6_landscape.c.
 */

#pragma once

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/perf_event.h>
#include <math.h>

#define ARMV8_PMU_TYPE   10      /* /sys/bus/event_source/devices/armv8_pmuv3/type */
#define CACHE_LINE       64UL

/* -------------------------------------------------------------------------
 * Timing
 * ---------------------------------------------------------------------- */
static inline double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* -------------------------------------------------------------------------
 * perf_event_open wrapper (not in standard libc headers on Android NDK)
 * ---------------------------------------------------------------------- */
static inline long perf_event_open(struct perf_event_attr *attr,
                                   pid_t pid, int cpu,
                                   int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

/* -------------------------------------------------------------------------
 * CPU affinity — pin calling thread to a single core
 * ---------------------------------------------------------------------- */
static inline void pin_to_cpu(int cpu)
{
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    sched_setaffinity(0, sizeof(s), &s);
}

/* -------------------------------------------------------------------------
 * ARM virtual counter — ~1 cycle read, no syscall.
 * cntfrq_el0 is typically 38.4 MHz (26 ns resolution) on Snapdragon.
 * ---------------------------------------------------------------------- */
static inline uint64_t rdcnt(void) {
    uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
static inline uint64_t cntfreq(void) {
    uint64_t v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}
/* Convert counter delta to nanoseconds */
static inline double cntdelta_ns(uint64_t t0, uint64_t t1) {
    return (double)(t1 - t0) * 1e9 / (double)cntfreq();
}

/* -------------------------------------------------------------------------
 * LLCC eviction scrub
 *
 * Stream through a buffer larger than the LLCC (12 MB on Oryon V2) to
 * push out any residual working-set from previous measurements.
 * Call with flush_bytes > LLCC size (use 24 MB) before each measurement.
 * ---------------------------------------------------------------------- */
static void flush_caches(size_t flush_bytes) {
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

/* -------------------------------------------------------------------------
 * Fixed-hop-count pointer chase — returns nanoseconds per hop.
 *
 * Uses CNTVCT_EL0 (no syscall, no now_s() overhead) to time exactly
 * nhops pointer dereferences.  nhops should be large enough that total
 * elapsed >> CNTVCT resolution (26 ns); 1M hops works for all regimes.
 * The chain is cyclic, so nhops may exceed chain length.
 * ---------------------------------------------------------------------- */
static inline double chase_latency_ns(uintptr_t *start, uint64_t nhops) {
    volatile uintptr_t *p = (volatile uintptr_t *)start;
    uint64_t t0 = rdcnt();
    for (uint64_t i = 0; i < nhops / 8; i++) {
        p=(volatile uintptr_t*)(*p); p=(volatile uintptr_t*)(*p);
        p=(volatile uintptr_t*)(*p); p=(volatile uintptr_t*)(*p);
        p=(volatile uintptr_t*)(*p); p=(volatile uintptr_t*)(*p);
        p=(volatile uintptr_t*)(*p); p=(volatile uintptr_t*)(*p);
    }
    uint64_t t1 = rdcnt();
    (void)p;
    return cntdelta_ns(t0, t1) / (double)nhops;
}

/* -------------------------------------------------------------------------
 * Statistics helpers — shared by bwprobe.c and matvec.c
 * ---------------------------------------------------------------------- */
static inline double stat_mean(const double *v, int n) {
    double s = 0.0;
    for (int i = 0; i < n; i++) s += v[i];
    return s / n;
}
static inline double stat_sd(const double *v, int n, double m) {
    if (n < 2) return 0.0;
    double s = 0.0;
    for (int i = 0; i < n; i++) { double d = v[i] - m; s += d * d; }
    return sqrt(s / (n - 1));
}

/* -------------------------------------------------------------------------
 * Random pointer-chase buffer
 *
 * Builds a cyclic chain through a buffer of size `sz` bytes (must be a
 * multiple of CACHE_LINE). Each hop traverses exactly one cache line,
 * defeating the hardware prefetcher and forcing DRAM access at large WS.
 * ---------------------------------------------------------------------- */
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
        buf[perm[i] * 8] = (uintptr_t)&buf[perm[i+1] * 8];
    buf[perm[n-1] * 8] = (uintptr_t)&buf[perm[0] * 8];
    free(perm);
    return buf;
}

/* Touch every page to fault in physical memory before measuring */
static inline void prefault(void *buf, size_t sz)
{
    volatile char *p = buf;
    for (size_t i = 0; i < sz; i += 4096) p[i] = 0;
}

/* Run pointer-chase for `secs` seconds, return hop count.
 * NOTE: the 8 dereferences per iteration are *sequentially dependent* —
 * each p load depends on the previous result, so there is no MLP.
 * The now_s() call (~20–22 ns overhead every 8 hops) dominates loop time
 * at cache-resident WS sizes, masking true memory latency. */
static inline uint64_t run_chase(uintptr_t *start, double secs)
{
    volatile uintptr_t *p = (volatile uintptr_t *)start;
    double t0 = now_s();
    uint64_t h = 0;
    while (now_s() - t0 < secs) {
        p=(volatile uintptr_t*)(*p); p=(volatile uintptr_t*)(*p);
        p=(volatile uintptr_t*)(*p); p=(volatile uintptr_t*)(*p);
        p=(volatile uintptr_t*)(*p); p=(volatile uintptr_t*)(*p);
        p=(volatile uintptr_t*)(*p); p=(volatile uintptr_t*)(*p);
        h += 8;
    }
    return h;
}
