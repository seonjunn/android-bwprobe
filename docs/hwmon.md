# bw_hwmon and the bw_hwmon_meas Tracepoint — SM8750 (Snapdragon 8 Elite)

The Qualcomm `bw_hwmon` hardware bandwidth monitor and its Linux kernel
tracepoint (`dcvs/bw_hwmon_meas`) as observed on the Samsung Galaxy S25+
(Oryon V2, Android 15, kernel 6.6).

---

## Part I — Theoretical Model

### 1.1 What bw_hwmon Is

`bw_hwmon` is a Qualcomm-proprietary hardware performance counter embedded in
the LLCC (Last Level Cache Controller). It counts byte transfers between the
LLCC and the DRAM controller over a fixed hardware measurement window,
producing a bandwidth sample (in MB/s) at the end of each window.

It is part of Qualcomm's **DCVS** (Dynamic Clock and Voltage Scaling) feedback
loop. The driver reads the hardware sample, applies a utilization model, and
feeds the result to the DDR DCVS governor, which adjusts LPDDR5X frequency
accordingly.

The measurement boundary:
```
CPU cluster (L2 output)
  ↓ L2 miss
LLCC (System Level Cache, ~112 MB)
  ↓ LLCC miss
[bw_hwmon unit]  ← measures bytes crossing this boundary
  ↓
LPDDR5X DRAM
```

Theoretical properties:
- Measures **DRAM reads only** — write-backs from LLCC to DRAM are not
  reflected in the reported `mbps` value.
- Is **cluster-level**: counts all traffic from all processes running on the
  measured cluster, including background and kernel activity.
- Only fires when traffic actually reaches DRAM; LLCC-resident traffic is
  architecturally invisible.
- Hardware measurement window is ~4 ms; one sample per window per instance.

### 1.2 Monitor Instances on SM8750

SM8750 exposes two `bw_hwmon` instances:

| Instance | Cluster | CPUs |
|---|---|---|
| `bwmon-llcc-prime` | Prime | cpu6–7 (Oryon V2) |
| `bwmon-llcc-gold` | Gold | cpu0–5 (Cortex-A720) |

Each instance reports the DRAM read bandwidth attributable to its respective
cluster's NOC (Network-on-Chip) master port. They are independent; traffic
from one cluster is not included in the other's counter.

GPU (Adreno), DSP (HexDSP), display engine (MDP), and video codec (Iris) each
have their own NOC paths to the DRAM controller that are **not** captured by
either CPU cluster bwmon instance.

### 1.3 The bw_hwmon_meas Tracepoint

The Linux kernel `bw_hwmon` driver exposes each hardware sample via the
`dcvs/bw_hwmon_meas` ftrace tracepoint. The tracepoint fires once per hardware
window per monitor instance — i.e., once per ~4 ms per cluster.

Event format:
```
<task>-<pid> [cpu] flags ... dcvs:bw_hwmon_meas:
  name=bwmon-llcc-prime dev=... mbps=10421 meas_bw=... ab=... ib=...
```

Fields:

| Field | Meaning |
|---|---|
| `name` | Monitor instance name (`bwmon-llcc-prime` or `bwmon-llcc-gold`) |
| `mbps` | **Hardware DRAM read bandwidth** for this window (MB/s) — primary metric |
| `meas_bw` | Driver's internal bandwidth estimate (DCVS-weighted) |
| `ab`, `ib` | Average and instantaneous bandwidth votes submitted to DDR DCVS |
| `dev` | Kernel device name of the DCVS provider |

Only `name` and `mbps` are used in this project. `meas_bw`, `ab`, and `ib`
carry DCVS internal state and are not meaningful for direct bandwidth measurement.

### 1.4 Role in the DCVS Feedback Loop

```
bw_hwmon HW counter (4 ms window)
  ↓ driver reads mbps
  ↓ applies utilization model → DDR OPP selection
DCVS submits icc_set_bw(path=llcc_mc-ebi, avg_bw=...)
  ↓ icc_set_bw tracepoint fires (~40 ms cadence)
LPDDR5X frequency programmed
```

`bwmon` is the **hardware ground truth** at the LLCC→DRAM boundary.
`icc_dram_agg` (from the `icc_set_bw` tracepoint) is the DCVS software vote
derived from bwmon with a ~40 ms lag. See `docs/icc.md` for the ICC side.

---

## Part II — Implementation

### 2.1 Enabling the Tracepoint

The tracepoint is enabled by writing `1` to its enable file in tracefs:

```sh
echo 1 > /sys/kernel/debug/tracing/events/dcvs/bw_hwmon_meas/enable
echo 1 > /sys/kernel/debug/tracing/tracing_on
```

Requires: root, tracefs mounted at `/sys/kernel/debug/tracing`.

### 2.2 Reading trace_pipe

