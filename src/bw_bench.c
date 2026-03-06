/*
 * bw_bench.c — Controlled bandwidth benchmark with thermal gating + convergence warmup
 *
 * Improvements over icc_bw.c:
 *   1. Thermal gating: waits for skin_c < THERMAL_MAX_C before each rep
 *   2. Convergence warmup: repeats 1-second warmup windows until bwmon stabilizes (< 5%)
 *      with WS-dependent minimum pass count (more passes for cache-resident WS)
 *   3. Wider workload variety: chase (latency), scalar matvec (compute-bound),
 *      NEON matvec (memory-intensive read-only), NEON AXPY (read+write streaming)
 *   4. WS sweep: {4, 16, 64, 128, 256} MB covering cache→DRAM transition
 *   5. 5 reps per (workload, ws_mb) for tighter statistics
 *
 * Arithmetic intensities:
 *   chase      : latency-bound; BW << DRAM peak regardless of WS
 *   scalar     : FMA-latency-bound; ~6 GB/s ceiling at any WS (compute-bound)
 *   neon_matvec: read-only streaming; ~54 GB/s at DRAM WS (BW-bound)
 *   neon_axpy  : read+write streaming; 3× traffic/WS vs matvec (most BW-intensive)
 *
 * Trace files written on device (DEVICE_DIR):
 *   bw_bench_trace_bwmon.csv : t_s,bwmon_MBs
 *   bw_bench_trace_icc.csv   : t_s,node,avg_kBps,agg_kBps
 *   bw_bench_trace_bus.csv   : t_s,bus_access_MBs
 *   bw_bench_context.csv     : t_s,event,workload,ws_mb,rep,bw_alg_MBs
 *
 * stdout (→ bw_bench_summary.csv):
 *   workload,ws_mb,rep,bwmon_MBs,icc_llcc_agg_MBs,icc_dram_agg_MBs,
 *   bw_alg_MBs,bus_access_MBs,warmup_passes,skin_c
 *
 * Post-processing: python3 analysis/split_traces.py --prefix bw_bench
 *
 * Platform : Qualcomm Oryon V2, Snapdragon 8 Elite, Android 15, kernel 6.6
 * Requires : root, perf_event_paranoid=-1, tracefs at /sys/kernel/debug/tracing
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <math.h>
#include <pthread.h>
#include "cpu.h"
#include "timer.h"
#include "cache.h"
#include "chase.h"
#include "buspoll.h"
#include "hwmon.h"
#include "pmu.h"
#include "thermal.h"
#include "matvec_kernels.h"

/* --------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */
#define CPU_PIN          6
#define M_ROWS           4096      /* matvec y vector: 16 KB, L1D-resident */
#define MEAS_SECS        5.0       /* measurement window per rep */
#define WARMUP_SECS      1.0       /* duration of each warmup window */
#define WARMUP_TOL       0.05      /* 5% bwmon change → converged */
#define WARMUP_MAX       8         /* cap on warmup passes */
#define NREPS            5
#define FLUSH_SIZE       (24 * 1024 * 1024)
#define THERMAL_MAX_C    42.0      /* wait for cooling if above this */
#define THERMAL_TIMEOUT  120       /* seconds to wait before giving up */
#define DEVICE_DIR       "/data/local/tmp"

static const int ws_mb_list[] = {4, 16, 64, 128, 256};
#define N_WS ((int)(sizeof(ws_mb_list) / sizeof(ws_mb_list[0])))

/* --------------------------------------------------------------------------
 * Thermal gating — wait for skin temperature to drop below threshold
 * ------------------------------------------------------------------------- */
