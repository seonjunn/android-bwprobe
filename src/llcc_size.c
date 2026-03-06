/*
 * llcc_size.c — Determine effective cache capacity via pointer-chase boundary scan
 *
 * Question: Is the ~112 MB cache-resident boundary seen in bwprobe (with 24 MB flush)
 * genuine cache capacity, or an artifact of incomplete LLCC eviction?
 *
 * Method: run pointer-chase across WS 12–256 MB with three flush-buffer sizes:
 *   24 MB  — current bwprobe design (2× nominal LLCC)
 *   128 MB — 10× nominal LLCC
 *   256 MB — definitely larger than any plausible cache
 *
 * If boundary stays at ~112 MB for all flush sizes → LLCC is genuinely ~112 MB.
 * If boundary moves to smaller WS with larger flush → 24 MB flush was insufficient.
 *
 * Protocol per rep:
 *   flush_caches(flush_MB) → warmup (0.5 s chase) → lat_ns (1M CNTVCT hops)
 *   → [bwmon snap] → 3 s chase → [bwmon snap]
 *
 * CSV output: flush_MB,ws_MB,rep,chase_GBs,lat_ns,bwmon_MBs
 *
 * Build: make llcc_size
 * Run  : make ADB="adb -P 5307" runllcc
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "cpu.h"
#include "timer.h"
#include "cache.h"
#include "chase.h"
#include "stats.h"
#include "hwmon.h"

#define CPU_PIN        6
#define WARMUP_SECS    0.5
#define MEASURE_SECS   3.0
#define LATENCY_HOPS   (1ULL << 20)  /* 1M hops via CNTVCT_EL0 */
#define NREPS          5

/* Flush sizes: 24 MB (baseline), 128 MB, 256 MB */
static const size_t flush_mb[] = { 24, 128, 256 };
static const int    n_flush    = 3;

/*
 * WS sweep: fine-grained through the 12–128 MB transition zone,
 * plus 160/256 MB as confirmed-DRAM anchors.
 */
static const size_t ws_mb[] = {
    12, 16, 24, 32, 48, 64, 80, 96, 112, 128, 160, 256
};
static const int n_ws = 12;

/* ------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------ */
int main(void)
{
    pin_to_cpu(CPU_PIN);

    fprintf(stderr,
        "=================================================================\n"
        " llcc_size -- effective cache capacity determination\n"
        " flush_MB in {24, 128, 256}; ws_MB in {12..256}\n"
        " %d reps x %.1f s measure  |  cpu%d\n"
        "=================================================================\n\n",
        NREPS, MEASURE_SECS, CPU_PIN);

    Hwmon hw;
    hwmon_init(&hw, HWMON_PRIME);
    pthread_t tid;
    pthread_create(&tid, NULL, hwmon_thread, &hw);
    sleep(1);
    if (!hw.available) {
        fprintf(stderr, "ERROR: bw_hwmon_meas tracepoint not available\n");
        hw.running = 0; pthread_join(tid, NULL); return 1;
    }
    fprintf(stderr, "[ bw_hwmon_meas: ACTIVE ]\n\n");

    /* CSV header */
    printf("flush_MB,ws_MB,rep,chase_GBs,lat_ns,bwmon_MBs\n");

    /* Stderr table header */
    fprintf(stderr, "%-10s %-8s  %-14s %-8s %-12s\n",
            "flush_MB", "ws_MB", "chase(GB/s)", "lat(ns)", "bwmon(MB/s)");
    fprintf(stderr, "%-10s %-8s  %-14s %-8s %-12s\n",
            "----------", "--------", "--------------", "--------", "------------");

    for (int fi = 0; fi < n_flush; fi++) {
        size_t flush_bytes = flush_mb[fi] * 1024 * 1024;

        fprintf(stderr, "\n--- flush = %zu MB ---\n", flush_mb[fi]);

        for (int wi = 0; wi < n_ws; wi++) {
            size_t ws_bytes = ws_mb[wi] * 1024 * 1024;

            uintptr_t *buf = build_chase(ws_bytes);
            if (!buf) {
                fprintf(stderr, "OOM at WS=%zu MB\n", ws_mb[wi]);
                continue;
            }
            prefault(buf, ws_bytes);

            double chase_v[NREPS], lat_v[NREPS], bwmon_v[NREPS];

            for (int r = 0; r < NREPS; r++) {
                /* Establish clean state */
                flush_caches(flush_bytes);
                /* Reload WS into cache */
                run_chase(buf, WARMUP_SECS);
                /* Per-hop latency (CNTVCT, no syscall) */
                lat_v[r] = chase_latency_ns(buf, LATENCY_HOPS);

                /* Bandwidth measurement window */
                double s0; long n0;
                hwmon_snap(&hw, &s0, &n0);

                double    t0   = now_s();
                uint64_t  hops = run_chase(buf, MEASURE_SECS);
                double    elap = now_s() - t0;

                double s1; long n1;
                hwmon_snap(&hw, &s1, &n1);

                chase_v[r] = (double)hops * CACHE_LINE / elap / 1e9;
                bwmon_v[r] = hwmon_bw_window(s0, n0, s1, n1, 2);

                printf("%zu,%zu,%d,%.3f,%.2f,%.1f\n",
                       flush_mb[fi], ws_mb[wi], r + 1,
                       chase_v[r], lat_v[r], bwmon_v[r]);
                fflush(stdout);
            }

            /* Stderr summary row */
            double cm = stat_mean(chase_v, NREPS);
            double cs = stat_sd(chase_v,   NREPS, cm);
            double lm = stat_mean(lat_v,   NREPS);
            double bm = stat_mean(bwmon_v, NREPS);

            /* Classify regime */
            const char *regime;
            if (cm > 18.0)       regime = "cache-resident";
            else if (cm > 14.0)  regime = "transition";
            else                 regime = "DRAM-active";

            fprintf(stderr, "%-10zu %-8zu  %5.2f +/- %-4.2f  %5.2f   %8.0f   %s\n",
                    flush_mb[fi], ws_mb[wi], cm, cs, lm, bm, regime);
            fflush(stderr);

            free(buf);
        }
    }

    hw.running = 0;
    pthread_join(tid, NULL);
    hwmon_destroy(&hw);

    fprintf(stderr,
        "\n=================================================================\n"
        " Done. Inspect: does the DRAM-active boundary shift with flush_MB?\n"
        " If yes  → 24 MB flush was insufficient (LLCC > 24 MB but < boundary)\n"
        " If no   → LLCC is genuinely ~112 MB (boundary fixed regardless of flush)\n"
        "=================================================================\n");

    return 0;
}
