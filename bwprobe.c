/*
 * bwprobe.c — CPU DRAM bandwidth measurement probe
 *
 * Probes every available PMU event alongside the hardware DRAM monitor
 * (bw_hwmon_meas tracepoint) across a working-set sweep.  For each event,
 * computes the ratio of its bandwidth estimate to the hardware ground truth,
 * then identifies the most accurate method and states the calibrated formula.
 *
 * Platform : Qualcomm Oryon V2, Snapdragon 8 Elite, Android 15, kernel 6.6
 * Requires : root, perf_event_paranoid=-1, tracefs at /sys/kernel/debug/tracing
 *
 * Build : make bwprobe
 * Run   : make runprobe      (CSV → /tmp/bwprobe.csv, analysis → stderr)
 *
 * Output
 *   stdout — CSV with one row per working-set size:
 *              ws_MB, elapsed_s, bw_hwmon_MBs, [bw_<evt>_MBs, ratio_<evt>] ...
 *   stderr — human-readable sweep table and final conclusion
 */

#include "common.h"
#include <math.h>
#include <pthread.h>

/* --------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */
#define CPU_PIN          6       /* Oryon V2 Prime; bwmon-llcc-prime covers cpu6-7 */
#define MEASURE_SECS     5.0
#define WARMUP_SECS      0.5
#define DRAM_WS_MIN_MB   128    /* WS >= this is confirmed DRAM-resident on Oryon V2 */
                                /* 64 MB still partially cache-resident (bwmon ~30 MB/s) */
#define TRACE_BASE       "/sys/kernel/debug/tracing"
#define SYSFS_PMU        "/sys/bus/event_source/devices/armv8_pmuv3/events"

/* --------------------------------------------------------------------------
 * Event table
 *
 * Every event is opened with exclude_kernel=0.  On Qualcomm Oryon V2 this
 * is mandatory: cache refill operations are attributed to kernel context,
 * causing 23-33x undercounting with exclude_kernel=1.
 *
 * Fields:
 *   sysfs   — armv8_pmuv3/events/<sysfs> name to read actual config from;
 *             NULL means use PERF_TYPE_HARDWARE with the hardcoded config.
 *   csv     — column name emitted in the CSV header.
 *   type    — perf_event_attr.type value.
 *   config  — initial value; overwritten from sysfs at startup if available.
 * ------------------------------------------------------------------------- */
#define PERF_TYPE_HW  1   /* PERF_TYPE_HARDWARE */
#define ARMV8_PMU    10   /* confirmed via /sys/bus/event_source/devices/armv8_pmuv3/type */
#define HW_CACHE_MISSES_ID 3  /* PERF_COUNT_HW_CACHE_MISSES */

typedef struct {
    /* identity */
    const char *label;
    const char *sysfs;
    const char *csv;
    uint32_t    type;
    uint64_t    config;
    /* runtime */
    int         fd;
    int         ok;           /* 1 = perf_event_open succeeds on this device */
    /* accuracy stats accumulated over DRAM-regime WS points */
    double      sum_ratio;    /* sum of (pmu_bw_est / hwmon_bw) */
    int         n_dram;       /* number of valid DRAM-regime data points */
} Evt;

static Evt evts[] = {
  /* Runtime fields (fd, ok, sum_ratio, n_dram) are zero-initialised here;
   * fd is always overwritten inside measure_ws() before any use.            */
  { .label="l2d_cache",       .sysfs="l2d_cache",          .csv="l2d_cache",       .type=ARMV8_PMU,   .config=0x0016 },
  { .label="l2d_cache_refill",.sysfs="l2d_cache_refill",   .csv="l2d_refill",      .type=ARMV8_PMU,   .config=0x0017 },
  { .label="l1d_cache_refill",.sysfs="l1d_cache_refill",   .csv="l1d_refill",      .type=ARMV8_PMU,   .config=0x0003 },
  { .label="HW_CACHE_MISSES", .sysfs=NULL,                  .csv="hw_cache_misses", .type=PERF_TYPE_HW,.config=HW_CACHE_MISSES_ID },
  { .label="bus_access",      .sysfs="bus_access",          .csv="bus_access",      .type=ARMV8_PMU,   .config=0x0019 },
  { .label="l2d_lmiss_rd",   .sysfs="l2d_cache_lmiss_rd",  .csv="l2d_lmiss_rd",   .type=ARMV8_PMU,   .config=0x4009 },
  { .label="l3d_cache_alloc", .sysfs="l3d_cache_allocate",  .csv="l3d_alloc",      .type=ARMV8_PMU,   .config=0x0029 },
};
#define N_EVTS ((int)(sizeof(evts)/sizeof(evts[0])))

