/*
 * hwmon.h — Hardware DRAM monitor (bw_hwmon_meas) + ICC bandwidth vote tracking
 *
 * Reads /sys/kernel/debug/tracing/trace_pipe in a background thread and parses:
 *   - dcvs/bw_hwmon_meas : hardware DRAM monitor (~4 ms samples)
 *       bwmon-llcc-prime  : cpu6-7, Prime cluster (always tracked)
 *       bwmon-llcc-gold   : cpu0-5, Gold cluster  (optional, HWMON_GOLD)
 *   - interconnect/icc_set_bw : DCVS bandwidth vote (~40 ms cadence)
 *       chm_apps -> qns_llcc  : CPU cluster -> LLCC fabric (destination node filter)
 *       llcc_mc  -> ebi       : LLCC -> DRAM controller   (destination node filter)
 *
 * ICC parsing notes:
 *   Each icc_set_bw update fires two events (src and dst node). To avoid double-counting,
 *   filter by both path AND destination node:
 *     path=chm_apps-qns_llcc + node=qns_llcc  (CPU->LLCC, destination only)
 *     path=llcc_mc-ebi        + node=ebi        (LLCC->DRAM, destination only)
 *
 *   avg_bw  = DCVS software client's bandwidth vote (partial view, one client)
 *   agg_avg = aggregate across ALL ICC clients at that node (use for bwmon correlation)
 *
 * Timeseries output (optional):
 *   Set hw.trace_bwmon_fp / hw.trace_icc_fp before starting the thread.
 *   Set hw.trace_t0 = now_s() as the experiment reference time.
 *   The hwmon_thread writes rows without holding the lock (sole writer).
 *
 * Usage:
 *   Hwmon hw;
 *   hwmon_init(&hw, HWMON_PRIME | HWMON_ICC);
 *   hw.trace_bwmon_fp = fopen("/data/local/tmp/trace_bwmon.csv", "w");
 *   hw.trace_icc_fp   = fopen("/data/local/tmp/trace_icc.csv",   "w");
 *   hw.trace_t0       = now_s();
 *   pthread_t tid;
 *   pthread_create(&tid, NULL, hwmon_thread, &hw);
 *   sleep(1);
 *   ...
 *   hw.running = 0;
 *   pthread_join(tid, NULL);
 *   hwmon_destroy(&hw);
 *
 * Snapshot pattern (per measurement window):
 *   double s0; long n0;
 *   hwmon_snap(&hw, &s0, &n0);           // before workload
 *   ... run workload ...
 *   double s1; long n1;
 *   hwmon_snap(&hw, &s1, &n1);           // after workload
 *   double bw = hwmon_bw_window(s0, n0, s1, n1, 3);  // MB/s, -1 if <3 samples
 *
 * Platform: Qualcomm Oryon V2, Snapdragon 8 Elite, Android 15, kernel 6.6
 * Requires: root, tracefs at /sys/kernel/debug/tracing
 *
 * NOTE: trace_pipe is a consuming reader. Only one thread may read it.
 *       All tracepoints needed by an experiment must be enabled here.
 */

#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include "timer.h"

#define TRACE_BASE "/sys/kernel/debug/tracing"

/* Feature flags for hwmon_init() */
#define HWMON_PRIME  0x01  /* Track bwmon-llcc-prime (mandatory base) */
#define HWMON_GOLD   0x02  /* Track bwmon-llcc-gold (Gold cluster background) */
#define HWMON_ICC    0x04  /* Track icc_set_bw (DCVS interconnect votes) */

typedef struct {
    volatile int    running;
    pthread_mutex_t lock;
    int             flags;
    int             available;

    /* bwmon-llcc-prime accumulators (MB/s) */
    double          sum_prime;
    long            n_prime;

    /* bwmon-llcc-gold accumulators (MB/s) — valid only with HWMON_GOLD */
    double          sum_gold;
    long            n_gold;

    /* ICC avg_bw: per-DCVS-client software vote (kBps) — valid only with HWMON_ICC.
     * One client's vote; may undercount total traffic. */
    double          sum_icc_llcc_avg;   /* qns_llcc avg_bw: CPU->LLCC client vote */
    long            n_icc_llcc_avg;
    double          sum_icc_dram_avg;   /* ebi avg_bw: LLCC->DRAM client vote */
    long            n_icc_dram_avg;

    /* ICC agg_avg: aggregate across ALL ICC clients at that node (kBps).
     * Primary metric for bwmon correlation; represents total node demand. */
    double          sum_icc_llcc_agg;   /* qns_llcc agg_avg */
    long            n_icc_llcc_agg;
    double          sum_icc_dram_agg;   /* ebi agg_avg */
    long            n_icc_dram_agg;

    /* Timeseries trace files (written only by hwmon_thread; no lock needed).
     * NULL means no timeseries output. Set before pthread_create. */
    FILE           *trace_bwmon_fp;   /* one row per bwmon event: "t_s,bwmon_MBs\n" */
    FILE           *trace_icc_fp;     /* one row per ICC event: "t_s,node,avg_kBps,agg_kBps\n" */
    double          trace_t0;         /* now_s() at experiment start (relative timestamps) */
} Hwmon;

