/*
 * matvec.c — CPU DRAM bandwidth via streaming matrix-vector multiply
 *
 * Complements bwprobe.c (random pointer-chase) by using a workload that
 * exploits hardware prefetching and achieves high Memory-Level Parallelism.
 *
 * Kernel:  y = A x,  A: MxN float32 row-major,  x: N float32,  y: M float32
 *
 * Two kernel variants are run back-to-back in each rep:
 *   scalar  — autovectorized C (compiler emits NEON FMLA)
 *   neon    — explicit 8-accumulator NEON with vfmaq_f32, fully unrolled 32-wide
 *
 * M is fixed at 4096 so y = 16 KB stays L1D-resident.
 * N varies with WS: N = WS_bytes / (M x 4).  x is at most 64 KB (L1D).
 * A is the only source of cache/DRAM pressure.
 *
 * Per rep:
 *   1. flush_caches(24 MB)  -- evict LLCC for clean starting state
 *   2. scalar warmup + timed measurement (bus_access + bwmon)
 *   3. NEON warmup + timed measurement (bus_access + bwmon)
 *
 * Platform : Qualcomm Oryon V2, Snapdragon 8 Elite, Android 15, kernel 6.6
 * Requires : root, perf_event_paranoid=-1, tracefs at /sys/kernel/debug/tracing
 *
 * Build : make matvec
 * Run   : make runmatvec   (CSV -> /tmp/matvec.csv, summary -> /tmp/matvec.log)
 *
 * Output (stdout) -- CSV, one row per (WS, rep):
 *   ws_MB, M, N, rep, skin_c,
 *   elapsed_s, passes, bw_matvec_MBs,
 *   elapsed_neon_s, passes_neon, bw_matvec_neon_MBs,
 *   bw_hwmon_MBs, bw_hwmon_neon_MBs,
 *   bw_bus_access_MBs, bw_bus_access_neon_MBs
 * Output (stderr) -- human-readable mean+/-stddev table
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <math.h>
#include <pthread.h>
#include "cpu.h"
#include "timer.h"
#include "cache.h"
#include "stats.h"
#include "hwmon.h"
#include "pmu.h"
#include "thermal.h"
#include "matvec_kernels.h"

/* --------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */
#define CPU_PIN         6       /* Oryon V2 Prime; bwmon-llcc-prime covers cpu6-7 */
#define M_ROWS          4096    /* y = M*4 = 16 KB -- always L1D-resident */
#define MEASURE_SECS    3.0     /* per-rep measurement window (each kernel) */
#define N_REPS          7       /* independent trials per WS point */
#define FLUSH_SIZE      (24 * 1024 * 1024)  /* > LLCC (12 MB) */

/* --------------------------------------------------------------------------
 * Per-rep result
 * ------------------------------------------------------------------------- */
typedef struct {
    /* scalar measurement */
    double elapsed_s;
    long   passes;
    double bw_matvec_MBs;
    double bw_hwmon_MBs;
    double bw_bus_MBs;
    /* NEON measurement */
    double elapsed_neon_s;
    long   passes_neon;
    double bw_matvec_neon_MBs;
    double bw_hwmon_neon_MBs;
    double bw_bus_neon_MBs;
    double skin_c;
} RepResult;

/* --------------------------------------------------------------------------
 * Measurement: one WS point, N_REPS reps (scalar + NEON per rep)
 * ------------------------------------------------------------------------- */
