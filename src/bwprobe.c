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
 *   stdout — CSV with one row per (working-set size, rep):
 *              ws_MB, rep, elapsed_s,
 *              bw_hwmon_prime_MBs, bw_hwmon_gold_MBs,
 *              hops, chase_GBs, lat_ns, skin_c,
 *              [bw_<evt>_MBs, ratio_<evt>] ...
 *   stderr — human-readable mean±stddev sweep table and final conclusion
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
#define NREPS            7       /* independent trials per WS point */
#define FLUSH_SIZE       (24 * 1024 * 1024)  /* > LLCC (12 MB) — evicts stale WS */
#define LATENCY_HOPS     (1ULL << 20)        /* 1M hops via CNTVCT_EL0 */
#define DRAM_WS_MIN_MB   128    /* WS >= this is confirmed DRAM-resident on Oryon V2 */
#define TRACE_BASE       "/sys/kernel/debug/tracing"
#define SYSFS_PMU        "/sys/bus/event_source/devices/armv8_pmuv3/events"

/* Thermal — zone 53 = sys-therm-0 (skin/board surface) on Samsung Galaxy S25+ */
#define SKIN_ZONE_PATH   "/sys/class/thermal/thermal_zone53/temp"

/* --------------------------------------------------------------------------
 * Event table
 *
 * Every event is opened with exclude_kernel=0.  On Qualcomm Oryon V2 this
 * is mandatory: cache refill operations are attributed to kernel context,
 * causing 23-33x undercounting with exclude_kernel=1.
 * ------------------------------------------------------------------------- */
#define PERF_TYPE_HW  1   /* PERF_TYPE_HARDWARE */
#define ARMV8_PMU    10   /* confirmed via /sys/bus/event_source/devices/armv8_pmuv3/type */
#define HW_CACHE_MISSES_ID 3  /* PERF_COUNT_HW_CACHE_MISSES */

typedef struct {
    const char *label;
    const char *sysfs;
    const char *csv;
    uint32_t    type;
    uint64_t    config;
    int         fd;
    int         ok;
    double      sum_ratio;
    int         n_dram;
} Evt;

