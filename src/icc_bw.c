/*
 * icc_bw.c — ICC bandwidth votes vs bwmon correlation: WS sweep + timeseries
 *
 * Sweeps working-set size {8, 32, 64, 128, 256} MB across three workloads
 * (pointer-chase, matvec scalar, matvec NEON) to observe how bwmon (hardware
 * DRAM ground truth), ICC votes (software DCVS votes), and bus_access PMU
 * track each other across the cache-to-DRAM transition.
 *
 * ICC field semantics (kBps):
 *   avg_bw   = one DCVS client's bandwidth vote (partial, may be lower than total)
 *   agg_avg  = aggregate across ALL ICC clients at that node (use for bwmon comparison)
 *
 * Trace files written on device (DEVICE_DIR):
 *   icc_bw_trace_bwmon.csv  : t_s,bwmon_MBs
 *   icc_bw_trace_icc.csv    : t_s,node,avg_kBps,agg_kBps
 *   icc_bw_trace_bus.csv    : t_s,bus_access_MBs
 *   icc_bw_context.csv      : t_s,event,workload,ws_mb,rep,bw_alg_MBs
 *
 * stdout (redirected to icc_bw_summary.csv by make):
 *   workload,ws_mb,rep,bwmon_MBs,icc_llcc_avg_MBs,icc_llcc_agg_MBs,
 *   icc_dram_avg_MBs,icc_dram_agg_MBs,bw_alg_MBs,bus_access_MBs,skin_c
 *
 * Platform : Qualcomm Oryon V2, Snapdragon 8 Elite, Android 15, kernel 6.6
 * Requires : root, perf_event_paranoid=-1,
 *            tracefs at /sys/kernel/debug/tracing,
 *            events/dcvs/bw_hwmon_meas and events/interconnect/icc_set_bw
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
#include "chase.h"
#include "stats.h"
#include "buspoll.h"
#include "hwmon.h"
#include "pmu.h"
#include "thermal.h"
#include "matvec_kernels.h"

/* --------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */
#define CPU_PIN       6
#define M_ROWS        4096        /* y = 16 KB, always L1D-resident */
#define CHASE_SECS    5.0
#define MATVEC_SECS   3.0
#define NREPS         3
#define FLUSH_SIZE    (24 * 1024 * 1024)
#define LATENCY_HOPS  (1ULL << 20)
#define DEVICE_DIR    "/data/local/tmp"

static const int ws_mb_list[] = {8, 32, 64, 128, 256};
#define N_WS ((int)(sizeof(ws_mb_list) / sizeof(ws_mb_list[0])))

/* --------------------------------------------------------------------------
 * Full snapshot — bwmon + all ICC fields + bus_access raw count
 * ------------------------------------------------------------------------- */
typedef struct {
    double sp; long np;                     /* bwmon-prime */
    double la; long nla;                    /* icc qns_llcc avg_bw */
    double lg; long nlg;                    /* icc qns_llcc agg_avg */
    double da; long nda;                    /* icc ebi avg_bw */
    double dg; long ndg;                    /* icc ebi agg_avg */
    uint64_t bus;                           /* bus_access cumulative count */
} FullSnap;

static void take_snap(Hwmon *hw, int bus_fd, FullSnap *s)
{
    hwmon_snap(hw, &s->sp, &s->np);
    hwmon_snap_icc(hw,
        &s->la, &s->nla, &s->lg, &s->nlg,
        &s->da, &s->nda, &s->dg, &s->ndg);
    s->bus = 0;
    if (bus_fd >= 0)
        read(bus_fd, &s->bus, sizeof(s->bus));
}

/* --------------------------------------------------------------------------
 * Per-rep result
 * ------------------------------------------------------------------------- */
typedef struct {
    double bwmon_MBs;
    double icc_llcc_avg_MBs;
    double icc_llcc_agg_MBs;
    double icc_dram_avg_MBs;
    double icc_dram_agg_MBs;
    double bw_alg_MBs;
    double bus_access_MBs;
    double skin_c;
} IccResult;