static void measure_ws(int ws_mb, int M, int N, Hwmon *hw,
                       RepResult res[N_REPS])
{
    size_t A_bytes = (size_t)M * N * sizeof(float);
    size_t x_bytes = (size_t)N *     sizeof(float);
    size_t y_bytes = (size_t)M *     sizeof(float);

    float *A = aligned_alloc(CACHE_LINE, A_bytes);
    float *x = aligned_alloc(CACHE_LINE, x_bytes < CACHE_LINE ? CACHE_LINE : x_bytes);
    float *y = aligned_alloc(CACHE_LINE, y_bytes);
    if (!A || !x || !y) {
        fprintf(stderr, "OOM at WS=%d MB\n", ws_mb);
        free(A); free(x); free(y);
        return;
    }

    /* Non-trivial init -- prevents dead-store elimination */
    for (size_t i = 0; i < (size_t)M * N; i++) A[i] = (float)(i % 97 + 1) * 0.01f;
    for (int j = 0; j < N; j++) x[j] = (float)(j % 31 + 1) * 0.03f;
    for (int i = 0; i < M; i++) y[i] = 0.0f;

    /* Fault in physical pages */
    prefault(A, A_bytes);
    prefault(x, x_bytes < CACHE_LINE ? CACHE_LINE : x_bytes);
    prefault(y, y_bytes);

    /* Open bus_access counter once per WS point */
    int bus_fd = open_bus_access();
    int bus_ok = (bus_fd >= 0);

    for (int r = 0; r < N_REPS; r++) {
        res[r].skin_c = read_skin_c();

        /* ---- Flush LLCC then run scalar measurement ---- */
        flush_caches(FLUSH_SIZE);

        /* Scalar warmup: one full pass to load A into LLCC */
        matvec_scalar(A, x, y, M, N);

        /* hwmon snapshot before scalar */
        double s0; long n0;
        hwmon_snap(hw, &s0, &n0);

        /* PMU reset + enable for scalar */
        if (bus_ok) {
            ioctl(bus_fd, PERF_EVENT_IOC_RESET,  0);
            ioctl(bus_fd, PERF_EVENT_IOC_ENABLE, 0);
        }

        double t0     = now_s();
        long   passes = 0;
        while (now_s() - t0 < MEASURE_SECS) {
            matvec_scalar(A, x, y, M, N);
            passes++;
        }
        double elapsed = now_s() - t0;

        uint64_t bus_scalar = 0;
        if (bus_ok) {
            ioctl(bus_fd, PERF_EVENT_IOC_DISABLE, 0);
            read(bus_fd, &bus_scalar, sizeof(bus_scalar));
        }

        /* hwmon snapshot after scalar */
        double s1; long n1;
        hwmon_snap(hw, &s1, &n1);
        double hw_bw = hwmon_bw_window(s0, n0, s1, n1, 3);
        double A_streamed = (double)passes * (double)A_bytes;

        res[r].elapsed_s     = elapsed;
        res[r].passes        = passes;
        res[r].bw_matvec_MBs = A_streamed / elapsed / 1e6;
        res[r].bw_hwmon_MBs  = hw_bw;
        res[r].bw_bus_MBs    = bus_ok
                               ? (double)bus_scalar * CACHE_LINE / elapsed / 1e6
                               : -1.0;

        /* ---- NEON warmup then NEON measurement (no additional LLCC flush) ---- */
        matvec_neon(A, x, y, M, N);

        /* hwmon snapshot before NEON */
        double s0n; long n0n;
        hwmon_snap(hw, &s0n, &n0n);

        /* PMU reset + enable for NEON */
        if (bus_ok) {
            ioctl(bus_fd, PERF_EVENT_IOC_RESET,  0);
            ioctl(bus_fd, PERF_EVENT_IOC_ENABLE, 0);
        }

        double t0n      = now_s();
        long   passesn  = 0;
        while (now_s() - t0n < MEASURE_SECS) {
            matvec_neon(A, x, y, M, N);
            passesn++;
        }
        double elapsedn = now_s() - t0n;

        uint64_t bus_neon = 0;
        if (bus_ok) {
            ioctl(bus_fd, PERF_EVENT_IOC_DISABLE, 0);
            read(bus_fd, &bus_neon, sizeof(bus_neon));
        }

        /* hwmon snapshot after NEON */
        double s1n; long n1n;
        hwmon_snap(hw, &s1n, &n1n);
        double hw_bwn = hwmon_bw_window(s0n, n0n, s1n, n1n, 3);
        double A_streamn = (double)passesn * (double)A_bytes;

        res[r].elapsed_neon_s     = elapsedn;
        res[r].passes_neon        = passesn;
        res[r].bw_matvec_neon_MBs = A_streamn / elapsedn / 1e6;
        res[r].bw_hwmon_neon_MBs  = hw_bwn;
        res[r].bw_bus_neon_MBs    = bus_ok
                                    ? (double)bus_neon * CACHE_LINE / elapsedn / 1e6
                                    : -1.0;
    }

    if (bus_ok) close(bus_fd);

    /* Prevent dead-code elimination */
    volatile float sink = y[0];
    (void)sink;

    free(A); free(x); free(y);
}