/* --------------------------------------------------------------------------
 * Hardware DRAM monitor (bw_hwmon_meas) — background thread
 *
 * Enables the bw_hwmon_meas tracepoint and reads trace_pipe in non-blocking
 * mode, parsing lines from "bwmon-llcc-prime" (the Prime cluster, cpu6-7).
 * Accumulates MB/s samples so measure_ws() can take a before/after snapshot.
 * ------------------------------------------------------------------------- */
typedef struct {
    volatile int    running;
    pthread_mutex_t lock;
    double          sum;     /* sum of mbps samples seen */
    long            n;       /* number of samples */
    int             available;
} Hwmon;

static void *hwmon_thread(void *arg)
{
    Hwmon *hw = arg;

    /* Enable tracepoint */
    int fd = open(TRACE_BASE "/events/dcvs/bw_hwmon_meas/enable", O_WRONLY);
    if (fd < 0) { hw->available = 0; hw->running = 0; return NULL; }
    write(fd, "1", 1); close(fd);
    fd = open(TRACE_BASE "/tracing_on", O_WRONLY);
    if (fd >= 0) { write(fd, "1", 1); close(fd); }

    hw->available = 1;

    int pfd = open(TRACE_BASE "/trace_pipe", O_RDONLY | O_NONBLOCK);
    if (pfd < 0) { hw->running = 0; return NULL; }

    char raw[8192], line[512];
    int lp = 0;

    while (hw->running) {
        ssize_t n = read(pfd, raw, sizeof(raw));
        if (n <= 0) { usleep(2000); continue; }
        for (ssize_t i = 0; i < n; i++) {
            char c = raw[i];
            if (c == '\n' || lp == (int)sizeof(line) - 2) {
                line[lp] = '\0';
                lp = 0;
                if (strstr(line, "bw_hwmon_meas") &&
                    strstr(line, "bwmon-llcc-prime")) {
                    char *p = strstr(line, "mbps");
                    if (p) {
                        unsigned long long v = 0;
                        if (sscanf(p, "mbps = %llu", &v) == 1) {
                            pthread_mutex_lock(&hw->lock);
                            hw->sum += (double)v;
                            hw->n++;
                            pthread_mutex_unlock(&hw->lock);
                        }
                    }
                }
            } else {
                line[lp++] = c;
            }
        }
    }

    close(pfd);
    fd = open(TRACE_BASE "/events/dcvs/bw_hwmon_meas/enable", O_WRONLY);
    if (fd >= 0) { write(fd, "0", 1); close(fd); }
    return NULL;
}

/* --------------------------------------------------------------------------
 * PMU helpers
 * ------------------------------------------------------------------------- */

/* Load event config from sysfs (overrides hardcoded ARM spec fallback). */
static void load_sysfs_configs(void)
{
    for (int i = 0; i < N_EVTS; i++) {
        if (!evts[i].sysfs) continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", SYSFS_PMU, evts[i].sysfs);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char buf[64];
        unsigned long long v;
        if (fgets(buf, sizeof(buf), f) && sscanf(buf, "event=0x%llx", &v) == 1)
            evts[i].config = (uint64_t)v;
        fclose(f);
    }
}

static int open_evt(const Evt *e)
{
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.type           = e->type;
    a.config         = e->config;
    a.size           = sizeof(a);
    a.disabled       = 1;
    a.exclude_kernel = 0;  /* MANDATORY on Oryon V2 */
    a.exclude_hv     = 1;
    return (int)perf_event_open(&a, 0, -1, -1, 0);
}

