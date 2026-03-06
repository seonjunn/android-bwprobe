/*
 * cpu.h — CPU affinity, PMU infrastructure, and platform constants
 *
 * pin_to_cpu()      : bind calling thread to a single core
 * perf_event_open() : syscall wrapper (not in Android NDK libc headers)
 * ARMV8_PMU_TYPE    : /sys/bus/event_source/devices/armv8_pmuv3/type
 * CACHE_LINE        : cache-line size in bytes (Oryon V2: 64 B)
 */

#pragma once
#include <stdint.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

#define ARMV8_PMU_TYPE   10      /* confirmed: /sys/bus/event_source/devices/armv8_pmuv3/type */
#define CACHE_LINE       64UL

/* perf_event_open is not declared in Android NDK's libc headers */
static inline long perf_event_open(struct perf_event_attr *attr,
                                   pid_t pid, int cpu,
                                   int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

/* Bind calling thread to a single logical CPU */
static inline void pin_to_cpu(int cpu)
{
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    sched_setaffinity(0, sizeof(s), &s);
}