`trace_pipe` is a **consuming reader**: bytes read from it are removed from the
kernel ring buffer and cannot be re-read by any process. This has two
important consequences:

1. Only one reader thread may exist per experiment — all tracepoints required
   (bwmon and ICC if needed) must be parsed in the same thread.
2. If the reader is slow relative to the event rate, events can be lost.

`hwmon_thread` opens `trace_pipe` with `O_RDONLY | O_NONBLOCK`. When no data
is available (read returns 0 or EAGAIN), it sleeps 2 ms and retries. It reads
in 8 KB chunks, assembles newline-terminated lines, and dispatches to
`hwmon_parse_line()`.

### 2.3 Parsing Logic (src/hwmon.h)

```c
if (strstr(line, "bw_hwmon_meas")) {
    int is_prime = strstr(line, "bwmon-llcc-prime") != NULL;
    int is_gold  = strstr(line, "bwmon-llcc-gold")  != NULL;
    if ((is_prime && (flags & HWMON_PRIME)) ||
        (is_gold  && (flags & HWMON_GOLD))) {
        const char *p = strstr(line, "mbps");
        unsigned long long v = 0;
        sscanf(p, "mbps = %llu", &v);
        // accumulate under mutex
    }
}
```

`"prime"` and `"gold"` are non-overlapping suffixes; the check order is
immaterial. The `mbps` field is parsed by scanning forward from its keyword.

### 2.4 Accumulator and Snapshot API

`hwmon.h` maintains a running sum and count per cluster:

```c
double sum_prime; long n_prime;
double sum_gold;  long n_gold;
```

Protected by `pthread_mutex_t lock`. The mean over a measurement window is:

```
mean_bw = (sum1 − sum0) / (n1 − n0)
```

where `sum0, n0` are snapshots taken before the workload via `hwmon_snap()`,
and `sum1, n1` are taken after. This is implemented in `hwmon_bw_window()`,
which returns -1.0 if fewer than `min_samples` new events arrived.

### 2.5 Feature Flags

`hwmon_init(hw, flags)` selects which monitors to track:

| Flag | Value | Effect |
|---|---|---|
| `HWMON_PRIME` | 0x01 | Track `bwmon-llcc-prime` |
| `HWMON_GOLD` | 0x02 | Also track `bwmon-llcc-gold` |
| `HWMON_ICC` | 0x04 | Also parse `icc_set_bw` from the same thread |

`HWMON_ICC` is bundled into `hwmon_thread` because `trace_pipe` is a consuming
reader — bwmon and ICC must share one reader thread.

### 2.6 Initialization Pattern

```c
Hwmon hw;
hwmon_init(&hw, HWMON_PRIME | HWMON_ICC);
hw.trace_bwmon_fp = fopen("trace_bwmon.csv", "w");
hw.trace_t0       = now_s();
pthread_t tid;
pthread_create(&tid, NULL, hwmon_thread, &hw);

sleep(2);   // allow trace_pipe to become live and ICC events to arrive

// ... measurement windows ...

hw.running = 0;
pthread_join(&tid, NULL);
hwmon_destroy(&hw);
```

The `sleep(2)` before starting workloads is important: `trace_pipe` may be
empty (or stale) immediately after enabling the tracepoint. ICC events in
particular take up to ~40 ms to appear; the sleep ensures a clean baseline.

### 2.7 Trace File Schema

Written by `hwmon_thread` to `Hwmon.trace_bwmon_fp` (bwmon-llcc-prime only):

```
t_s,bwmon_MBs
0.003812,10421.0
0.007894,10388.0
```

- `t_s`: Elapsed wall-clock seconds since `Hwmon.trace_t0` (`CLOCK_MONOTONIC`).
- `bwmon_MBs`: Hardware DRAM read bandwidth (`mbps` field) for this sample.

One row per `bw_hwmon_meas` event. ~250 rows/second. Sole-writer pattern;
no file lock needed.

---

## Part III — Experimental Findings

### 3.1 Cadence Is Consistently ~4 ms

The hardware measurement window and tracepoint cadence are ~4 ms per cluster.
Over a 5 s measurement window, expect ~1,250 events per instance. At stable
bandwidth, the CV (coefficient of variation) per rep is:

| Workload (WS=128 MB) | bwmon CV |
|---|---|
| neon_axpy | 0.2% |
| neon_matvec | 1.1% |
| chase | 8.7% |

The higher CV for chase reflects its inherently bursty DRAM access pattern,
not measurement noise.

### 3.2 LLCC Blind Spot Is Real and Workload-Dependent

The theoretical expectation — that bwmon only fires when WS > LLCC — is
confirmed experimentally, but the onset is **workload-dependent**, not purely
a function of WS vs. LLCC size:

| WS | Workload | bwmon | Explanation |
|---|---|---|---|
| 16 MB | neon_matvec | fires (~50 GB/s) | Streaming bypasses LLCC |
| 16 MB | chase | ≈ 0 | LLCC absorbs random misses |
| 64 MB | neon_matvec | fires (~90 GB/s) | Still streaming; no LLCC benefit |
| 64 MB | chase | ≈ background | LLCC still absorbs most misses |
| 128 MB | chase | fires (~10 GB/s) | WS > LLCC; all misses reach DRAM |

The root cause is **temporal reuse**: whether the LLCC absorbs an L2 miss
depends on whether the *same cache line* is accessed again before it gets
evicted, not on whether the working set fits within LLCC capacity.

**Chase (random, looping):** The workload loops over the same `ws_MB` of data
indefinitely. After the first pass loads data from DRAM into the LLCC, all
subsequent laps are LLCC hits — every miss is absorbed. bwmon stays quiet until
WS exceeds LLCC capacity (~112 MB) and there is simply no room to hold the
entire working set.

**Streaming (matvec, axpy — sequential, single-pass):** Each cache line is
read once and never again. The LLCC loads it from DRAM, the CPU consumes it,
and the LLCC immediately evicts it for the next incoming line. There is no
second access to reuse. The LLCC acts as a transparent staging buffer:
`DRAM → LLCC → CPU` with no caching benefit, regardless of WS vs. LLCC size.
bwmon fires as soon as WS > L2 (~16 MB), because every L2 miss must ultimately
come from DRAM.

This transparency is compounded by the hardware stream prefetcher (see
`docs/pmu.md` §1.5): for sequential patterns, the prefetcher issues requests
far ahead of the current position, keeping many DRAM fetches in flight
simultaneously. The LLCC is continuously draining from DRAM at the full
bandwidth of the stream.

#### Implications for DNN Workloads

The same dichotomy applies directly to neural network inference and training.

**No temporal reuse — LLCC transparent:**

- **Single-sample FC inference (batch size = 1).** A fully-connected layer
  multiplies a weight matrix `[M × K]` by a single input vector `[K]`. Each
  weight element is read exactly once per inference call, then discarded. If
  the weight matrix is large (e.g., 256 MB for a transformer layer), every
  weight element traverses DRAM → LLCC → CPU on every call. The LLCC provides
  no benefit; bwmon fires in full.

- **Large activation tensors, single forward pass.** Feature maps in a deep
  vision model may be written by layer N and read once by layer N+1. There
  is no second access between write and eviction.

**With temporal reuse — LLCC opaque:**

- **Batched inference (batch size > 1) with weights that fit in LLCC.** If
  weights are 50 MB (< 112 MB LLCC) and you run a batch of 8 samples, the
  first sample loads weights from DRAM; the remaining 7 samples are served
  from the LLCC. bwmon drops after the first sample warms the cache. The
  effective DRAM pressure per sample is 1/8 of the single-sample case.

- **Convolution filters.** A 3×3 filter (tiny, fits easily in L2) is applied
  to every spatial position of the feature map — thousands of reuses per
  layer. The filter stays hot in L2/LLCC while the (larger) input feature map
  streams through. Filter traffic never reaches DRAM.

- **Attention with a fixed KV-cache.** During autoregressive token generation,
  the key and value tensors from prior tokens are read by every new query. If
  the KV-cache fits in LLCC, each key/value line is loaded from DRAM once
  and reused across all query positions and attention heads.

**Practical consequence:** `bus_access` is the correct metric for all these
cases because it fires at the L2 boundary regardless of reuse pattern. bwmon
can only see the LLCC-transparent cases; it is silent for any workload whose
data fits in LLCC and has sufficient temporal locality.

### 3.3 bwmon Reads Only — Write-Backs Not Counted

The theoretical claim that bwmon counts DRAM reads only is confirmed by the
AXPY ratio:

```
bus_access / bwmon ≈ 1.56   (AXPY: 2R + 1W streams)
```

If bwmon counted writes, the ratio would be lower. The 3/2 ratio (3 algorithmic
streams vs. 2 measured DRAM reads) is consistent across all reps with very low
variance (CV = 0.2% for neon_axpy at WS=128 MB).

### 3.4 Spatial Burst Amplification

Expected: bwmon should equal algorithmic bandwidth (bw_alg). Observed:

```
bwmon / bw_alg ≈ 1.88×   (scalar_matvec, neon_matvec — read-only streaming)
bwmon / bw_alg ≈ 1.25×   (neon_axpy — only 2 of 3 streams are DRAM reads)
```

**Root cause: LPDDR5X minimum burst size (BL16 = 128B) combined with L2 thrashing.**

LPDDR5X specifies a minimum burst length of BL16 (Burst Length 16). With
SM8750's 64-bit bus: `BL16 × 8 bytes = 128 bytes per DRAM access`. Every
LLCC miss fetches 128B — the demanded 64B cache line plus the spatially
adjacent 64B line — regardless of demand size.