/* --------------------------------------------------------------------------
 * Measurement: one working-set size
 * ------------------------------------------------------------------------- */
static void measure_ws(size_t ws, Hwmon *hw,
                       uint64_t *counts,    /* [N_EVTS] out */
                       double   *bw_hwmon,  /* MB/s average from hardware monitor */
                       double   *elapsed,
                       uint64_t *hops)
{
    uintptr_t *buf = build_chase(ws);
    if (!buf) { fprintf(stderr, "OOM at %zu MB\n", ws >> 20); return; }
    prefault(buf, ws);

    /* Open counters for available events */
    for (int i = 0; i < N_EVTS; i++) {
        evts[i].fd = evts[i].ok ? open_evt(&evts[i]) : -1;
    }

    /* Warm up — not measured */
    run_chase(buf, WARMUP_SECS);

    /* Snapshot hwmon before measurement */
    pthread_mutex_lock(&hw->lock);
    double s0 = hw->sum;  long n0 = hw->n;
    pthread_mutex_unlock(&hw->lock);

    /* Reset and start all counters atomically */
    for (int i = 0; i < N_EVTS; i++) {
        if (evts[i].fd < 0) continue;
        ioctl(evts[i].fd, PERF_EVENT_IOC_RESET,  0);
        ioctl(evts[i].fd, PERF_EVENT_IOC_ENABLE, 0);
    }

    double t0 = now_s();
    *hops     = run_chase(buf, MEASURE_SECS);
    *elapsed  = now_s() - t0;

    for (int i = 0; i < N_EVTS; i++)
        if (evts[i].fd >= 0) ioctl(evts[i].fd, PERF_EVENT_IOC_DISABLE, 0);

    /* Read counts then close */
    for (int i = 0; i < N_EVTS; i++) {
        counts[i] = 0;
        if (evts[i].fd >= 0) {
            read(evts[i].fd, &counts[i], sizeof(counts[i]));
            close(evts[i].fd);
            evts[i].fd = -1;
        }
    }

    /* hwmon average over the measurement window */
    pthread_mutex_lock(&hw->lock);
    double s1 = hw->sum;  long n1 = hw->n;
    pthread_mutex_unlock(&hw->lock);

    long nn = n1 - n0;
    *bw_hwmon = (nn >= 3) ? (s1 - s0) / (double)nn : -1.0;

    free(buf);
}

/* --------------------------------------------------------------------------
 * Final conclusion
 * ------------------------------------------------------------------------- */