static inline void hwmon_init(Hwmon *hw, int flags)
{
    memset(hw, 0, sizeof(*hw));
    hw->running = 1;
    hw->flags   = flags;
    pthread_mutex_init(&hw->lock, NULL);
}

static inline void hwmon_destroy(Hwmon *hw)
{
    pthread_mutex_destroy(&hw->lock);
}

/* Parse one null-terminated line from trace_pipe and update accumulators. */
static inline void hwmon_parse_line(Hwmon *hw, const char *line)
{
    if (strstr(line, "bw_hwmon_meas")) {
        int is_prime = (strstr(line, "bwmon-llcc-prime") != NULL);
        int is_gold  = (strstr(line, "bwmon-llcc-gold")  != NULL);
        int want     = (is_prime && (hw->flags & HWMON_PRIME)) ||
                       (is_gold  && (hw->flags & HWMON_GOLD));
        if (want) {
            const char *p = strstr(line, "mbps");
            if (p) {
                unsigned long long v = 0;
                if (sscanf(p, "mbps = %llu", &v) == 1) {
                    if (hw->trace_bwmon_fp && is_prime)
                        fprintf(hw->trace_bwmon_fp, "%.6f,%.1f\n",
                                now_s() - hw->trace_t0, (double)v);
                    pthread_mutex_lock(&hw->lock);
                    if (is_prime && (hw->flags & HWMON_PRIME)) {
                        hw->sum_prime += (double)v;
                        hw->n_prime++;
                    }
                    if (is_gold  && (hw->flags & HWMON_GOLD)) {
                        hw->sum_gold  += (double)v;
                        hw->n_gold++;
                    }
                    pthread_mutex_unlock(&hw->lock);
                }
            }
        }
        return;
    }

    if ((hw->flags & HWMON_ICC) && strstr(line, "icc_set_bw")) {
        /*
         * Filter by both path AND destination node to avoid:
         *   - Double-counting src+dst events for the same update
         *   - Matching unrelated paths (e.g. chm_apps-qhs_i2c, chm_apps-qns_apss)
         */
        int is_icc_llcc = (strstr(line, "path=chm_apps-qns_llcc") != NULL) &&
                          (strstr(line, "node=qns_llcc")           != NULL);
        int is_icc_dram = (strstr(line, "path=llcc_mc-ebi") != NULL) &&
                          (strstr(line, "node=ebi")          != NULL);
        if (is_icc_llcc || is_icc_dram) {
            unsigned long long avg_v = 0, agg_v = 0;
            const char *pa = strstr(line, "avg_bw=");
            const char *pg = strstr(line, "agg_avg=");
            if (pa) sscanf(pa, "avg_bw=%llu",  &avg_v);
            if (pg) sscanf(pg, "agg_avg=%llu", &agg_v);
            if (hw->trace_icc_fp)
                fprintf(hw->trace_icc_fp, "%.6f,%s,%llu,%llu\n",
                        now_s() - hw->trace_t0,
                        is_icc_llcc ? "qns_llcc" : "ebi",
                        avg_v, agg_v);
            pthread_mutex_lock(&hw->lock);
            if (is_icc_llcc) {
                hw->sum_icc_llcc_avg += (double)avg_v;
                hw->n_icc_llcc_avg++;
                hw->sum_icc_llcc_agg += (double)agg_v;
                hw->n_icc_llcc_agg++;
            }
            if (is_icc_dram) {
                hw->sum_icc_dram_avg += (double)avg_v;
                hw->n_icc_dram_avg++;
                hw->sum_icc_dram_agg += (double)agg_v;
                hw->n_icc_dram_agg++;
            }
            pthread_mutex_unlock(&hw->lock);
        }
    }
}

/*
 * Background thread: enables requested tracepoints, reads trace_pipe,
 * parses bwmon and/or ICC lines into Hwmon accumulators.
 */
