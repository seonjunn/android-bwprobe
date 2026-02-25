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

/* Run pointer-chase for `secs` seconds, return hop count */
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