/* --------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */
int main(void)
{
    pin_to_cpu(CPU_PIN);

    fprintf(stderr,
        "=================================================================\n"
        " matvec -- DRAM bandwidth (streaming y = Ax, float32)\n"
        " Device : Snapdragon 8 Elite (Oryon V2), pinned to CPU%d\n"
        " M = %d (y = %d KB, L1D-resident);  x <= 64 KB (L1D-resident)\n"
        " %d reps x %.1f s per kernel per WS point; scalar + NEON variants\n"
        "=================================================================\n\n",
        CPU_PIN, M_ROWS, M_ROWS * 4 / 1024, N_REPS, MEASURE_SECS);

    /* Start hwmon background thread */
    Hwmon hw;
    hwmon_init(&hw, HWMON_PRIME);
    pthread_t tid;
    pthread_create(&tid, NULL, hwmon_thread, &hw);
    sleep(1);

    if (hw.available)
        fprintf(stderr, "[ bw_hwmon_meas tracepoint: ACTIVE (ground truth) ]\n\n");
    else
        fprintf(stderr, "[ bw_hwmon_meas tracepoint: NOT AVAILABLE ]\n\n");

    /* Probe bus_access availability */
    {
        int fd = open_bus_access();
        if (fd >= 0) {
            fprintf(stderr, "[ bus_access (0x0019): AVAILABLE ]\n\n");
            close(fd);
        } else {
            fprintf(stderr, "[ bus_access (0x0019): NOT AVAILABLE (%s) ]\n\n",
                    strerror(errno));
        }
    }

    /* CSV header */
    printf("ws_MB,M,N,rep,skin_c,elapsed_s,passes,bw_matvec_MBs,"
           "elapsed_neon_s,passes_neon,bw_matvec_neon_MBs,"
           "bw_hwmon_MBs,bw_hwmon_neon_MBs,"
           "bw_bus_access_MBs,bw_bus_access_neon_MBs\n");

    /* Stderr summary header */
    fprintf(stderr,
        "%-8s  %-6s  %-8s  %-20s  %-20s  %-20s  %-20s\n",
        "WS(MB)", "N", "x_size",
        "scalar (MB/s)", "neon (MB/s)", "hwmon_scalar (MB/s)", "hwmon_neon (MB/s)");
    fprintf(stderr,
        "%-8s  %-6s  %-8s  %-20s  %-20s  %-20s  %-20s\n",
        "--------", "------", "--------",
        "mean +/- stddev", "mean +/- stddev", "mean +/- stddev", "mean +/- stddev");

    /* WS sweep */
    static const int ws_mb_list[] = { 2, 4, 8, 16, 32, 64, 128, 256 };
    static const int n_ws         = (int)(sizeof(ws_mb_list)/sizeof(ws_mb_list[0]));

    int M = M_ROWS;

    for (int w = 0; w < n_ws; w++) {
        int ws_mb  = ws_mb_list[w];
        int N      = (int)((size_t)ws_mb * 1024 * 1024 /
                          ((size_t)M * sizeof(float)));
        if (N < 1) continue;

        RepResult res[N_REPS];
        measure_ws(ws_mb, M, N, &hw, res);

        /* Emit CSV rows and collect stat arrays */
        double mv_v[N_REPS], neon_v[N_REPS];
        double hw_v[N_REPS], hwn_v[N_REPS];
        double ba_v[N_REPS], ban_v[N_REPS];

        for (int r = 0; r < N_REPS; r++) {
            printf("%d,%d,%d,%d,%.1f,"
                   "%.4f,%ld,%.1f,"
                   "%.4f,%ld,%.1f,"
                   "%.1f,%.1f,"
                   "%.1f,%.1f\n",
                   ws_mb, M, N, r + 1,
                   res[r].skin_c,
                   res[r].elapsed_s,      res[r].passes,      res[r].bw_matvec_MBs,
                   res[r].elapsed_neon_s, res[r].passes_neon, res[r].bw_matvec_neon_MBs,
                   res[r].bw_hwmon_MBs,     res[r].bw_hwmon_neon_MBs,
                   res[r].bw_bus_MBs,       res[r].bw_bus_neon_MBs);

            mv_v[r]  = res[r].bw_matvec_MBs;
            neon_v[r] = res[r].bw_matvec_neon_MBs;
            hw_v[r]  = (res[r].bw_hwmon_MBs     > 0) ? res[r].bw_hwmon_MBs     : 0.0;
            hwn_v[r] = (res[r].bw_hwmon_neon_MBs > 0) ? res[r].bw_hwmon_neon_MBs : 0.0;
            ba_v[r]  = (res[r].bw_bus_MBs        > 0) ? res[r].bw_bus_MBs        : 0.0;
            ban_v[r] = (res[r].bw_bus_neon_MBs   > 0) ? res[r].bw_bus_neon_MBs   : 0.0;
        }
        fflush(stdout);

        /* Compute and emit stats to stderr */
        double mv_m   = stat_mean(mv_v,   N_REPS), mv_s  = stat_sd(mv_v,   N_REPS, mv_m);
        double neon_m = stat_mean(neon_v,  N_REPS), neon_s = stat_sd(neon_v, N_REPS, neon_m);
        double hw_m   = stat_mean(hw_v,   N_REPS), hw_s  = stat_sd(hw_v,   N_REPS, hw_m);
        double hwn_m  = stat_mean(hwn_v,  N_REPS), hwn_s = stat_sd(hwn_v,  N_REPS, hwn_m);
        (void)ba_v; (void)ban_v; /* available in CSV; omit from narrow stderr */

        char mv_str[32], neon_str[32], hw_str[32], hwn_str[32], x_str[16];
        snprintf(mv_str,   sizeof(mv_str),   "%.0f +/- %.0f", mv_m,   mv_s);
        snprintf(neon_str, sizeof(neon_str), "%.0f +/- %.0f", neon_m, neon_s);
        snprintf(hw_str,   sizeof(hw_str),   "%.0f +/- %.0f", hw_m,   hw_s);
        snprintf(hwn_str,  sizeof(hwn_str),  "%.0f +/- %.0f", hwn_m,  hwn_s);
        snprintf(x_str,    sizeof(x_str),    "%d KB",          N * 4 / 1024);

        fprintf(stderr, "%-8d  %-6d  %-8s  %-20s  %-20s  %-20s  %-20s\n",
                ws_mb, N, x_str, mv_str, neon_str, hw_str, hwn_str);
        fflush(stderr);
    }

    /* Stop hwmon thread */
    hw.running = 0;
    pthread_join(tid, NULL);
    hwmon_destroy(&hw);

    return 0;
}