static void wait_for_thermal(void)
{
    double skin = read_skin_c();
    if (skin < THERMAL_MAX_C) return;

    fprintf(stderr, "  [thermal: %.1f°C > %.0f°C — waiting for cooling]",
            skin, THERMAL_MAX_C);
    fflush(stderr);

    double t_start = now_s();
    while (skin >= THERMAL_MAX_C && now_s() - t_start < THERMAL_TIMEOUT) {
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
 * Minimum warmup passes based on WS size.
 * Cache-resident WS needs more passes to fill LLCC and stabilize.
 * DRAM-regime WS is self-evicting; 2 passes suffice.
 * ------------------------------------------------------------------------- */
static int warmup_min_passes(int ws_mb)
{
    if (ws_mb <= 16)  return 5;   /* always cache-resident; fill LLCC */
    if (ws_mb <= 112) return 3;   /* may fit in LLCC (112 MB capacity) */
    return 2;                     /* DRAM regime; self-evicting */
}

/*
 * Generic convergence warmup using time-bounded "run windows".
 * run_window(arg) must execute the workload for approximately WARMUP_SECS.
 * Returns the number of warmup passes actually run.
 */
static int warmup_until_stable(Hwmon *hw, void (*run_window)(void *), void *arg,
                                int min_passes)
{
    double prev_bw = -1.0;
    for (int p = 0; p < WARMUP_MAX; p++) {
        double s0; long n0;
        hwmon_snap(hw, &s0, &n0);
        run_window(arg);
        double s1; long n1;
        hwmon_snap(hw, &s1, &n1);
        double bw = hwmon_bw_window(s0, n0, s1, n1, 1);
        if (p + 1 >= min_passes && bw > 0 && prev_bw > 0) {
            if (fabs(bw - prev_bw) / prev_bw < WARMUP_TOL)
                return p + 1;
        }
        if (bw > 0) prev_bw = bw;
    }
    return WARMUP_MAX;
}

/* --------------------------------------------------------------------------
 * Snapshot helpers
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

/* --------------------------------------------------------------------------
 * Per-rep result
 * ------------------------------------------------------------------------- */
typedef struct {
    double bwmon_MBs;
    double icc_llcc_agg_MBs;
    double icc_dram_agg_MBs;
    double bw_alg_MBs;
    double bus_access_MBs;
    int    warmup_passes;
    double skin_c;
} BenchResult;

static void compute_result(const Snap *s0, const Snap *s1,
                           double bw_alg_MBs, double elapsed_s,
                           int bus_ok, int warmup_passes, BenchResult *r)
{
    r->bwmon_MBs      = hwmon_bw_window(s0->sp, s0->np, s1->sp, s1->np, 2);

    double lg = hwmon_bw_window(s0->lg, s0->nlg, s1->lg, s1->nlg, 1);
    double dg = hwmon_bw_window(s0->dg, s0->ndg, s1->dg, s1->ndg, 1);
    r->icc_llcc_agg_MBs = (lg > 0) ? lg / 1e3 : -1.0;
    r->icc_dram_agg_MBs = (dg > 0) ? dg / 1e3 : -1.0;

    r->bw_alg_MBs    = bw_alg_MBs;
    r->bus_access_MBs = bus_ok
        ? (double)(s1->bus - s0->bus) * CACHE_LINE / elapsed_s / 1e6
        : -1.0;
    r->warmup_passes = warmup_passes;
    r->skin_c        = read_skin_c();
}

static void emit_row(const char *wl, int ws_mb, int rep, const BenchResult *r)
{
    printf("%s,%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%.1f\n",
           wl, ws_mb, rep,
           r->bwmon_MBs, r->icc_llcc_agg_MBs, r->icc_dram_agg_MBs,
           r->bw_alg_MBs, r->bus_access_MBs,
           r->warmup_passes, r->skin_c);
    fflush(stdout);

    fprintf(stderr,
            "  %-12s ws=%3dMB rep%d:"
            " bwmon=%7.0f  icc_dram=%7.0f  alg=%7.0f"
            " bus=%7.0f  warmup=%d  skin=%.1f°C\n",
            wl, ws_mb, rep,
            r->bwmon_MBs, r->icc_dram_agg_MBs, r->bw_alg_MBs,
            r->bus_access_MBs, r->warmup_passes, r->skin_c);
    fflush(stderr);
}

static void ctx_log(FILE *fp, double t0_exp, const char *event,
                    const char *wl, int ws_mb, int rep, double bw_alg)
{
    if (!fp) return;
    fprintf(fp, "%.6f,%s,%s,%d,%d,%.1f\n",
            now_s() - t0_exp, event, wl, ws_mb, rep, bw_alg);
    fflush(fp);
}

/* --------------------------------------------------------------------------
 * Workload: pointer-chase
 * ------------------------------------------------------------------------- */
typedef struct { uintptr_t *buf; } ChaseCtx;

static void chase_window(void *arg)
{
    run_chase(((ChaseCtx *)arg)->buf, WARMUP_SECS);
}

static void run_chase_ws(Hwmon *hw, int bus_fd, FILE *ctx_fp, double t_exp,
                         int ws_mb, BenchResult res[NREPS])
{
    size_t ws = (size_t)ws_mb * 1024 * 1024;
    uintptr_t *buf = build_chase(ws);
    if (!buf) { fprintf(stderr, "chase %dMB: OOM\n", ws_mb); return; }
    prefault(buf, ws);

    ChaseCtx ctx = { .buf = buf };

    for (int r = 0; r < NREPS; r++) {
        wait_for_thermal();
        flush_caches(FLUSH_SIZE);

        int wp = warmup_until_stable(hw, chase_window, &ctx, warmup_min_passes(ws_mb));

        ctx_log(ctx_fp, t_exp, "start", "chase", ws_mb, r + 1, -1.0);
        Snap s0; take_snap(hw, bus_fd, &s0);

        double t0   = now_s();
        uint64_t hops = run_chase(buf, MEAS_SECS);
        double elapsed = now_s() - t0;

        Snap s1; take_snap(hw, bus_fd, &s1);
        double bw_alg = (double)hops * CACHE_LINE / elapsed / 1e6;
        compute_result(&s0, &s1, bw_alg, elapsed, (bus_fd >= 0), wp, &res[r]);
        ctx_log(ctx_fp, t_exp, "end", "chase", ws_mb, r + 1, bw_alg);
        emit_row("chase", ws_mb, r + 1, &res[r]);

        sleep(2);
    }
    free(buf);
}

/* --------------------------------------------------------------------------
 * Workload: matvec (scalar or NEON)
 * Arithmetic intensity: A is streamed from DRAM (ws_mb); x is tiny (ws_mb/256 KB)
 * and stays L1D-resident after first pass. bw_alg counts A traffic only.
 * ------------------------------------------------------------------------- */
typedef struct {
    const float *A; const float *x; float *y;
    int M, N; int use_neon;
} MatvecCtx;

static void matvec_window(void *arg)
{
    MatvecCtx *c = arg;
    double t0 = now_s();
    while (now_s() - t0 < WARMUP_SECS) {
        if (c->use_neon) matvec_neon(c->A, c->x, c->y, c->M, c->N);
        else             matvec_scalar(c->A, c->x, c->y, c->M, c->N);
    }
}

static void run_matvec_ws(Hwmon *hw, int bus_fd, FILE *ctx_fp, double t_exp,
                           int ws_mb, int use_neon, BenchResult res[NREPS])
{
    const char *wl = use_neon ? "neon_matvec" : "scalar_matvec";
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
        fprintf(stderr, "%s %dMB: OOM\n", wl, ws_mb);
        free(A); free(x); free(y); return;
    }
    for (size_t i = 0; i < (size_t)M * N; i++) A[i] = (float)(i % 97 + 1) * 0.01f;
    for (int j = 0; j < N; j++) x[j] = (float)(j % 31 + 1) * 0.03f;
    for (int i = 0; i < M; i++) y[i] = 0.0f;
    prefault(A, A_bytes);

    MatvecCtx ctx = { A, x, y, M, N, use_neon };

    for (int r = 0; r < NREPS; r++) {
        wait_for_thermal();
        flush_caches(FLUSH_SIZE);

        int wp = warmup_until_stable(hw, matvec_window, &ctx, warmup_min_passes(ws_mb));

        ctx_log(ctx_fp, t_exp, "start", wl, ws_mb, r + 1, -1.0);
        Snap s0; take_snap(hw, bus_fd, &s0);

        double t0    = now_s();
        long   passes = 0;
        while (now_s() - t0 < MEAS_SECS) {
            if (use_neon) matvec_neon(A, x, y, M, N);
            else          matvec_scalar(A, x, y, M, N);
            passes++;
        }
        double elapsed = now_s() - t0;

        Snap s1; take_snap(hw, bus_fd, &s1);
        /* bw_alg: only A is streamed from DRAM; x (N*4 = ws_mb/256 KB) is L1D-resident */
        double bw_alg = (double)passes * (double)A_bytes / elapsed / 1e6;
        compute_result(&s0, &s1, bw_alg, elapsed, (bus_fd >= 0), wp, &res[r]);
        ctx_log(ctx_fp, t_exp, "end", wl, ws_mb, r + 1, bw_alg);
        emit_row(wl, ws_mb, r + 1, &res[r]);

        sleep(2);
    }

    volatile float sink = y[0]; (void)sink;
    free(A); free(x); free(y);
}

/* --------------------------------------------------------------------------
 * Workload: NEON AXPY  (y[i] += alpha * x[i])
 * Memory traffic per pass: 3 * N * sizeof(float) bytes (read x, read+write y).
 * bw_alg counts all 3 arrays (read x, read y, write y).
 * ------------------------------------------------------------------------- */
typedef struct {
    float alpha; float *x; float *y; int N;
} AxpyCtx;

static void axpy_window(void *arg)
{
    AxpyCtx *c = arg;
    double t0 = now_s();
    while (now_s() - t0 < WARMUP_SECS)
        axpy_neon(c->alpha, c->x, c->y, c->N);
}

static void run_axpy_ws(Hwmon *hw, int bus_fd, FILE *ctx_fp, double t_exp,
                         int ws_mb, BenchResult res[NREPS])
{
    /* x and y split the working set 50/50 */
    int    N       = (int)((size_t)ws_mb * 1024 * 1024 / (2 * sizeof(float)));
    size_t arr_bytes = (size_t)N * sizeof(float);

    float *x = aligned_alloc(CACHE_LINE, arr_bytes);
    float *y = aligned_alloc(CACHE_LINE, arr_bytes);
    if (!x || !y) {
        fprintf(stderr, "axpy %dMB: OOM\n", ws_mb);
        free(x); free(y); return;
    }
    for (int i = 0; i < N; i++) { x[i] = (float)(i % 97 + 1) * 0.01f; y[i] = 0.0f; }
    prefault(x, arr_bytes);
    prefault(y, arr_bytes);

    AxpyCtx ctx = { .alpha = 1.5f, .x = x, .y = y, .N = N };

    for (int r = 0; r < NREPS; r++) {
        wait_for_thermal();
        flush_caches(FLUSH_SIZE);

        int wp = warmup_until_stable(hw, axpy_window, &ctx, warmup_min_passes(ws_mb));

        ctx_log(ctx_fp, t_exp, "start", "neon_axpy", ws_mb, r + 1, -1.0);
        Snap s0; take_snap(hw, bus_fd, &s0);

        double t0    = now_s();
        long   passes = 0;
        while (now_s() - t0 < MEAS_SECS) {
            axpy_neon(ctx.alpha, x, y, N);
            passes++;
        }
        double elapsed = now_s() - t0;

        Snap s1; take_snap(hw, bus_fd, &s1);
        /* bw_alg: 3 streams (read x, read y, write y) = 3 * arr_bytes per pass */
        double bw_alg = (double)passes * 3.0 * (double)arr_bytes / elapsed / 1e6;
        compute_result(&s0, &s1, bw_alg, elapsed, (bus_fd >= 0), wp, &res[r]);
        ctx_log(ctx_fp, t_exp, "end", "neon_axpy", ws_mb, r + 1, bw_alg);
        emit_row("neon_axpy", ws_mb, r + 1, &res[r]);

        sleep(2);
    }

    volatile float sink = y[0]; (void)sink;
    free(x); free(y);
}

/* --------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */
int main(void)
{
    pin_to_cpu(CPU_PIN);

    fprintf(stderr,
        "=================================================================\n"
        " bw_bench — Controlled bandwidth benchmark (thermal + warmup)\n"
        " WS: {4,16,64,128,256} MB  |  cpu%d  |  %d reps\n"
        " Thermal gate: <%.0f°C  |  Warmup tol: %.0f%%  |  Meas: %.0fs\n"
        "=================================================================\n\n",
        CPU_PIN, NREPS, THERMAL_MAX_C, WARMUP_TOL * 100, MEAS_SECS);

    /* Open trace files */
    FILE *trace_bwmon_fp = fopen(DEVICE_DIR "/bw_bench_trace_bwmon.csv", "w");
    FILE *trace_icc_fp   = fopen(DEVICE_DIR "/bw_bench_trace_icc.csv",   "w");
    FILE *trace_bus_fp   = fopen(DEVICE_DIR "/bw_bench_trace_bus.csv",   "w");
    FILE *ctx_fp         = fopen(DEVICE_DIR "/bw_bench_context.csv",      "w");

    if (trace_bwmon_fp) fprintf(trace_bwmon_fp, "t_s,bwmon_MBs\n");
    if (trace_icc_fp)   fprintf(trace_icc_fp,   "t_s,node,avg_kBps,agg_kBps\n");
    if (trace_bus_fp)   fprintf(trace_bus_fp,    "t_s,bus_access_MBs\n");
    if (ctx_fp)         fprintf(ctx_fp,          "t_s,event,workload,ws_mb,rep,bw_alg_MBs\n");

    /* Initialize hwmon */
    Hwmon hw;
    hwmon_init(&hw, HWMON_PRIME | HWMON_ICC);
    hw.trace_bwmon_fp = trace_bwmon_fp;
    hw.trace_icc_fp   = trace_icc_fp;
    hw.trace_t0       = now_s();

    pthread_t htid;
    pthread_create(&htid, NULL, hwmon_thread, &hw);
    sleep(2);  /* allow ICC events to arrive before first measurement */

    if (!hw.available) {
        fprintf(stderr, "ERROR: bw_hwmon_meas tracepoint not available\n");
        hw.running = 0;
        pthread_join(htid, NULL);
        hwmon_destroy(&hw);
        return 1;
    }
    fprintf(stderr, "[ bw_hwmon_meas: ACTIVE ]\n");

    /* bus_access PMU */
    int bus_fd = open_bus_access();
    fprintf(stderr, "[ bus_access: %s ]\n\n",
            bus_fd >= 0 ? "AVAILABLE" : "NOT AVAILABLE");

    BusPoll bp = { .running = 1, .bus_fd = bus_fd,
                   .trace_fp = trace_bus_fp, .t0 = hw.trace_t0, .cpu_pin = CPU_PIN };
    pthread_t btid = 0;
    if (bus_fd >= 0)
        pthread_create(&btid, NULL, buspoll_thread, &bp);

    /* CSV header */
    printf("workload,ws_mb,rep,bwmon_MBs,"
           "icc_llcc_agg_MBs,icc_dram_agg_MBs,"
           "bw_alg_MBs,bus_access_MBs,warmup_passes,skin_c\n");

    double t_exp = hw.trace_t0;

    /* ---- WS sweep ---- */
    for (int w = 0; w < N_WS; w++) {
        int ws_mb = ws_mb_list[w];
        fprintf(stderr,
            "\n══ WS = %d MB ══════════════════════════════════════════\n",
            ws_mb);

        BenchResult res_chase[NREPS], res_scalar[NREPS],
                    res_neon[NREPS],  res_axpy[NREPS];

        fprintf(stderr, "  [chase]\n");
        run_chase_ws(&hw, bus_fd, ctx_fp, t_exp, ws_mb, res_chase);
        sleep(3);

        fprintf(stderr, "  [scalar_matvec]\n");
        run_matvec_ws(&hw, bus_fd, ctx_fp, t_exp, ws_mb, 0, res_scalar);
        sleep(3);

        fprintf(stderr, "  [neon_matvec]\n");
        run_matvec_ws(&hw, bus_fd, ctx_fp, t_exp, ws_mb, 1, res_neon);
        sleep(3);

        fprintf(stderr, "  [neon_axpy]\n");
        run_axpy_ws(&hw, bus_fd, ctx_fp, t_exp, ws_mb, res_axpy);

        if (w < N_WS - 1) sleep(5);  /* thermal recovery between WS points */
    }

    /* Stop threads */
    bp.running = 0;
    if (btid) pthread_join(btid, NULL);
    if (bus_fd >= 0) close(bus_fd);

    hw.running = 0;
    pthread_join(htid, NULL);
    hwmon_destroy(&hw);

    if (trace_bwmon_fp) fclose(trace_bwmon_fp);
    if (trace_icc_fp)   fclose(trace_icc_fp);
    if (trace_bus_fp)   fclose(trace_bus_fp);
    if (ctx_fp)         fclose(ctx_fp);

    fprintf(stderr,
        "\nTrace files written to " DEVICE_DIR ":\n"
        "  bw_bench_trace_bwmon.csv  bw_bench_trace_icc.csv\n"
        "  bw_bench_trace_bus.csv    bw_bench_context.csv\n"
        "\nPost-process: python3 analysis/split_traces.py --prefix bw_bench\n");

    return 0;
}
