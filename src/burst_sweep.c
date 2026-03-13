/*
 * burst_sweep.c — Three experiments to demystify bwmon > theoretical DRAM peak
 *
 * Exp 1 — Dense WS sweep (neon_matvec): bwmon/bw_alg amplification factor vs WS.
 *   Prediction: ratio rises from ~0 at WS ≤ 12 MB (L2-resident) to ~1.88 at WS ≫ L2.
 *   If mechanism is L2 thrashing + LPDDR5X 128B burst: crossover should be at WS ≈ 12–20 MB.
 *   Collect icc_dram at each WS — if icc_dram ≤ 85.33 GB/s while bwmon > 85.33 → bwmon ≠ DRAM.
 *
 * Exp 2 — Non-temporal store: does bwmon count write traffic?
 *   stnp (Store Non-Temporal Pair) bypasses cache hierarchy — pure DRAM writes, no read-allocate.
 *   If bwmon ≈ 0: bwmon is reads-only confirmed.
 *   If bwmon ≈ bw_alg × 1.88: bwmon counts writes with burst amplification.
 *
 * Exp 3 — DRAM ceiling: empirical bwmon saturation level (served by WS=128–512 MB points).
 *   If bwmon plateaus at ≤ 85.33 GB/s → DRAM peak confirmed.
 *   If bwmon saturates above 85.33 GB/s → device peak > product brief or bwmon ≠ DRAM boundary.
 *
 * WS sweep:  {3,6,8,10,12,14,16,18,20,24,32,48,64,128,256,512} MB, 3 reps
 * Store sweep: {64,128,256} MB, 3 reps
 *
 * stdout (→ burst_sweep.csv):
 *   workload,ws_mb,rep,bwmon_MBs,icc_llcc_agg_MBs,icc_dram_agg_MBs,bw_alg_MBs,bus_access_MBs
 *
 * Platform : Qualcomm Oryon V2, Snapdragon 8 Elite, Android 15, kernel 6.6
 * Requires : root, perf_event_paranoid=-1, tracefs at /sys/kernel/debug/tracing
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include "cpu.h"
#include "timer.h"
#include "cache.h"
#include "buspoll.h"
#include "hwmon.h"
#include "pmu.h"
#include "thermal.h"
#include "matvec_kernels.h"

/* --------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */
#define CPU_PIN         6
#define M_ROWS          4096      /* matvec y vector: L1D-resident */
#define MEAS_SECS       3.0       /* measurement window per rep */
#define WARMUP_SECS     1.0       /* duration of each warmup window */
#define N_WARMUP        2         /* fixed warmup passes before each rep */
#define NREPS           3         /* repetitions per (workload, ws_mb) */
#define FLUSH_SIZE      (24 * 1024 * 1024)
#define THERMAL_MAX_C   42.0
#define THERMAL_TIMEOUT 120
#define DEVICE_DIR      "/data/local/tmp"

static const int ws_mb_sweep[] = {3,6,8,10,12,14,16,18,20,24,32,48,64,128,256,512};
#define N_WS_SWEEP ((int)(sizeof(ws_mb_sweep) / sizeof(ws_mb_sweep[0])))

static const int ws_mb_store[] = {64, 128, 256};
#define N_WS_STORE ((int)(sizeof(ws_mb_store) / sizeof(ws_mb_store[0])))

/* --------------------------------------------------------------------------
 * Snapshot + emit helpers  (pattern from bw_bench.c)
 * ------------------------------------------------------------------------- */
typedef struct {
    double sp; long np;
    double lg; long nlg;   /* icc qns_llcc agg_avg */
    double dg; long ndg;   /* icc ebi agg_avg */
    uint64_t bus;
} Snap;

static void take_snap(Hwmon *hw, int bus_fd, Snap *s)
{
    hwmon_snap(hw, &s->sp, &s->np);
    hwmon_snap_icc(hw,
        NULL, NULL, &s->lg, &s->nlg,
        NULL, NULL, &s->dg, &s->ndg);
    s->bus = 0;
    if (bus_fd >= 0) read(bus_fd, &s->bus, sizeof(s->bus));
}