static void compute_result(const FullSnap *s0, const FullSnap *s1,
                           double bw_alg_MBs, double elapsed_s,
                           int bus_ok, IccResult *r)
{
    r->bwmon_MBs = hwmon_bw_window(s0->sp, s0->np, s1->sp, s1->np, 2);

    /* ICC: kBps / 1e3 = MB/s */
    double la = hwmon_bw_window(s0->la, s0->nla, s1->la, s1->nla, 1);
    double lg = hwmon_bw_window(s0->lg, s0->nlg, s1->lg, s1->nlg, 1);
    double da = hwmon_bw_window(s0->da, s0->nda, s1->da, s1->nda, 1);
    double dg = hwmon_bw_window(s0->dg, s0->ndg, s1->dg, s1->ndg, 1);
    r->icc_llcc_avg_MBs = (la > 0) ? la / 1e3 : -1.0;
    r->icc_llcc_agg_MBs = (lg > 0) ? lg / 1e3 : -1.0;
    r->icc_dram_avg_MBs = (da > 0) ? da / 1e3 : -1.0;
    r->icc_dram_agg_MBs = (dg > 0) ? dg / 1e3 : -1.0;

    r->bw_alg_MBs    = bw_alg_MBs;
    r->bus_access_MBs = bus_ok
        ? (double)(s1->bus - s0->bus) * CACHE_LINE / elapsed_s / 1e6
        : -1.0;
    r->skin_c = read_skin_c();
}

/* --------------------------------------------------------------------------
 * Emit CSV row to stdout + detail line to stderr
 * ------------------------------------------------------------------------- */
static void emit_row(const char *wl, int ws_mb, int rep, const IccResult *r)
{
    printf("%s,%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
           wl, ws_mb, rep,
           r->bwmon_MBs,
           r->icc_llcc_avg_MBs, r->icc_llcc_agg_MBs,
           r->icc_dram_avg_MBs, r->icc_dram_agg_MBs,
           r->bw_alg_MBs, r->bus_access_MBs, r->skin_c);
    fflush(stdout);

    fprintf(stderr,
            "  %-7s ws=%3dMB rep%d:"
            " bwmon=%7.0f  icc_dram_agg=%7.0f  alg=%7.0f"
            " bus=%7.0f  skin=%.1f°C (MB/s)\n",
            wl, ws_mb, rep,
            r->bwmon_MBs,
            r->icc_dram_agg_MBs,
            r->bw_alg_MBs,
            r->bus_access_MBs,
            r->skin_c);
    fflush(stderr);
}

/* Write one row to context log (bw_alg_MBs=-1 for "start" events) */
static void ctx_log(FILE *fp, double t_exp, const char *event,
                    const char *wl, int ws_mb, int rep, double bw_alg)
{
    if (!fp) return;
    fprintf(fp, "%.6f,%s,%s,%d,%d,%.1f\n",
            now_s() - t_exp, event, wl, ws_mb, rep, bw_alg);
    fflush(fp);
}

/* --------------------------------------------------------------------------
 * Workload: pointer-chase
 * ------------------------------------------------------------------------- */
static void run_chase_ws(Hwmon *hw, int bus_fd, FILE *ctx_fp, double t_exp,
                         int ws_mb, IccResult res[NREPS])
{
    size_t ws = (size_t)ws_mb * 1024 * 1024;
    uintptr_t *buf = build_chase(ws);
    if (!buf) { fprintf(stderr, "chase %dMB: OOM\n", ws_mb); return; }
    prefault(buf, ws);

    for (int r = 0; r < NREPS; r++) {
        flush_caches(FLUSH_SIZE);
        run_chase(buf, 0.5);  /* warmup: reload WS into LLCC */

        ctx_log(ctx_fp, t_exp, "start", "chase", ws_mb, r + 1, -1.0);

        FullSnap s0;
        take_snap(hw, bus_fd, &s0);

        double t0 = now_s();
        uint64_t hops = run_chase(buf, CHASE_SECS);
        double elapsed = now_s() - t0;

        FullSnap s1;
        take_snap(hw, bus_fd, &s1);

        double bw_alg = (double)hops * CACHE_LINE / elapsed / 1e6;
        compute_result(&s0, &s1, bw_alg, elapsed, (bus_fd >= 0), &res[r]);

        ctx_log(ctx_fp, t_exp, "end", "chase", ws_mb, r + 1, bw_alg);
        emit_row("chase", ws_mb, r + 1, &res[r]);
    }

    free(buf);
}

/* --------------------------------------------------------------------------
 * Workload: matvec (scalar or NEON)
 * ------------------------------------------------------------------------- */
