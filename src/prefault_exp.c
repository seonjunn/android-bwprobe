/*
 * prefault_exp.c — Isolate source of bwmon/bw_matvec ≈ 1.88× amplification
 *
 * During matvec measurement windows, bwmon and bus_access both show ~1.88×
 * the algorithmic A-read bandwidth, despite A being read-only and x,y being
 * L1D-resident.  This experiment isolates WHERE and WHY the extra DRAM
 * traffic comes from.
 *
 * Three conditions at WS=128 MB, 3 reps each:
 *
 *   A) baseline   : write-init A → 24 MB flush → warmup → measure
 *                   Matches matvec.c design.  Expect amp ≈ 1.88×.
 *
 *   B) clean-page : write-init A → madvise(MADV_DONTNEED) → warmup → measure
 *                   DONTNEED releases dirty pages; next access gets OS zero-
 *                   fills (clean, no dirty history).  If amp drops → the
 *                   1.88× comes from dirty write-backs during measurement.
 *                   If amp stays → it is NOT dirty write-backs.
 *
 *   C) no-prefetch: write-init → 24 MB flush → madvise(MADV_RANDOM) → warmup
 *                   → measure.  MADV_RANDOM tells OS to disable sequential
 *                   read-ahead for this mapping.  If amp drops → HW prefetcher
 *                   is over-fetching (prefetch trashing).  If amp stays →
 *                   another effect (e.g. DRAM burst granularity).
 *
 * Phase-separated bwmon for condition A shows when write-backs actually occur:
 *   bwmon_flush   : DRAM traffic during flush_caches() (write-back phase)
 *   bwmon_warmup  : DRAM traffic during warmup pass (first-time DRAM reads)
 *   bwmon_measure : DRAM traffic during timed measurement (the mystery 1.88×)
 *
 * CSV output:
 *   variant,rep, bwmon_flush,bwmon_warmup,bwmon_measure, bw_matvec, amp
 *
 * Build: make prefault_exp
 * Run  : make ADB="adb -P 5307" runprefault
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include "cpu.h"
#include "timer.h"
#include "cache.h"
#include "chase.h"
#include "hwmon.h"
#include "matvec_kernels.h"

#define CPU_PIN       6
#define WS_MB         128
#define M_ROWS        4096
#define MEASURE_SECS  3.0
#define N_REPS        3
#define FLUSH_SZ      (24  * 1024 * 1024)

/* -------------------------------------------------------------------------
 * One measurement variant
 * ---------------------------------------------------------------------- */
typedef enum { VAR_BASELINE, VAR_CLEAN_PAGE, VAR_NO_PREFETCH } Variant;