At WS = 128 MB with L2 = 12 MB, L2 thrashes at 10.7× capacity. The "bonus"
64B line from the burst arrives in L2 but is evicted before use. Result:
128B of DRAM traffic per 64B of useful demand → bwmon ≈ 2 × bw_alg in
theory, 1.88× in practice (partial spatial reuse reduces waste slightly).

For AXPY: bwmon counts only reads (2 streams: x, y_read). Each read stream
is amplified by 1.88×:

```
bwmon / bw_alg = 1.88 × (2 read streams / 3 total streams) = 1.25×
```

`MADV_RANDOM` (via `madvise`) has no measurable effect on the factor,
confirming it is driven by the hardware DRAM burst, not the OS page prefetcher.
The factor is stable across WS ≥ 128 MB for all streaming patterns.

**Note on bwmon exceeding theoretical DRAM peak:** The SM8750 DRAM peak is
~85 GB/s (Qualcomm product brief: 85.33 GB/s). The neon_matvec bwmon reading
(~100 GB/s) exceeds this. The discrepancy is unresolved: bwmon may capture
additional write traffic not documented in the "reads only" characterization,
or the actual achievable bandwidth under prefetcher-saturated streaming differs
from the rated peak. See `docs/pmu.md` §3.7 for discussion.

### 3.5 Cluster-Level Scope: Background Floor Matters

bwmon captures all processes on the cluster, not just the benchmark. On a
quiet Android device with the workload at LLCC-resident WS (no DRAM traffic
from the workload), the observed background floor is:

```
bwmon-llcc-prime (idle workload, WS=8 MB) ≈ 25 MB/s
```

At LLCC-resident WS with an active but moderate workload:
```
bus_access ≈ 85 MB/s    (workload only, per-process)
bwmon      ≈ 315 MB/s   (workload + ~230 MB/s system background)
```

At DRAM-saturating WS (WS=128 MB, neon_matvec, ~100 GB/s), background is
negligible relative to workload traffic.

### 3.6 LLCC Absorption Constant at 30%

At DRAM-saturating WS, the ratio between `icc_llcc_agg` (L2 miss BW entering
LLCC) and `icc_dram_agg` (LLCC miss BW reaching DRAM) is:

```
icc_dram = 0.70 × icc_llcc   (LLCC absorption = 30%)
```

This ratio is constant at 30% ± 0.1% across all workloads (chase, matvec,
axpy) and all WS ≥ 16 MB. It reflects the LLCC's steady-state miss rate under
high-BW conditions, not a workload-specific property.

### 3.7 Accuracy vs. ICC and bus_access

From the icc_bw experiment (n=31 DRAM reps, WS sweep {8,32,64,128,256} MB):

| Metric | Ratio to bwmon | 95% CI | Pearson r |
|---|---|---|---|
| `bus_access` | 1.052 ± 0.055 | [1.032, 1.073] | 0.9997 |
| `icc_dram_agg` | 0.924 ± 0.107 | [0.885, 0.963] | 0.9966 |

OLS fits:
```
bus_access   = 1.075 × bwmon − 387     (R²=0.9994)
icc_dram_agg = 0.863 × bwmon + 525     (R²=0.9932)
```

`bus_access` tracks bwmon with near-unity correlation. `icc_dram_agg` shows
systematic undercounting (~8%) relative to bwmon due to DCVS smoothing and
the ~40 ms vote lag (see `docs/icc.md` §3.3 for the full ICC undercounting model).

---

## References

- **Qualcomm SM8750-3-AB Product Brief** — DRAM peak bandwidth 85.33 GB/s
  (64-bit LPDDR5X bus):
  <https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/Snapdragon-8-Elite-SM8750-3-AB-Product-Brief.pdf>

- **PhoneDB SM8750-AC detailed specs** (corroborates 85.33 GB/s):
  <https://phonedb.net/index.php?m=processor&id=1027&c=qualcomm_snapdragon_8_elite_sm8750-ac___sun&d=detailed_specs>

- **Qualcomm `governor_bw_hwmon.c` kernel driver** — confirms `mbps` field is
  MB/s, computed as `bytes_to_mbps(get_bytes_and_clear(hwmon), us)`:
  - Facebook Portal kernel mirror:
    <https://github.com/facebookincubator/Portal-Kernel/blob/master/drivers/devfreq/governor_bw_hwmon.c>
  - Android/MSM kernel (older, same pattern):
    <https://android.googlesource.com/kernel/msm/+/android-7.1.0_r0.2/drivers/devfreq/governor_bw_hwmon.c>

- **JEDEC LPDDR5/5X specification (JESD209-5C)** — BL16 burst geometry:
  <https://www.jedec.org/sites/default/files/docs/JESD209-5C.pdf>
