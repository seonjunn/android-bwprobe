/*
 * timer.h — Wall-clock and ARM virtual counter timing
 *
 * now_s()       : CLOCK_MONOTONIC wall clock (double, seconds)
 * rdcnt()       : CNTVCT_EL0 virtual counter (~38.4 MHz on Snapdragon; no syscall)
 * cntfreq()     : CNTFRQ_EL0 counter frequency
 * cntdelta_ns() : convert counter delta to nanoseconds
 */

#pragma once
#include <time.h>
#include <stdint.h>

static inline double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static inline uint64_t rdcnt(void)
{
    uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline uint64_t cntfreq(void)
{
    uint64_t v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline double cntdelta_ns(uint64_t t0, uint64_t t1)
{
    return (double)(t1 - t0) * 1e9 / (double)cntfreq();
}