static void *hwmon_thread(void *arg)
{
    Hwmon *hw = arg;

    /* Enable bw_hwmon_meas */
    int fd = open(TRACE_BASE "/events/dcvs/bw_hwmon_meas/enable", O_WRONLY);
    if (fd < 0) { hw->available = 0; hw->running = 0; return NULL; }
    write(fd, "1", 1); close(fd);

    /* Enable icc_set_bw if requested (non-fatal if unavailable) */
    if (hw->flags & HWMON_ICC) {
        fd = open(TRACE_BASE "/events/interconnect/icc_set_bw/enable", O_WRONLY);
        if (fd >= 0) { write(fd, "1", 1); close(fd); }
    }

    fd = open(TRACE_BASE "/tracing_on", O_WRONLY);
    if (fd >= 0) { write(fd, "1", 1); close(fd); }

    hw->available = 1;

    int pfd = open(TRACE_BASE "/trace_pipe", O_RDONLY | O_NONBLOCK);
    if (pfd < 0) { hw->running = 0; return NULL; }

    char raw[8192], line[512];
    int  lp = 0;

    while (hw->running) {
        ssize_t n = read(pfd, raw, sizeof(raw));
        if (n <= 0) { usleep(2000); continue; }
        for (ssize_t i = 0; i < n; i++) {
            char c = raw[i];
            if (c == '\n' || lp == (int)sizeof(line) - 2) {
                line[lp] = '\0';
                lp = 0;
                hwmon_parse_line(hw, line);
            } else {
                line[lp++] = c;
            }
        }
    }

    close(pfd);

    /* Flush trace files before disabling tracepoints */
    if (hw->trace_bwmon_fp) fflush(hw->trace_bwmon_fp);
    if (hw->trace_icc_fp)   fflush(hw->trace_icc_fp);

    fd = open(TRACE_BASE "/events/dcvs/bw_hwmon_meas/enable", O_WRONLY);
    if (fd >= 0) { write(fd, "0", 1); close(fd); }
    if (hw->flags & HWMON_ICC) {
        fd = open(TRACE_BASE "/events/interconnect/icc_set_bw/enable", O_WRONLY);
        if (fd >= 0) { write(fd, "0", 1); close(fd); }
    }
    return NULL;
}

/*
 * Snapshot the prime-cluster bwmon accumulators (most common case).
 * Call before and after a measurement window, then use hwmon_bw_window().
 */
static inline void hwmon_snap(Hwmon *hw, double *sum, long *n)
{
    pthread_mutex_lock(&hw->lock);
    *sum = hw->sum_prime;
    *n   = hw->n_prime;
    pthread_mutex_unlock(&hw->lock);
}

/*
 * Snapshot all accumulators (for experiments tracking gold + ICC avg_bw).
 * NULL may be passed for any output pointer to skip that field.
 * ICC fields return avg_bw accumulators (per-client vote, kBps).
 */
static inline void hwmon_snap_full(Hwmon *hw,
    double *sp,     long *np,
    double *sg,     long *ng,
    double *icc_cl, long *n_cl,
    double *icc_ld, long *n_ld)
{
    pthread_mutex_lock(&hw->lock);
    if (sp)     *sp     = hw->sum_prime;
    if (np)     *np     = hw->n_prime;
    if (sg)     *sg     = hw->sum_gold;
    if (ng)     *ng     = hw->n_gold;
    if (icc_cl) *icc_cl = hw->sum_icc_llcc_avg;
    if (n_cl)   *n_cl   = hw->n_icc_llcc_avg;
    if (icc_ld) *icc_ld = hw->sum_icc_dram_avg;
    if (n_ld)   *n_ld   = hw->n_icc_dram_avg;
    pthread_mutex_unlock(&hw->lock);
}

/*
 * Snapshot all four ICC accumulators (avg_bw and agg_avg for both paths).
 * NULL may be passed for any output pointer.
 * Units: kBps; divide by 1e3 for MB/s, 1e6 for GB/s.
 */
static inline void hwmon_snap_icc(Hwmon *hw,
    double *llcc_avg, long *n_la,
    double *llcc_agg, long *n_lg,
    double *dram_avg, long *n_da,
    double *dram_agg, long *n_dg)
{
    pthread_mutex_lock(&hw->lock);
    if (llcc_avg) *llcc_avg = hw->sum_icc_llcc_avg;
    if (n_la)     *n_la     = hw->n_icc_llcc_avg;
    if (llcc_agg) *llcc_agg = hw->sum_icc_llcc_agg;
    if (n_lg)     *n_lg     = hw->n_icc_llcc_agg;
    if (dram_avg) *dram_avg = hw->sum_icc_dram_avg;
    if (n_da)     *n_da     = hw->n_icc_dram_avg;
    if (dram_agg) *dram_agg = hw->sum_icc_dram_agg;
    if (n_dg)     *n_dg     = hw->n_icc_dram_agg;
    pthread_mutex_unlock(&hw->lock);
}

/*
 * Compute mean BW over a measurement window from two snapshots.
 * Returns the mean if at least min_samples arrived, -1.0 otherwise.
 * For bwmon (4 ms cadence): use min_samples=3 for 5 s windows, 2 for 3 s.
 * For ICC  (40 ms cadence): use min_samples=1 (fewer events expected).
 */
static inline double hwmon_bw_window(double s0, long n0,
                                     double s1, long n1,
                                     long   min_samples)
{
    long dn = n1 - n0;
    return (dn >= min_samples) ? (s1 - s0) / (double)dn : -1.0;
}
