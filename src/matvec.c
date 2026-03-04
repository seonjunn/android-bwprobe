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

#include "common.h"
#include <math.h>
#include <pthread.h>
#include <arm_neon.h>

/* --------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */
#define CPU_PIN         6       /* Oryon V2 Prime; bwmon-llcc-prime covers cpu6-7 */
#define M_ROWS          4096    /* y = M*4 = 16 KB -- always L1D-resident */
#define MEASURE_SECS    3.0     /* per-rep measurement window (each kernel) */
#define N_REPS          7       /* independent trials per WS point */
#define FLUSH_SIZE      (24 * 1024 * 1024)  /* > LLCC (12 MB) */
#define TRACE_BASE      "/sys/kernel/debug/tracing"
#define SKIN_ZONE_PATH  "/sys/class/thermal/thermal_zone53/temp"

/* --------------------------------------------------------------------------
 * Hardware DRAM monitor -- identical to bwprobe.c
 * ------------------------------------------------------------------------- */
typedef struct {
    volatile int    running;
    pthread_mutex_t lock;
    double          sum;
    long            n;
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
                line[lp] = '\0'; lp = 0;
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
 * Scalar matvec kernel — autovectorized by compiler (NEON FMLA on AArch64)
 *
 * noinline: prevents the compiler from merging consecutive kernel calls,
 * which would collapse A reads across iterations of the pass loop.
 * restrict: informs the compiler that A, x, y have no aliasing.
 * ------------------------------------------------------------------------- */
__attribute__((noinline))
static void matvec_scalar(const float * restrict A,
                          const float * restrict x,
                          float       * restrict y,
                          int M, int N)
{
    for (int i = 0; i < M; i++) {
        const float * restrict row = A + (size_t)i * N;
        float s = 0.0f;
        for (int j = 0; j < N; j++)
            s += row[j] * x[j];
        y[i] = s;
    }
}

/* --------------------------------------------------------------------------
 * NEON matvec kernel — 8 float32x4 accumulators, 32-wide inner unroll
 *
 * Each iteration processes 32 floats (128 bytes = 2 cache lines) from A
 * and x simultaneously.  8 independent accumulators hide FMA latency and
 * keep the load-execute pipeline saturated.
 * ------------------------------------------------------------------------- */
__attribute__((noinline))
static void matvec_neon(const float * restrict A,
                        const float * restrict x,
                        float       * restrict y,
                        int M, int N)
{
    for (int i = 0; i < M; i++) {
        const float *row = A + (size_t)i * N;
        float32x4_t acc0 = vdupq_n_f32(0.0f), acc1 = vdupq_n_f32(0.0f);
        float32x4_t acc2 = vdupq_n_f32(0.0f), acc3 = vdupq_n_f32(0.0f);
        float32x4_t acc4 = vdupq_n_f32(0.0f), acc5 = vdupq_n_f32(0.0f);
        float32x4_t acc6 = vdupq_n_f32(0.0f), acc7 = vdupq_n_f32(0.0f);
        int j = 0;
        for (; j <= N - 32; j += 32) {
            acc0 = vfmaq_f32(acc0, vld1q_f32(row + j     ), vld1q_f32(x + j     ));
            acc1 = vfmaq_f32(acc1, vld1q_f32(row + j +  4), vld1q_f32(x + j +  4));
            acc2 = vfmaq_f32(acc2, vld1q_f32(row + j +  8), vld1q_f32(x + j +  8));
            acc3 = vfmaq_f32(acc3, vld1q_f32(row + j + 12), vld1q_f32(x + j + 12));
            acc4 = vfmaq_f32(acc4, vld1q_f32(row + j + 16), vld1q_f32(x + j + 16));
            acc5 = vfmaq_f32(acc5, vld1q_f32(row + j + 20), vld1q_f32(x + j + 20));
            acc6 = vfmaq_f32(acc6, vld1q_f32(row + j + 24), vld1q_f32(x + j + 24));
            acc7 = vfmaq_f32(acc7, vld1q_f32(row + j + 28), vld1q_f32(x + j + 28));
        }
        /* Reduce 8 accumulators */
        acc0 = vaddq_f32(vaddq_f32(acc0, acc1), vaddq_f32(acc2, acc3));
        acc4 = vaddq_f32(vaddq_f32(acc4, acc5), vaddq_f32(acc6, acc7));
        float s = vaddvq_f32(vaddq_f32(acc0, acc4));
        /* Scalar tail for N not divisible by 32 */
        for (; j < N; j++) s += row[j] * x[j];
        y[i] = s;
    }
}

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
    struct perf_event_attr pa;
    memset(&pa, 0, sizeof(pa));
    pa.type           = ARMV8_PMU_TYPE;
    pa.config         = 0x0019;         /* bus_access */
    pa.size           = sizeof(pa);
    pa.disabled       = 1;
    pa.exclude_kernel = 0;              /* mandatory on Oryon V2 */
    pa.exclude_hv     = 1;
    int bus_fd = (int)perf_event_open(&pa, 0, -1, -1, 0);
    int bus_ok = (bus_fd >= 0);

    for (int r = 0; r < N_REPS; r++) {
        res[r].skin_c = read_skin_c();

        /* ---- Flush LLCC then run scalar measurement ---- */
        flush_caches(FLUSH_SIZE);

        /* Scalar warmup: one full pass to load A into LLCC */
        matvec_scalar(A, x, y, M, N);

        /* hwmon snapshot before scalar */
        pthread_mutex_lock(&hw->lock);
        double s0 = hw->sum;  long n0 = hw->n;
        pthread_mutex_unlock(&hw->lock);

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
        pthread_mutex_lock(&hw->lock);
        double s1 = hw->sum;  long n1 = hw->n;
        pthread_mutex_unlock(&hw->lock);

        long   nn     = n1 - n0;
        double hw_bw  = (nn >= 3) ? (s1 - s0) / (double)nn : -1.0;
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
        pthread_mutex_lock(&hw->lock);
        double s0n = hw->sum;  long n0n = hw->n;
        pthread_mutex_unlock(&hw->lock);

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
        pthread_mutex_lock(&hw->lock);
        double s1n = hw->sum;  long n1n = hw->n;
        pthread_mutex_unlock(&hw->lock);

        long   nnn      = n1n - n0n;
        double hw_bwn   = (nnn >= 3) ? (s1n - s0n) / (double)nnn : -1.0;
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
    Hwmon hw = { .running = 1, .sum = 0.0, .n = 0, .available = 0 };
    pthread_mutex_init(&hw.lock, NULL);
    pthread_t tid;
    pthread_create(&tid, NULL, hwmon_thread, &hw);
    sleep(1);

    if (hw.available)
        fprintf(stderr, "[ bw_hwmon_meas tracepoint: ACTIVE (ground truth) ]\n\n");
    else
        fprintf(stderr, "[ bw_hwmon_meas tracepoint: NOT AVAILABLE ]\n\n");

    /* Probe bus_access availability */
    {
        struct perf_event_attr pa;
        memset(&pa, 0, sizeof(pa));
        pa.type = ARMV8_PMU_TYPE; pa.config = 0x0019;
        pa.size = sizeof(pa); pa.disabled = 1;
        pa.exclude_kernel = 0; pa.exclude_hv = 1;
        int fd = (int)perf_event_open(&pa, 0, -1, -1, 0);
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
    pthread_mutex_destroy(&hw.lock);

    return 0;
}