static void print_conclusion(int hwmon_ok)
{
    fprintf(stderr,
        "\n"
        "=================================================================\n"
        " CONCLUSION: Best CPU DRAM Measurement Method\n"
        "=================================================================\n\n");

    /* Ground truth */
    if (hwmon_ok) {
        fprintf(stderr,
            "  [GROUND TRUTH]  bw_hwmon_meas tracepoint\n"
            "    Path   : /sys/kernel/debug/tracing/events/dcvs/bw_hwmon_meas\n"
            "    Device : bwmon-llcc-prime  (CPU6-7 cluster)\n"
            "    Field  : mbps  (hardware-measured DRAM bandwidth)\n"
            "    Rate   : ~4 ms sampling, cluster-level only\n"
            "    Accuracy: hardware-exact — this IS the DRAM monitor\n\n");
    } else {
        fprintf(stderr, "  [GROUND TRUTH]  bw_hwmon_meas — NOT AVAILABLE on this device\n\n");
    }

    /* Find best PMU event (closest mean ratio to 1.0 at DRAM WS) */
    int    best     = -1;
    double best_err = 1e9;
    for (int i = 0; i < N_EVTS; i++) {
        if (!evts[i].ok || evts[i].n_dram == 0) continue;
        double mean = evts[i].sum_ratio / evts[i].n_dram;
        double err  = fabs(mean - 1.0);
        if (err < best_err) { best_err = err; best = i; }
    }

    /* Per-event accuracy table */
    fprintf(stderr, "  PMU event accuracy at WS >= %d MB (vs bw_hwmon ground truth):\n\n",
            DRAM_WS_MIN_MB);
    fprintf(stderr, "  %-20s  %10s  %5s  %s\n",
            "Event", "mean ratio", "pts", "Verdict");
    fprintf(stderr, "  %-20s  %10s  %5s  %s\n",
            "--------------------", "----------", "-----", "-------");

    for (int i = 0; i < N_EVTS; i++) {
        if (!evts[i].ok) {
            fprintf(stderr, "  %-20s  %10s  %5s  not available on device\n",
                    evts[i].label, "—", "—");
            continue;
        }
        if (!hwmon_ok || evts[i].n_dram == 0) {
            fprintf(stderr, "  %-20s  %10s  %5s  no hwmon reference\n",
                    evts[i].label, "—", "0");
            continue;
        }
        double mean = evts[i].sum_ratio / evts[i].n_dram;
        const char *verdict;
        if      (fabs(mean - 1.0) < 0.15) verdict = "ACCURATE";
        else if (mean < 0.5)               verdict = "undercount";
        else if (mean > 5.0)               verdict = "severe overcount (not a DRAM counter)";
        else if (fabs(mean - 1.0) < 0.5)  verdict = "usable with correction";
        else                               verdict = "inaccurate";

        fprintf(stderr, "  %-20s  %9.2fx  %5d  %s%s\n",
                evts[i].label, mean, evts[i].n_dram,
                verdict, (i == best) ? "  <-- BEST" : "");
    }

    /* Best PMU event summary */
    if (best >= 0 && hwmon_ok) {
        double mean = evts[best].sum_ratio / evts[best].n_dram;
        fprintf(stderr,
            "\n"
            "  Best per-process method: %s (config=0x%04llx)\n"
            "  Mean ratio at WS >= %d MB: %.2fx  (%.0f%% error)\n"
            "\n"
            "  Formula:\n"
            "    BW_MB_s = count * 64 / elapsed_s / 1e6\n"
            "    perf_event_attr.exclude_kernel = 0  (mandatory)\n"
            "    pin process to CPU6 (Prime core)\n"
            "\n"
            "  Constraint:\n"
            "    Only accurate when working set >> total cache (~24 MB on this SoC).\n"
            "    Below that threshold the event counts cache traffic, not DRAM.\n",
            evts[best].label,
            (unsigned long long)evts[best].config,
            DRAM_WS_MIN_MB,
            mean, fabs(mean - 1.0) * 100.0);
    }

    fprintf(stderr,
        "\n"
        "  Decision guide:\n"
        "    Need per-process attribution AND WS >> 24 MB → %s\n"
        "    Any other case                               → bw_hwmon_meas.mbps\n"
        "=================================================================\n",
        (best >= 0) ? evts[best].label : "(no PMU event accurate enough)");
}