static void emit_row(const char *wl, int ws_mb, int rep,
                     const Snap *s0, const Snap *s1,
                     double bw_alg_MBs, double elapsed, int bus_ok)
{
    double bwmon    = hwmon_bw_window(s0->sp, s0->np, s1->sp, s1->np, 2);
    double lg       = hwmon_bw_window(s0->lg, s0->nlg, s1->lg, s1->nlg, 1);
    double dg       = hwmon_bw_window(s0->dg, s0->ndg, s1->dg, s1->ndg, 1);
    double icc_llcc = (lg > 0) ? lg / 1e3 : -1.0;
    double icc_dram = (dg > 0) ? dg / 1e3 : -1.0;
    double bus      = bus_ok
        ? (double)(s1->bus - s0->bus) * CACHE_LINE / elapsed / 1e6
        : -1.0;

    printf("%s,%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f\n",
           wl, ws_mb, rep, bwmon, icc_llcc, icc_dram, bw_alg_MBs, bus);
    fflush(stdout);

    fprintf(stderr,
            "  %-12s ws=%4dMB rep%d:"
            " bwmon=%8.0f  icc_dram=%7.0f  alg=%8.0f  bus=%8.0f\n",
            wl, ws_mb, rep, bwmon, icc_dram, bw_alg_MBs, bus);
    fflush(stderr);
}

/* --------------------------------------------------------------------------
 * Thermal gate
 * ------------------------------------------------------------------------- */
static void wait_for_thermal(void)
{
    double skin = read_skin_c();
    if (skin < THERMAL_MAX_C) return;
    fprintf(stderr, "  [thermal: %.1f°C — waiting for cooling]", skin);
    fflush(stderr);
    double t0 = now_s();
    while (skin >= THERMAL_MAX_C && now_s() - t0 < THERMAL_TIMEOUT) {
        sleep(5);
        skin = read_skin_c();
        fprintf(stderr, " %.1f°C", skin);
        fflush(stderr);
    }
    fprintf(stderr, "\n");
    if (skin >= THERMAL_MAX_C)
        fprintf(stderr, "  [thermal: timed out at %.1f°C — proceeding]\n", skin);
}

/* --------------------------------------------------------------------------
 * Workload: neon_matvec
 * Streaming read-only; proven BW-bound; bwmon fires at WS ≥ 16 MB (LLCC bypassed).
 * ------------------------------------------------------------------------- */
static void run_matvec_ws(Hwmon *hw, int bus_fd, int ws_mb)
{
    int    M       = M_ROWS;
    int    N       = (int)((size_t)ws_mb * 1024 * 1024 /
                           ((size_t)M * sizeof(float)));
    size_t A_bytes = (size_t)M * N * sizeof(float);
    size_t x_bytes = (size_t)N * sizeof(float);
    size_t y_bytes = (size_t)M * sizeof(float);

    float *A = aligned_alloc(CACHE_LINE, A_bytes);
    float *x = aligned_alloc(CACHE_LINE, x_bytes < CACHE_LINE ? CACHE_LINE : x_bytes);
    float *y = aligned_alloc(CACHE_LINE, y_bytes);
    if (!A || !x || !y) {
        fprintf(stderr, "neon_matvec %dMB: OOM\n", ws_mb);
        free(A); free(x); free(y); return;
    }
    for (size_t i = 0; i < (size_t)M * N; i++) A[i] = (float)(i % 97 + 1) * 0.01f;
    for (int j = 0; j < N; j++) x[j] = (float)(j % 31 + 1) * 0.03f;
    for (int i = 0; i < M; i++) y[i] = 0.0f;
    prefault(A, A_bytes);

    for (int r = 0; r < NREPS; r++) {
        wait_for_thermal();
        flush_caches(FLUSH_SIZE);

        /* Fixed N_WARMUP × WARMUP_SECS warmup — no convergence check needed */
        for (int w = 0; w < N_WARMUP; w++) {
            double t0 = now_s();
            while (now_s() - t0 < WARMUP_SECS)
                matvec_neon(A, x, y, M, N);
        }

        Snap s0; take_snap(hw, bus_fd, &s0);
        double t0    = now_s();
        long   passes = 0;
        while (now_s() - t0 < MEAS_SECS) {
            matvec_neon(A, x, y, M, N);
            passes++;
        }
        double elapsed = now_s() - t0;
        Snap s1; take_snap(hw, bus_fd, &s1);

        double bw_alg = (double)passes * (double)A_bytes / elapsed / 1e6;
        emit_row("neon_matvec", ws_mb, r + 1, &s0, &s1, bw_alg, elapsed, (bus_fd >= 0));

        sleep(2);
    }

    volatile float sink = y[0]; (void)sink;
    free(A); free(x); free(y);
}

/* --------------------------------------------------------------------------
 * Workload: non-temporal store (Exp 2)
 * stnp bypasses all cache levels — pure write traffic, no read-allocate.
 * bwmon ≈ 0 → reads-only confirmed; bwmon > 0 → bwmon sees writes.
 * ------------------------------------------------------------------------- */