static void run_variant(Variant var, int M, int N, Hwmon *hw,
                        int rep, FILE *out)
{
    size_t A_bytes = (size_t)M * N * sizeof(float);
    size_t x_bytes = (size_t)N * sizeof(float);
    size_t y_bytes = (size_t)M * sizeof(float);

    /* Allocate */
    float *A = mmap(NULL, A_bytes, PROT_READ|PROT_WRITE,
                    MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    float *x = aligned_alloc(CACHE_LINE, x_bytes < CACHE_LINE ? CACHE_LINE : x_bytes);
    float *y = aligned_alloc(CACHE_LINE, y_bytes);
    if (A == MAP_FAILED || !x || !y) {
        fprintf(stderr, "OOM\n");
        if (A != MAP_FAILED) munmap(A, A_bytes);
        free(x); free(y); return;
    }

    /* Write-init A (non-trivial, prevents dead-store elimination) */
    for (size_t i = 0; i < (size_t)M * N; i++) A[i] = (float)(i % 97 + 1) * 0.01f;
    for (int j = 0; j < N; j++) x[j] = (float)(j % 31 + 1) * 0.03f;
    for (int i = 0; i < M; i++) y[i] = 0.0f;
    prefault(x, x_bytes < CACHE_LINE ? CACHE_LINE : x_bytes);
    prefault(y, y_bytes);

    const char *vname;
    if (var == VAR_BASELINE) {
        /* A: normal flush */
        vname = "baseline";
    } else if (var == VAR_CLEAN_PAGE) {
        /*
         * B: madvise(MADV_DONTNEED) releases all dirty A pages back to the OS.
         * Next access to A will receive fresh OS zero-filled pages (clean).
         * No dirty write-backs will ever occur for A.
         */
        vname = "clean_page";
        madvise(A, A_bytes, MADV_DONTNEED);
    } else {
        /* C: MADV_RANDOM disables sequential read-ahead / prefetch hints */
        vname = "no_prefetch";
        madvise(A, A_bytes, MADV_RANDOM);
    }

    /* ── Phase 1: flush ── */
    double s0, s1, s2, s3; long n0, n1, n2, n3;
    hwmon_snap(hw, &s0, &n0);
    flush_caches(FLUSH_SZ);
    hwmon_snap(hw, &s1, &n1);
    double bwmon_flush = hwmon_bw_window(s0, n0, s1, n1, 2);

    /* ── Phase 2: warmup ── */
    matvec_scalar(A, x, y, M, N);
    hwmon_snap(hw, &s2, &n2);
    double bwmon_warmup = hwmon_bw_window(s1, n1, s2, n2, 2);

    /* ── Phase 3: measure ── */
    /* For MADV_RANDOM: apply AFTER warmup (warmup intentionally skips it
       to pre-warm the LLCC normally), so the random hint only affects the
       measurement's hardware prefetcher behavior. */
    if (var == VAR_NO_PREFETCH)
        madvise(A, A_bytes, MADV_RANDOM);

    double t0 = now_s(); long passes = 0;
    while (now_s() - t0 < MEASURE_SECS) {
        matvec_scalar(A, x, y, M, N);
        passes++;
    }
    double elapsed = now_s() - t0;
    hwmon_snap(hw, &s3, &n3);
    double bwmon_measure = hwmon_bw_window(s2, n2, s3, n3, 2);

    double bw_matvec = (double)passes * (double)A_bytes / elapsed / 1e6;
    double amp = (bwmon_measure > 0) ? bwmon_measure / bw_matvec : -1.0;

    fprintf(out, "%s,%d,%.1f,%.1f,%.1f,%.1f,%.4f\n",
            vname, rep,
            bwmon_flush, bwmon_warmup, bwmon_measure, bw_matvec, amp);
    fprintf(stderr, "  %-12s rep%d: flush=%6.0f warmup=%6.0f measure=%6.0f bw_alg=%5.0f amp=%.3f (all MB/s)\n",
            vname, rep,
            bwmon_flush, bwmon_warmup, bwmon_measure, bw_matvec, amp);
    fflush(stderr);

    /* Reset MADV_RANDOM before free if applied */
    if (var == VAR_NO_PREFETCH)
        madvise(A, A_bytes, MADV_NORMAL);

    munmap(A, A_bytes);
    free(x); free(y);

    /* Prevent dead-store elimination */
    volatile float sink = y ? 0.0f : 1.0f; (void)sink;
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */
int main(void)
{
    pin_to_cpu(CPU_PIN);

    int M = M_ROWS;
    int N = (int)((size_t)WS_MB * 1024 * 1024 / ((size_t)M * sizeof(float)));
    size_t A_bytes = (size_t)M * N * sizeof(float);

    fprintf(stderr,
        "=================================================================\n"
        " prefault_exp -- bwmon amplification source isolation\n"
        " WS=%d MB  M=%d  N=%d  x=%zu KB  y=%d KB\n"
        " Variants: baseline / clean_page(DONTNEED) / no_prefetch(RANDOM)\n"
        " %d reps x %.1f s  |  cpu%d\n"
        "=================================================================\n\n",
        WS_MB, M, N, (size_t)N*4/1024, M*4/1024, N_REPS, MEASURE_SECS, CPU_PIN);

    fprintf(stderr,
        " Hypotheses:\n"
        "   1. Dirty write-backs: amp drops for clean_page → write-init causes DRAM writes\n"
        "   2. HW prefetcher:     amp drops for no_prefetch → prefetch trashing\n"
        "   3. DRAM burst:        amp stays for both → DRAM burst > cache-line size\n\n");

    fprintf(stderr, " A bytes = %.0f MB\n\n", (double)A_bytes / 1048576);

    /* Start hwmon thread */
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
    FILE *out = stdout;
    fprintf(out, "variant,rep,bwmon_flush_MBs,bwmon_warmup_MBs,"
                 "bwmon_measure_MBs,bw_matvec_MBs,amp\n");

    Variant variants[] = { VAR_BASELINE, VAR_CLEAN_PAGE, VAR_NO_PREFETCH };
    const char *vnames[] = { "baseline", "clean_page", "no_prefetch" };
    int nvariants = 3;

    for (int v = 0; v < nvariants; v++) {
        fprintf(stderr, "── Variant: %s ──\n", vnames[v]);
        for (int r = 0; r < N_REPS; r++) {
            run_variant(variants[v], M, N, &hw, r+1, out);
            fflush(out);
            sleep(1); /* thermal recovery */
        }
        fprintf(stderr, "\n");
    }

    hw.running = 0;
    pthread_join(tid, NULL);
    hwmon_destroy(&hw);
    return 0;
}