/* --------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */
int main(void)
{
    pin_to_cpu(CPU_PIN);

    fprintf(stderr,
        "=================================================================\n"
        " bwprobe — CPU DRAM bandwidth measurement probe\n"
        " Device : Snapdragon 8 Elite (Oryon V2), pinned to CPU%d\n"
        "=================================================================\n\n",
        CPU_PIN);

    /* Step 1: load configs from sysfs, fall back to hardcoded ARM spec values */
    load_sysfs_configs();

    /* Step 2: probe which events open successfully */
    fprintf(stderr, "[ PMU event availability ]\n");
    for (int i = 0; i < N_EVTS; i++) {
        int fd = open_evt(&evts[i]);
        if (fd >= 0) {
            evts[i].ok = 1;
            close(fd);
            fprintf(stderr, "  + %-20s  type=%-2u  config=0x%04llx\n",
                    evts[i].label, evts[i].type,
                    (unsigned long long)evts[i].config);
        } else {
            evts[i].ok = 0;
            fprintf(stderr, "  - %-20s  UNAVAILABLE (%s)\n",
                    evts[i].label, strerror(errno));
        }
    }
    fprintf(stderr, "\n");

    /* Step 3: start hardware DRAM monitor background thread */
    Hwmon hw = { .running = 1, .sum = 0.0, .n = 0, .available = 0 };
    pthread_mutex_init(&hw.lock, NULL);
    pthread_t tid;
    pthread_create(&tid, NULL, hwmon_thread, &hw);
    sleep(1);  /* allow first bw_hwmon_meas samples to arrive */

    if (hw.available)
        fprintf(stderr, "[ bw_hwmon_meas tracepoint: ACTIVE (ground truth) ]\n\n");
    else
        fprintf(stderr, "[ bw_hwmon_meas tracepoint: NOT AVAILABLE — ratios will not be computed ]\n\n");

    /* Step 4: emit CSV header */
    printf("ws_MB,elapsed_s,bw_hwmon_MBs");
    for (int i = 0; i < N_EVTS; i++)
        if (evts[i].ok)
            printf(",bw_%s_MBs,ratio_%s", evts[i].csv, evts[i].csv);
    printf("\n");

    /* Step 5: stderr sweep table header */
    fprintf(stderr, "%-8s  %-12s", "WS(MB)", "hwmon(MB/s)");
    for (int i = 0; i < N_EVTS; i++)
        if (evts[i].ok)
            fprintf(stderr, "  %-24s", evts[i].label);
    fprintf(stderr, "\n%-8s  %-12s", "--------", "------------");
    for (int i = 0; i < N_EVTS; i++)
        if (evts[i].ok)
            fprintf(stderr, "  %-24s", "(est MB/s : ratio)");
    fprintf(stderr, "\n");

    /* Step 6: working-set sweep */
    static const size_t ws_mb[] = { 2, 4, 8, 16, 32, 64, 128, 256 };
    static const int    n_ws    = 8;

    for (int w = 0; w < n_ws; w++) {
        size_t   ws = ws_mb[w] * 1024 * 1024;
        uint64_t counts[N_EVTS];
        double   bw_hwmon, elapsed;
        uint64_t hops;

        measure_ws(ws, &hw, counts, &bw_hwmon, &elapsed, &hops);

        /* CSV row */
        printf("%zu,%.3f,%.1f", ws_mb[w], elapsed,
               bw_hwmon > 0 ? bw_hwmon : -1.0);
        for (int i = 0; i < N_EVTS; i++) {
            if (!evts[i].ok) continue;
            double bw    = (double)counts[i] * CACHE_LINE / elapsed / 1e6;
            double ratio = (bw_hwmon > 0) ? bw / bw_hwmon : -1.0;
            printf(",%.1f,%.4f", bw, ratio);
        }
        printf("\n");
        fflush(stdout);

        /* Stderr row */
        if (bw_hwmon > 0)
            fprintf(stderr, "%-8zu  %-12.0f", ws_mb[w], bw_hwmon);
        else
            fprintf(stderr, "%-8zu  %-12s", ws_mb[w], "(no hwmon)");

        for (int i = 0; i < N_EVTS; i++) {
            if (!evts[i].ok) continue;
            double bw    = (double)counts[i] * CACHE_LINE / elapsed / 1e6;
            double ratio = (bw_hwmon > 0) ? bw / bw_hwmon : -1.0;
            char cell[25];
            if (bw_hwmon > 0)
                snprintf(cell, sizeof(cell), "%.0f (%.2fx)", bw, ratio);
            else
                snprintf(cell, sizeof(cell), "%.0f MB/s", bw);
            fprintf(stderr, "  %-24s", cell);

            /* Accumulate stats only for confirmed DRAM-regime points:
             * WS >= DRAM_WS_MIN_MB AND bw_hwmon shows real workload BW (>1 GB/s).
             * Guards against background-only bwmon readings (~30 MB/s) at small WS. */
            if (bw_hwmon > 1000.0 && (int)ws_mb[w] >= DRAM_WS_MIN_MB) {
                evts[i].sum_ratio += ratio;
                evts[i].n_dram++;
            }
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }

    /* Step 7: stop hwmon thread */
    hw.running = 0;
    pthread_join(tid, NULL);
    pthread_mutex_destroy(&hw.lock);

    /* Step 8: print conclusion */
    print_conclusion(hw.available);

    return 0;
}