static void run_matvec_ws(Hwmon *hw, int bus_fd, FILE *ctx_fp, double t_exp,
                          int ws_mb, int use_neon, IccResult res[NREPS])
{
    const char *wl = use_neon ? "neon" : "scalar";
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
    prefault(x, x_bytes < CACHE_LINE ? CACHE_LINE : x_bytes);
    prefault(y, y_bytes);

    for (int r = 0; r < NREPS; r++) {
        flush_caches(FLUSH_SIZE);
        /* Warmup: one pass to establish cache state */
        if (use_neon) matvec_neon(A, x, y, M, N);
        else          matvec_scalar(A, x, y, M, N);

        ctx_log(ctx_fp, t_exp, "start", wl, ws_mb, r + 1, -1.0);

        FullSnap s0;
        take_snap(hw, bus_fd, &s0);

        double t0    = now_s();
        long   passes = 0;
        while (now_s() - t0 < MATVEC_SECS) {
            if (use_neon) matvec_neon(A, x, y, M, N);
            else          matvec_scalar(A, x, y, M, N);
            passes++;
        }
        double elapsed = now_s() - t0;

        FullSnap s1;
        take_snap(hw, bus_fd, &s1);

        double bw_alg = (double)passes * (double)A_bytes / elapsed / 1e6;
        compute_result(&s0, &s1, bw_alg, elapsed, (bus_fd >= 0), &res[r]);

        ctx_log(ctx_fp, t_exp, "end", wl, ws_mb, r + 1, bw_alg);
        emit_row(wl, ws_mb, r + 1, &res[r]);
    }

    volatile float sink = y[0]; (void)sink;
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
        " icc_bw -- ICC vs bwmon correlation: WS sweep + timeseries\n"
        " WS: {8,32,64,128,256} MB  |  cpu%d  |  %d reps/workload\n"
        " Workloads: chase (%.0fs), scalar (%.0fs), NEON (%.0fs)\n"
        "=================================================================\n\n",
        CPU_PIN, NREPS, CHASE_SECS, MATVEC_SECS, MATVEC_SECS);

    /* Open trace files on device */
    FILE *trace_bwmon_fp = fopen(DEVICE_DIR "/icc_bw_trace_bwmon.csv", "w");
    FILE *trace_icc_fp   = fopen(DEVICE_DIR "/icc_bw_trace_icc.csv",   "w");
    FILE *trace_bus_fp   = fopen(DEVICE_DIR "/icc_bw_trace_bus.csv",   "w");
    FILE *ctx_fp         = fopen(DEVICE_DIR "/icc_bw_context.csv",      "w");

    if (trace_bwmon_fp) fprintf(trace_bwmon_fp, "t_s,bwmon_MBs\n");
    if (trace_icc_fp)   fprintf(trace_icc_fp,   "t_s,node,avg_kBps,agg_kBps\n");
    if (trace_bus_fp)   fprintf(trace_bus_fp,    "t_s,bus_access_MBs\n");
    if (ctx_fp)         fprintf(ctx_fp,          "t_s,event,workload,ws_mb,rep,bw_alg_MBs\n");

    /* Initialize hwmon with trace files */
    Hwmon hw;
    hwmon_init(&hw, HWMON_PRIME | HWMON_ICC);
    hw.trace_bwmon_fp = trace_bwmon_fp;
    hw.trace_icc_fp   = trace_icc_fp;
    hw.trace_t0       = now_s();

    pthread_t htid;
    pthread_create(&htid, NULL, hwmon_thread, &hw);
    sleep(2);  /* allow tracepoint events to arrive; ICC fires ~40 ms */

    if (!hw.available) {
        fprintf(stderr, "ERROR: bw_hwmon_meas tracepoint not available\n");
        hw.running = 0;
        pthread_join(htid, NULL);
        hwmon_destroy(&hw);
        return 1;
    }
    fprintf(stderr, "[ bw_hwmon_meas: ACTIVE ]\n");

    {
        double s; long n;
        hwmon_snap_full(&hw, NULL, NULL, NULL, NULL, &s, &n, NULL, NULL);
        if (n == 0)
            fprintf(stderr,
                "[ icc_set_bw: no events yet — may appear once workload starts ]\n\n");
        else
            fprintf(stderr,
                "[ icc_set_bw: ACTIVE (%ld events in startup window) ]\n\n", n);
    }

    /* Open bus_access PMU and start BusPoll thread */
    int bus_fd = open_bus_access();
    if (bus_fd < 0)
        fprintf(stderr, "[ bus_access: NOT AVAILABLE (%s) ]\n\n", strerror(errno));
    else
        fprintf(stderr, "[ bus_access: AVAILABLE ]\n\n");

    BusPoll bp = { .running = 1, .bus_fd = bus_fd,
                   .trace_fp = trace_bus_fp, .t0 = hw.trace_t0, .cpu_pin = CPU_PIN };
    pthread_t btid = 0;
    if (bus_fd >= 0)
        pthread_create(&btid, NULL, buspoll_thread, &bp);

    /* CSV header */
    printf("workload,ws_mb,rep,bwmon_MBs,"
           "icc_llcc_avg_MBs,icc_llcc_agg_MBs,"
           "icc_dram_avg_MBs,icc_dram_agg_MBs,"
           "bw_alg_MBs,bus_access_MBs,skin_c\n");

    double t_exp = hw.trace_t0;

    /* ---- WS sweep ---- */
    for (int w = 0; w < N_WS; w++) {
        int ws_mb = ws_mb_list[w];
        fprintf(stderr,
            "\n══ WS = %d MB ══════════════════════════════════════════\n",
            ws_mb);

        IccResult res_chase[NREPS], res_scalar[NREPS], res_neon[NREPS];

        fprintf(stderr, "  [chase]\n");
        run_chase_ws(&hw, bus_fd, ctx_fp, t_exp, ws_mb, res_chase);
        sleep(1);

        fprintf(stderr, "  [scalar]\n");
        run_matvec_ws(&hw, bus_fd, ctx_fp, t_exp, ws_mb, 0, res_scalar);
        sleep(1);

        fprintf(stderr, "  [neon]\n");
        run_matvec_ws(&hw, bus_fd, ctx_fp, t_exp, ws_mb, 1, res_neon);
        sleep(1);

        /* Per-WS summary to stderr */
        const IccResult *all[3] = { res_chase, res_scalar, res_neon };
        const char *wnames[3]   = { "chase", "scalar", "neon" };
        fprintf(stderr,
            "\n  WS=%dMB summary  %-7s  %7s  %7s  %7s  %7s  %7s\n",
            ws_mb, "workload", "bwmon", "icc_da", "icc_dg", "alg", "bus");
        for (int i = 0; i < 3; i++) {
            double bm = 0, da = 0, dg = 0, am = 0, um = 0;
            int bn = 0, dan = 0, dgn = 0;
            for (int r = 0; r < NREPS; r++) {
                if (all[i][r].bwmon_MBs        > 0) { bm += all[i][r].bwmon_MBs;        bn++; }
                if (all[i][r].icc_dram_avg_MBs > 0) { da += all[i][r].icc_dram_avg_MBs; dan++; }
                if (all[i][r].icc_dram_agg_MBs > 0) { dg += all[i][r].icc_dram_agg_MBs; dgn++; }
                am += all[i][r].bw_alg_MBs;
                if (all[i][r].bus_access_MBs > 0) um += all[i][r].bus_access_MBs;
            }
            if (bn)  bm /= bn;  if (dan) da /= dan;  if (dgn) dg /= dgn;
            am /= NREPS;        um /= NREPS;
            fprintf(stderr,
                "  WS=%dMB summary  %-7s  %7.0f  %7.0f  %7.0f  %7.0f  %7.0f\n",
                ws_mb, wnames[i], bm, da, dg, am, um);
        }

        if (w < N_WS - 1) sleep(2);  /* thermal recovery between WS points */
    }

    /* Stop threads */
    bp.running = 0;
    if (btid) pthread_join(btid, NULL);
    if (bus_fd >= 0) close(bus_fd);

    hw.running = 0;
    pthread_join(htid, NULL);
    hwmon_destroy(&hw);

    /* Close trace files */
    if (trace_bwmon_fp) fclose(trace_bwmon_fp);
    if (trace_icc_fp)   fclose(trace_icc_fp);
    if (trace_bus_fp)   fclose(trace_bus_fp);
    if (ctx_fp)         fclose(ctx_fp);

    fprintf(stderr,
        "\nTrace files written to " DEVICE_DIR ":\n"
        "  icc_bw_trace_bwmon.csv  icc_bw_trace_icc.csv\n"
        "  icc_bw_trace_bus.csv    icc_bw_context.csv\n");

    return 0;
}
