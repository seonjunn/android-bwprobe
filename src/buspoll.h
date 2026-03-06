/*
 * buspoll.h — bus_access PMU polling thread for bandwidth timeseries
 *
 * Polls the bus_access (PMU 0x0019) counter every 4 ms and writes
 * instantaneous bandwidth estimates to a CSV trace file.
 *
 * Usage:
 *   int bus_fd = open_bus_access();   // from pmu.h
 *   BusPoll bp = { .running=1, .bus_fd=bus_fd,
 *                  .trace_fp=fopen(..., "w"),
 *                  .t0=now_s(), .cpu_pin=6 };
 *   pthread_t tid;
 *   pthread_create(&tid, NULL, buspoll_thread, &bp);
 *   ... run workload ...
 *   bp.running = 0;
 *   pthread_join(tid, NULL);
 *
 * The PMU fd must remain open for the lifetime of the thread.
 * trace_fp is written only by buspoll_thread (no lock needed).
 * Set cpu_pin < 0 to skip CPU pinning.
 */

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include "cpu.h"
#include "timer.h"

typedef struct {
    volatile int  running;
    int           bus_fd;
    FILE         *trace_fp;   /* sole writer; no lock needed */
    double        t0;         /* experiment reference time for relative timestamps */
    int           cpu_pin;    /* CPU to pin to; < 0 to skip pinning */
} BusPoll;

/*
 * Background thread: enables bus_access counter, polls every 4 ms,
 * writes "t_s,bus_access_MBs" rows to trace_fp.
 */
static void *buspoll_thread(void *arg)
{
    BusPoll *bp = arg;
    if (bp->cpu_pin >= 0) pin_to_cpu(bp->cpu_pin);

    ioctl(bp->bus_fd, PERF_EVENT_IOC_ENABLE, 0);

    uint64_t prev = 0;
    read(bp->bus_fd, &prev, sizeof(prev));
    double t_prev = now_s();

    while (bp->running) {
        usleep(4000);
        uint64_t curr = 0;
        read(bp->bus_fd, &curr, sizeof(curr));
        double t_now = now_s();
        double dt = t_now - t_prev;
        if (dt > 0 && bp->trace_fp) {
            double bw = (double)(curr - prev) * CACHE_LINE / dt / 1e6;
            fprintf(bp->trace_fp, "%.6f,%.1f\n", t_now - bp->t0, bw);
        }
        prev   = curr;
        t_prev = t_now;
    }

    ioctl(bp->bus_fd, PERF_EVENT_IOC_DISABLE, 0);
    if (bp->trace_fp) fflush(bp->trace_fp);
    return NULL;
}