static Evt evts[] = {
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
 * Tracks both "bwmon-llcc-prime" (cpu6-7, Prime cluster) and
 * "bwmon-llcc-gold" (cpu0-5, Gold cluster) simultaneously.
 * ------------------------------------------------------------------------- */
typedef struct {
    volatile int    running;
    pthread_mutex_t lock;
    double          sum_prime;   /* MB/s accumulator — Prime cluster */
    long            n_prime;
    double          sum_gold;    /* MB/s accumulator — Gold cluster */
    long            n_gold;
    int             available;
} Hwmon;

static void *hwmon_thread(void *arg)
{
    Hwmon *hw = arg;

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
                if (strstr(line, "bw_hwmon_meas")) {
                    int is_prime = (strstr(line, "bwmon-llcc-prime") != NULL);
                    int is_gold  = (strstr(line, "bwmon-llcc-gold")  != NULL);
                    if (is_prime || is_gold) {
                        char *p = strstr(line, "mbps");
                        if (p) {
                            unsigned long long v = 0;
                            if (sscanf(p, "mbps = %llu", &v) == 1) {
                                pthread_mutex_lock(&hw->lock);
                                if (is_prime) { hw->sum_prime += (double)v; hw->n_prime++; }
                                if (is_gold)  { hw->sum_gold  += (double)v; hw->n_gold++;  }
                                pthread_mutex_unlock(&hw->lock);
                            }
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
 * Thermal helper
 * ------------------------------------------------------------------------- */
static double read_skin_c(void)
{
    FILE *f = fopen(SKIN_ZONE_PATH, "r");
    if (!f) return -1.0;
    int raw = 0;
    fscanf(f, "%d", &raw);
    fclose(f);
    return raw / 1000.0;
}

/* --------------------------------------------------------------------------
 * Per-rep result
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t counts[N_EVTS];
    double   bw_hwmon_prime;
    double   bw_hwmon_gold;
    double   elapsed;
    uint64_t hops;
    double   lat_ns;      /* ns/hop from CNTVCT_EL0, fixed 1M hops */
    double   skin_c;
} BwResult;

/* --------------------------------------------------------------------------
 * Measurement: NREPS reps for one working-set size
 *
 * Per rep:
 *   1. flush_caches(24 MB) — evict LLCC to establish clean starting state
 *   2. warmup (run_chase for WARMUP_SECS) — re-load WS into LLCC
 *   3. chase_latency_ns(1M hops) — per-hop latency via CNTVCT (no syscall)
 *   4. hwmon snapshot before
 *   5. PMU reset + enable; run_chase for MEASURE_SECS; PMU disable + read
 *   6. hwmon snapshot after → compute prime + gold BW
 * ------------------------------------------------------------------------- */
static void measure_ws_multi(size_t ws, Hwmon *hw, BwResult res[NREPS])
{
    uintptr_t *buf = build_chase(ws);
    if (!buf) { fprintf(stderr, "OOM at %zu MB\n", ws >> 20); return; }
    prefault(buf, ws);

    for (int r = 0; r < NREPS; r++) {
        res[r].skin_c = read_skin_c();

        /* Evict LLCC, then warm up this WS */
        flush_caches(FLUSH_SIZE);
        run_chase(buf, WARMUP_SECS);

        /* Latency measurement — no PMU overhead, CNTVCT only */
        res[r].lat_ns = chase_latency_ns(buf, LATENCY_HOPS);

        /* Open PMU counters fresh each rep */
        for (int i = 0; i < N_EVTS; i++)
            evts[i].fd = evts[i].ok ? open_evt(&evts[i]) : -1;

        /* hwmon snapshot before BW measurement */
        pthread_mutex_lock(&hw->lock);
        double s0p = hw->sum_prime; long n0p = hw->n_prime;
        double s0g = hw->sum_gold;  long n0g = hw->n_gold;
        pthread_mutex_unlock(&hw->lock);

        /* Reset and start all PMU counters */
        for (int i = 0; i < N_EVTS; i++) {
            if (evts[i].fd < 0) continue;
            ioctl(evts[i].fd, PERF_EVENT_IOC_RESET,  0);
            ioctl(evts[i].fd, PERF_EVENT_IOC_ENABLE, 0);
        }

        double t0      = now_s();
        res[r].hops    = run_chase(buf, MEASURE_SECS);
        res[r].elapsed = now_s() - t0;

        for (int i = 0; i < N_EVTS; i++)
            if (evts[i].fd >= 0) ioctl(evts[i].fd, PERF_EVENT_IOC_DISABLE, 0);

        for (int i = 0; i < N_EVTS; i++) {
            res[r].counts[i] = 0;
            if (evts[i].fd >= 0) {
                read(evts[i].fd, &res[r].counts[i], sizeof(res[r].counts[i]));
                close(evts[i].fd);
                evts[i].fd = -1;
            }
        }

        /* hwmon snapshot after */
        pthread_mutex_lock(&hw->lock);
        double s1p = hw->sum_prime; long n1p = hw->n_prime;
        double s1g = hw->sum_gold;  long n1g = hw->n_gold;
        pthread_mutex_unlock(&hw->lock);

        long nnp = n1p - n0p;
        long nng = n1g - n0g;
        res[r].bw_hwmon_prime = (nnp >= 3) ? (s1p - s0p) / (double)nnp : -1.0;
        res[r].bw_hwmon_gold  = (nng >= 3) ? (s1g - s0g) / (double)nng : -1.0;
    }

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

    if (hwmon_ok) {
        fprintf(stderr,
            "  [GROUND TRUTH]  bw_hwmon_meas tracepoint\n"
            "    Path   : /sys/kernel/debug/tracing/events/dcvs/bw_hwmon_meas\n"
            "    Devices: bwmon-llcc-prime (CPU6-7), bwmon-llcc-gold (CPU0-5)\n"
            "    Field  : mbps  (hardware-measured DRAM bandwidth)\n"
            "    Rate   : ~4 ms sampling, cluster-level only\n"
            "    Accuracy: hardware-exact — this IS the DRAM monitor\n\n");
    } else {
        fprintf(stderr, "  [GROUND TRUTH]  bw_hwmon_meas — NOT AVAILABLE on this device\n\n");
    }

    int    best     = -1;
    double best_err = 1e9;
    for (int i = 0; i < N_EVTS; i++) {
        if (!evts[i].ok || evts[i].n_dram == 0) continue;
        double mean = evts[i].sum_ratio / evts[i].n_dram;
        double err  = fabs(mean - 1.0);
        if (err < best_err) { best_err = err; best = i; }
    }

    fprintf(stderr, "  PMU event accuracy at WS >= %d MB (vs bw_hwmon_prime ground truth):\n\n",
            DRAM_WS_MIN_MB);
    fprintf(stderr, "  %-20s  %10s  %5s  %s\n",
            "Event", "mean ratio", "pts", "Verdict");
    fprintf(stderr, "  %-20s  %10s  %5s  %s\n",
            "--------------------", "----------", "-----", "-------");

    for (int i = 0; i < N_EVTS; i++) {
        if (!evts[i].ok) {
            fprintf(stderr, "  %-20s  %10s  %5s  not available on device\n",
                    evts[i].label, "-", "-");
            continue;
        }
        if (!hwmon_ok || evts[i].n_dram == 0) {
            fprintf(stderr, "  %-20s  %10s  %5s  no hwmon reference\n",
                    evts[i].label, "-", "0");
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
        "    Need per-process attribution AND WS >> 24 MB -> %s\n"
        "    Any other case                               -> bw_hwmon_meas.mbps\n"
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
        " bwprobe -- CPU DRAM bandwidth measurement probe\n"
        " Device : Snapdragon 8 Elite (Oryon V2), pinned to CPU%d\n"
        " %d reps x %.1f s per WS; LLCC flush + CNTVCT latency each rep\n"
        "=================================================================\n\n",
        CPU_PIN, NREPS, MEASURE_SECS);

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
    Hwmon hw = { .running = 1,
                 .sum_prime = 0.0, .n_prime = 0,
                 .sum_gold  = 0.0, .n_gold  = 0,
                 .available = 0 };
    pthread_mutex_init(&hw.lock, NULL);
    pthread_t tid;
    pthread_create(&tid, NULL, hwmon_thread, &hw);
    sleep(1);  /* allow first bw_hwmon_meas samples to arrive */

    if (hw.available)
        fprintf(stderr, "[ bw_hwmon_meas tracepoint: ACTIVE (prime + gold) ]\n\n");
    else
        fprintf(stderr, "[ bw_hwmon_meas tracepoint: NOT AVAILABLE -- ratios will not be computed ]\n\n");

    /* Step 4: emit CSV header */
    printf("ws_MB,rep,elapsed_s,bw_hwmon_prime_MBs,bw_hwmon_gold_MBs,"
           "hops,chase_GBs,lat_ns,skin_c");
    for (int i = 0; i < N_EVTS; i++)
        if (evts[i].ok)
            printf(",bw_%s_MBs,ratio_%s", evts[i].csv, evts[i].csv);
    printf("\n");

    /* Step 5: stderr sweep table header */
    fprintf(stderr, "%-8s  %-15s  %-13s  %-12s  %-8s",
            "WS(MB)", "prime(MB/s)", "gold(MB/s)", "chase(GB/s)", "lat(ns)");
    for (int i = 0; i < N_EVTS; i++)
        if (evts[i].ok)
            fprintf(stderr, "  %-20s", evts[i].label);
    fprintf(stderr, "\n%-8s  %-15s  %-13s  %-12s  %-8s",
            "--------", "mean +/- sd", "mean +/- sd", "mean +/- sd", "mean");
    for (int i = 0; i < N_EVTS; i++)
        if (evts[i].ok)
            fprintf(stderr, "  %-20s", "ratio (mean+/-sd)");
    fprintf(stderr, "\n");

    /* Step 6: working-set sweep */
    static const size_t ws_mb[] = {
        2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 24, 32, 48, 64, 80, 96, 112, 128, 160, 192, 256
    };
    static const int n_ws = 21;

    for (int w = 0; w < n_ws; w++) {
        size_t   ws = ws_mb[w] * 1024 * 1024;
        BwResult res[NREPS];

        measure_ws_multi(ws, &hw, res);

        /* Emit CSV rows (one per rep) and accumulate DRAM-regime stats */
        for (int r = 0; r < NREPS; r++) {
            double chase_GBs = (double)res[r].hops * CACHE_LINE / res[r].elapsed / 1e9;
            printf("%zu,%d,%.3f,%.1f,%.1f,%llu,%.3f,%.2f,%.1f",
                   ws_mb[w], r + 1, res[r].elapsed,
                   res[r].bw_hwmon_prime > 0 ? res[r].bw_hwmon_prime : -1.0,
                   res[r].bw_hwmon_gold  > 0 ? res[r].bw_hwmon_gold  : -1.0,
                   (unsigned long long)res[r].hops, chase_GBs,
                   res[r].lat_ns, res[r].skin_c);
            for (int i = 0; i < N_EVTS; i++) {
                if (!evts[i].ok) continue;
                double bw    = (double)res[r].counts[i] * CACHE_LINE / res[r].elapsed / 1e6;
                double ratio = (res[r].bw_hwmon_prime > 0) ? bw / res[r].bw_hwmon_prime : -1.0;
                printf(",%.1f,%.4f", bw, ratio);
            }
            printf("\n");

            /* Accumulate DRAM-regime PMU calibration stats */
            if (res[r].bw_hwmon_prime > 1000.0 && (int)ws_mb[w] >= DRAM_WS_MIN_MB) {
                for (int i = 0; i < N_EVTS; i++) {
                    if (!evts[i].ok) continue;
                    double bw    = (double)res[r].counts[i] * CACHE_LINE / res[r].elapsed / 1e6;
                    double ratio = bw / res[r].bw_hwmon_prime;
                    evts[i].sum_ratio += ratio;
                    evts[i].n_dram++;
                }
            }
        }
        fflush(stdout);

        /* Compute mean+/-stddev across reps for stderr summary */
        double prime_v[NREPS], gold_v[NREPS], chase_v[NREPS], lat_v[NREPS];
        for (int r = 0; r < NREPS; r++) {
            prime_v[r] = res[r].bw_hwmon_prime > 0 ? res[r].bw_hwmon_prime : 0.0;
            gold_v[r]  = res[r].bw_hwmon_gold  > 0 ? res[r].bw_hwmon_gold  : 0.0;
            chase_v[r] = (double)res[r].hops * CACHE_LINE / res[r].elapsed / 1e9;
            lat_v[r]   = res[r].lat_ns;
        }
        double pm = stat_mean(prime_v, NREPS), ps = stat_sd(prime_v, NREPS, pm);
        double gm = stat_mean(gold_v,  NREPS), gs = stat_sd(gold_v,  NREPS, gm);
        double cm = stat_mean(chase_v, NREPS), cs = stat_sd(chase_v, NREPS, cm);
        double lm = stat_mean(lat_v,   NREPS);

        char prime_str[24], gold_str[24], chase_str[24], lat_str[12];
        snprintf(prime_str, sizeof(prime_str), "%.0f +/- %.0f", pm, ps);
        snprintf(gold_str,  sizeof(gold_str),  "%.0f +/- %.0f", gm, gs);
        snprintf(chase_str, sizeof(chase_str), "%.2f +/- %.2f", cm, cs);
        snprintf(lat_str,   sizeof(lat_str),   "%.1f", lm);

        fprintf(stderr, "%-8zu  %-15s  %-13s  %-12s  %-8s",
                ws_mb[w], prime_str, gold_str, chase_str, lat_str);

        for (int i = 0; i < N_EVTS; i++) {
            if (!evts[i].ok) continue;
            double rv[NREPS];
            for (int r = 0; r < NREPS; r++) {
                double bw = (double)res[r].counts[i] * CACHE_LINE / res[r].elapsed / 1e6;
                rv[r] = (res[r].bw_hwmon_prime > 0) ? bw / res[r].bw_hwmon_prime : 0.0;
            }
            double rm = stat_mean(rv, NREPS), rs = stat_sd(rv, NREPS, rm);
            char cell[24];
            snprintf(cell, sizeof(cell), "%.2fx +/- %.2f", rm, rs);
            fprintf(stderr, "  %-20s", cell);
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