static void run_store_ws(Hwmon *hw, int bus_fd, int ws_mb)
{
    size_t n = (size_t)ws_mb * 1024 * 1024 / sizeof(float);
    n &= ~(size_t)63;   /* round down to 256B (64-float) boundary for store_neon */

    float *buf = aligned_alloc(128, n * sizeof(float));
    if (!buf) {
        fprintf(stderr, "store_nt %dMB: OOM\n", ws_mb);
        return;
    }
    for (size_t i = 0; i < n; i++) buf[i] = 0.0f;
    prefault(buf, n * sizeof(float));

    for (int r = 0; r < NREPS; r++) {
        wait_for_thermal();
        flush_caches(FLUSH_SIZE);

        /* Warmup: N_WARMUP × WARMUP_SECS */
        for (int w = 0; w < N_WARMUP; w++)
            store_neon(buf, n, WARMUP_SECS);

        Snap s0; take_snap(hw, bus_fd, &s0);
        double t0    = now_s();
        long   passes = store_neon(buf, n, MEAS_SECS);
        double elapsed = now_s() - t0;
        Snap s1; take_snap(hw, bus_fd, &s1);

        double bw_alg = (double)passes * (double)n * sizeof(float) / elapsed / 1e6;
        emit_row("store_nt", ws_mb, r + 1, &s0, &s1, bw_alg, elapsed, (bus_fd >= 0));

        sleep(2);
    }

    free(buf);
}

/* --------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */
int main(void)
{
    pin_to_cpu(CPU_PIN);

    fprintf(stderr,
        "=================================================================\n"
        " burst_sweep — bwmon amplification factor & DRAM ceiling\n"
        " WS sweep: %d pts  |  Store sweep: %d pts  |  cpu%d  |  %d reps\n"
        " Thermal gate: <%.0f°C  |  Warmup: %d×%.0fs  |  Meas: %.0fs\n"
        "=================================================================\n\n",
        N_WS_SWEEP, N_WS_STORE, CPU_PIN, NREPS,
        THERMAL_MAX_C, N_WARMUP, WARMUP_SECS, MEAS_SECS);

    /* Initialize hwmon */
    Hwmon hw;
    hwmon_init(&hw, HWMON_PRIME | HWMON_ICC);

    pthread_t htid;
    pthread_create(&htid, NULL, hwmon_thread, &hw);
    sleep(2);   /* allow ICC events before first measurement */

    if (!hw.available) {
        fprintf(stderr, "ERROR: bw_hwmon_meas tracepoint not available\n");
        hw.running = 0;
        pthread_join(htid, NULL);
        hwmon_destroy(&hw);
        return 1;
    }
    fprintf(stderr, "[ bw_hwmon_meas: ACTIVE ]\n");

    int bus_fd = open_bus_access();
    fprintf(stderr, "[ bus_access: %s ]\n\n",
            bus_fd >= 0 ? "AVAILABLE" : "NOT AVAILABLE");

    BusPoll bp = { .running = 1, .bus_fd = bus_fd,
                   .trace_fp = NULL, .t0 = now_s(), .cpu_pin = CPU_PIN };
    pthread_t btid = 0;
    if (bus_fd >= 0)
        pthread_create(&btid, NULL, buspoll_thread, &bp);

    /* CSV header */
    printf("workload,ws_mb,rep,bwmon_MBs,"
           "icc_llcc_agg_MBs,icc_dram_agg_MBs,"
           "bw_alg_MBs,bus_access_MBs\n");

    /* ---- Exp 1 + 3: neon_matvec WS sweep ---- */
    fprintf(stderr,
        "\n── Exp 1+3: neon_matvec WS sweep (amplification factor + DRAM ceiling)\n");
    for (int w = 0; w < N_WS_SWEEP; w++) {
        int ws_mb = ws_mb_sweep[w];
        fprintf(stderr, "\n══ WS = %d MB ══\n", ws_mb);
        run_matvec_ws(&hw, bus_fd, ws_mb);
        if (w < N_WS_SWEEP - 1) sleep(3);
    }

    /* ---- Exp 2: non-temporal store sweep ---- */
    fprintf(stderr,
        "\n── Exp 2: non-temporal store (does bwmon count writes?)\n");
    for (int w = 0; w < N_WS_STORE; w++) {
        int ws_mb = ws_mb_store[w];
        fprintf(stderr, "\n══ WS = %d MB (store_nt) ══\n", ws_mb);
        run_store_ws(&hw, bus_fd, ws_mb);
        if (w < N_WS_STORE - 1) sleep(3);
    }

    /* Stop threads */
    bp.running = 0;
    if (btid) pthread_join(btid, NULL);
    if (bus_fd >= 0) close(bus_fd);

    hw.running = 0;
    pthread_join(htid, NULL);
    hwmon_destroy(&hw);

    fprintf(stderr,
        "\nDone. Post-process: python3 analysis/burst_sweep_analysis.py\n");
    return 0;
}
