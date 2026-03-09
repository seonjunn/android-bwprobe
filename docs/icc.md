# DCVS and the icc_set_bw Tracepoint — SM8750 (Snapdragon 8 Elite)

The Linux kernel interconnect framework (`icc`), Qualcomm DCVS, and the
`interconnect/icc_set_bw` tracepoint as observed on the Samsung Galaxy S25+
(Oryon V2, Android 15, kernel 6.6).

---

## Part I — Theoretical Model

### 1.1 The Linux Interconnect Framework

The Linux kernel **interconnect framework** (`drivers/interconnect/`) provides
a unified API for on-chip bus bandwidth and latency negotiation. Subsystem
drivers declare their bandwidth requirements by calling:

```c
icc_set_bw(path, avg_bw_kBps, peak_bw_kBps)
```

The framework aggregates votes from all clients for each path and programs the
interconnect hardware accordingly. On Qualcomm SoCs, this framework is backed
by the **DCVS** (Dynamic Clock and Voltage Scaling) governor.

Key distinction from hardware monitors: ICC reports **software votes** — what
drivers *request* — not hardware measurements. The `bw_hwmon` hardware unit
(see `docs/hwmon.md`) measures what actually transits the DRAM bus; ICC is
derived from that measurement with a ~40 ms aggregation lag.

### 1.2 DCVS and the DDR Frequency Loop

On SM8750, DDR frequency is governed by DCVS in a closed loop:

```
bw_hwmon HW unit (LLCC→DRAM, ~4 ms window)
  ↓ driver reads mbps, applies utilization model
DCVS selects DDR OPP (bandwidth operating point)
  ↓ calls icc_set_bw() on behalf of the cluster's ICC path
icc_set_bw tracepoint fires (path=llcc_mc-ebi, cadence ~40 ms)
  ↓ ICC framework aggregates all clients' votes at ebi node
LPDDR5X controller programmed to new frequency
```

The ~40 ms vote cadence is the DCVS aggregation period — much longer than
the 4 ms bwmon hardware window. ICC votes represent a time-smoothed estimate
of demand, not instantaneous bandwidth.

### 1.3 Node Topology on SM8750

From `/sys/kernel/debug/interconnect/` on SM8750 (Android 15):

**Fabric nodes (shared infrastructure):**

| Node | Hardware Entity | Role |
|---|---|---|
| `qns_llcc` | LLCC QoS slave port | Destination for **all** on-chip masters accessing the LLCC |
| `llcc_mc` | LLCC Memory Controller | Source of LLCC → DRAM path |
| `ebi` | External Bus Interface (LPDDR5X ctrl) | Destination: LLCC → DRAM path |

**ICC clients (source nodes, per processor):**

| Client node | Processor | ICC coverage |
|---|---|---|
| `chm_apps` | CPU clusters (Prime + Gold) | Yes — `chm_apps → qns_llcc` |
| `remoteproc-cdsp` | Hexagon NPU / Compute DSP | Yes — registers ICC path to LLCC |
| `remoteproc-adsp` | Audio DSP | Yes — registers ICC path to LLCC |
| `ae00000.qcom,mdss_mdp` | Display engine (MDP) | Yes — display scanout reads |
| `aa00000.qcom,vidc` | Video codec (Iris) | Yes — decode/encode buffer I/O |
| `1d84000.ufshc` | UFS Host Controller (storage) | Yes — storage DMA |
| `3d00000.qcom,kgsl-3d0` | Adreno GPU | **No** — compute bypasses ICC via GMU |

**Key property of `qns_llcc`:** Because it is the shared destination for all
non-GPU on-chip masters, its `agg_avg` field aggregates bandwidth votes from
every registered ICC client — CPU, NPU/DSP, display, audio, video, and storage.
`icc_llcc_agg` (the `agg_avg` at `qns_llcc`) is therefore **not a CPU-only
metric**. It reflects total non-GPU LLCC demand from the entire SoC.

The full traffic path:
```
CPU clusters (chm_apps) ──┐
NPU/DSP (remoteproc-cdsp) ┤
Display (mdss_mdp)        ├──→ qns_llcc [agg_avg = total non-GPU LLCC demand]
Audio DSP (remoteproc-adsp)┤        ↓ LLCC hit → served from LLCC
Video / UFS               ┘        ↓ LLCC miss →
                                llcc_mc → ebi [agg_avg = total DRAM demand]
                                        ↓ LPDDR5X DRAM
GPU (Adreno via GMU) ─────────────────────────────────────→ (bypasses ICC entirely)
```

### 1.4 icc_set_bw Tracepoint Mechanics

Each `icc_set_bw()` call fires **two tracepoint events** — one for the source
node and one for the destination node of the path:

```
interconnect:icc_set_bw:
  path=chm_apps-qns_llcc  node=chm_apps   avg_bw=... agg_avg=...  ← source event
interconnect:icc_set_bw:
  path=chm_apps-qns_llcc  node=qns_llcc   avg_bw=... agg_avg=...  ← destination event
```

All bandwidth values are in **kBps**.

| Field | Meaning |
|---|---|
| `path` | `<src>-<dst>` identifier for this ICC path |
| `node` | The node being updated in this specific event (src or dst) |
| `avg_bw` | This DCVS client's average bandwidth vote |
| `peak_bw` | This DCVS client's peak bandwidth vote |
| `agg_avg` | Aggregate of **all** clients' avg votes at this node |
| `agg_peak` | Aggregate of all clients' peak votes at this node |

`agg_avg` at the destination node (`qns_llcc` or `ebi`) represents the total
demand seen by that interconnect node from all clients. This is what DCVS
actually programs into hardware — use `agg_avg`, not `avg_bw`, for comparison
with hardware measurements.

### 1.5 ICC vs. bwmon: Expected Relationship

At DRAM-saturating WS, the expected relationship is:
```
bwmon ≈ icc_dram_agg / 1e3   (converting kBps to MB/s)
```

`icc_dram_agg` (`agg_avg` at `ebi`) aggregates all ICC clients' DRAM demand
votes. Because DCVS derives its votes from `bwmon` measurements, the two should
track each other — with some lag and smoothing due to the 40 ms vote period.

`icc_llcc_agg` and `icc_dram_agg` are **sequential hops**, not parallel paths.
LLCC demand (`qns_llcc`) is a superset of DRAM demand (`ebi`): some fraction
of L2 misses hit the LLCC and never reach DRAM. Adding the two values
double-counts the DRAM portion.

### 1.6 Why GPU KGSL Bypasses ICC

KGSL (Kernel Graphics Support Layer) does not use the Linux ICC framework for
Adreno GPU compute traffic. The GPU uses the **GMU** (Graphics Management Unit),
a dedicated hardware power management block that programs DDR frequency directly
via a separate path. GPU DRAM bandwidth therefore does **not** appear in
`icc_set_bw` events during GPU compute workloads.

---

## Part II — Implementation

### 2.1 Enabling the Tracepoint

```sh
echo 1 > /sys/kernel/debug/tracing/events/interconnect/icc_set_bw/enable
echo 1 > /sys/kernel/debug/tracing/tracing_on
```

Requires root and tracefs at `/sys/kernel/debug/tracing`. The ICC tracepoint
shares `trace_pipe` with `bw_hwmon_meas`; both are read by `hwmon_thread` in a
single consuming-reader thread (see `docs/hwmon.md` §2.2).

### 2.2 Parsing and Filtering (src/hwmon.h)

Naively matching on `chm_apps` is insufficient (see §3.1). The correct filter
requires both path and destination node:

```c
int is_icc_llcc = strstr(line, "path=chm_apps-qns_llcc") &&
                  strstr(line, "node=qns_llcc");
int is_icc_dram = strstr(line, "path=llcc_mc-ebi") &&
                  strstr(line, "node=ebi");

if (is_icc_llcc || is_icc_dram) {
    unsigned long long avg_v = 0, agg_v = 0;
    sscanf(strstr(line, "avg_bw="),  "avg_bw=%llu",  &avg_v);
    sscanf(strstr(line, "agg_avg="), "agg_avg=%llu", &agg_v);
    // accumulate avg_v into sum_icc_{llcc,dram}_avg
    // accumulate agg_v into sum_icc_{llcc,dram}_agg
}
```

The dual filter (path + node) is necessary to:
1. Eliminate the source-node event of the same `icc_set_bw()` call (which
   would double-count the update).
2. Avoid matching unrelated paths that share a node name substring (e.g.,
   `chm_apps-qhs_i2c` for I2C peripheral traffic).

### 2.3 avg_bw vs. agg_avg

- **`avg_bw`**: One DCVS client's vote. If the CPU DCVS client has the highest
  vote, `avg_bw ≈ agg_avg`. If other clients (display, DSP, audio) dominate,
  `avg_bw ≪ agg_avg`.
- **`agg_avg`**: Aggregate across all ICC clients at that node. Use this for
  comparison with `bwmon`.

### 2.4 Snapshot API

ICC accumulators are exposed via `hwmon_snap_icc()`:

```c
double llcc_avg, llcc_agg, dram_avg, dram_agg;
long   n_la, n_lg, n_da, n_dg;
hwmon_snap_icc(&hw,
    &llcc_avg, &n_la,
    &llcc_agg, &n_lg,
    &dram_avg, &n_da,
    &dram_agg, &n_dg);
```

Mean bandwidth over a window: `(agg1 - agg0) / (n1 - n0)` in kBps; divide by
1000 for MB/s. Use `min_samples=1` for ICC (fewer events per rep due to 40 ms
cadence; ~12 events per 5 s window vs. ~1,250 for bwmon).

### 2.5 Startup Delay

ICC events take longer to appear than bwmon events after tracepoint enable:
- bwmon: fires within the first 4 ms window.
- ICC: first event arrives at the next DCVS vote, up to ~40 ms after startup.

Wait `sleep(2)` after `pthread_create` before starting the first measurement
window to ensure a valid ICC baseline.

### 2.6 Trace File Schema

Written by `hwmon_thread` to `Hwmon.trace_icc_fp`:

```
t_s,node,avg_kBps,agg_kBps
0.041234,qns_llcc,6773792,6850592
0.041290,ebi,5442816,5516288
```

- `t_s`: Elapsed seconds since `Hwmon.trace_t0`.
- `node`: Destination node (`qns_llcc` or `ebi`).
- `avg_kBps`: Single client's vote (kBps).
- `agg_kBps`: All-client aggregate at this node (kBps).

---

## Part III — Experimental Findings

### 3.1 The Double-Count Bug (Fixed)

The original `hwmon.h` filtered ICC lines with `strstr(line, "chm_apps")`.
This matched all three problematic cases:
1. Both the source (`node=chm_apps`) and destination (`node=qns_llcc`) events
   of the same `icc_set_bw()` call → 2× count.
2. Unrelated paths like `chm_apps-qhs_i2c` (I2C peripheral traffic).

After applying the dual filter (path + destination node), counts dropped to
the expected ~half of the naive count. The fix is in `hwmon_parse_line()`.

### 3.2 icc_llcc and icc_dram Are Sequential, Not Parallel

Before experiments, it was unclear whether `icc_llcc_agg` and `icc_dram_agg`
were measuring parallel or sequential traffic. Empirical verification from
neon_matvec at WS=16 MB (LLCC-resident regime):

```
icc_llcc_agg ≈ bw_alg   (L2 miss rate ≈ algorithmic BW, all misses hit LLCC)
icc_dram_agg ≈ 0        (no DRAM traffic)
```

And in the DRAM regime:
```
icc_dram = 0.70 × icc_llcc   (30% LLCC absorption, constant)
```

This confirms that `qns_llcc` measures all L2 misses entering the LLCC, while
`ebi` measures only LLCC misses that proceed to DRAM. They are the same data
traffic at two successive hop points, not two separate streams. **Adding them
is wrong.**

### 3.3 ICC Undercounting Model

ICC systematically undercounts bwmon. The undercounting has two components:

**Component 1 — Background floor:** Non-CPU clients (display, audio DSP, etc.)
submit ICC votes that appear in `icc_dram_agg` regardless of the CPU workload.
This creates a constant background offset.

**Component 2 — DCVS smoothing:** DCVS averages bwmon samples over its ~40 ms
vote window and applies a utilization model, producing a vote that lags and
smooths the instantaneous bwmon measurement. At high bandwidth, the lag causes
the vote to trail the actual hardware measurement.

Global OLS fit across all workloads and WS (DRAM regime only):
```
icc_dram_agg = 0.812 × bwmon + 1,057 MB/s   (R²=0.978)
```

- Slope 0.812: DCVS captures 81% of bwmon on average.
- Intercept 1,057 MB/s: background ICC floor from display/audio/DSP clients.
- Crossover (icc_dram_agg = bwmon): at bwmon* ≈ 5,615 MB/s.

Worst-case undercount: neon_axpy at WS=128 MB, `icc_dram / bus_access = 0.483`
(52% undercount relative to the true R+W metric).

### 3.4 LLCC Absorption Is Constant at 30%

```
icc_dram_agg = 0.70 × icc_llcc_agg   (±0.1% across all workloads, all WS ≥ 16 MB)
```

This constancy is surprising: the theoretical expectation is that LLCC
absorption would vary with WS (higher WS → less LLCC benefit → less
absorption). In practice, the 30% absorption rate is stable across chase,
matvec, and axpy at DRAM-saturating WS. This likely reflects the LLCC's
steady-state miss rate under high-throughput streaming pressure, not workload
access pattern differences.

### 3.5 icc_llcc_agg Tracks bwmon at LLCC-Resident WS

At WS=16 MB with neon_matvec (LLCC-resident, streaming):
```
icc_llcc_agg / bw_alg ≈ 1.007   (near-unity)
icc_dram_agg ≈ 0
```

DCVS sees all L2 misses arriving at the LLCC and votes accordingly. Because no
DRAM traffic is generated, `icc_dram_agg` is near zero. This validates the
interpretation of `icc_llcc_agg` as the total L2 miss bandwidth entering the
shared subsystem (LLCC).

### 3.6 Metric Boundary and Contention Use

The two ICC metrics sit at different points in the memory hierarchy and serve
different purposes:

**`icc_dram_agg` (at `ebi`) — wrong boundary for contention characterization.**
It measures the LLCC→DRAM path. Any L2 miss absorbed by the LLCC — traffic
that does cause LLCC contention — is invisible here by construction. The miss
is structurally, not a precision or cadence issue: even a perfect zero-latency
measurement at `ebi` would correctly report zero DRAM traffic for an
LLCC-resident workload. `icc_dram_agg` is useful for DDR DCVS analysis and
system-level DRAM demand estimation, not for characterizing CPU-induced
shared-subsystem contention.

**`icc_llcc_agg` (at `qns_llcc`) — correct boundary, lower precision, SoC-wide scope.**
`qns_llcc` is the shared LLCC destination node for all non-GPU on-chip masters.
Its `agg_avg` aggregates bandwidth votes from every ICC client — CPU clusters,
Hexagon NPU/DSP, display, audio, video, and storage. `icc_llcc_agg` is not a
CPU-only metric. For a CPU-only workload with all other processors idle, it
approximates CPU L2 miss bandwidth (plus idle background). For mixed workloads
it reflects total non-GPU LLCC pressure.

When compared to `bus_access` (CPU process only), the relationship is:

```
icc_llcc_agg  ≈  Σ bus_access (all processes, all CPU clusters)
                 + NPU/DSP LLCC traffic + display + audio + ...
```

For CPU-only characterization, `bus_access` is the per-process, workload-isolated
version of the CPU component of `icc_llcc_agg`. Where they differ:

| | `bus_access` | `icc_llcc_agg` |
|---|---|---|
| Scope | Per-process (workload only) | All CPU clusters (Prime + Gold) |
| Background | Excluded | Included (Gold cluster + all CPU processes) |
| Precision | High (~4 ms hardware counter) | Lower (~40 ms DCVS EMA) |
| Low-BW sensitivity | Resolves ~85 MB/s cleanly | May not resolve against background floor |

For workload characterization (ranking kernels by memory intensity), `bus_access`
is preferred: stable, per-workload, no background contamination. For runtime
system-level CPU memory monitoring, `icc_llcc_agg` is the available proxy when
per-process PMU is not accessible.

**Background inclusion tradeoff.** Including background traffic (as `icc_llcc_agg`
does) makes the metric more accurate for predicting actual runtime contention
(the system is never idle; Gold cluster and background processes always consume
some LLCC bandwidth). However, background fluctuates across runs, making
cross-run and cross-system comparisons less reliable. For intrinsic kernel
characterization, background is noise; for scheduler/runtime decisions, it is
signal.

### 3.7 Accuracy vs. bwmon (icc_bw experiment, n=31 DRAM reps)

| Metric | Ratio to bwmon | 95% CI | Pearson r |
|---|---|---|---|
| `icc_dram_agg` | 0.924 ± 0.107 | [0.885, 0.963] | 0.9966 |
| `icc_llcc_agg` | 1.316 ± 0.148 | — | — |

The `icc_llcc_agg > bwmon` result (ratio ~1.32) is explained by the sequential
topology: `icc_llcc_agg` includes all L2 misses (including those absorbed by
the LLCC), while bwmon counts only misses that reached DRAM.

### 3.8 Pandas Merge Recipe

```python
import pandas as pd

icc = pd.read_csv("data/bw_bench_trace_icc.csv")
# columns: t_s, node, avg_kBps, agg_kBps

icc_llcc = icc[icc.node == "qns_llcc"].copy()
icc_dram = icc[icc.node == "ebi"].copy()

icc_llcc["icc_llcc_agg_MBs"] = icc_llcc["agg_kBps"] / 1e3
icc_dram["icc_dram_agg_MBs"] = icc_dram["agg_kBps"] / 1e3

# Align with bwmon (4 ms cadence) using nearest-timestamp merge
bwmon = pd.read_csv("data/bw_bench_trace_bwmon.csv")
merged = pd.merge_asof(
    bwmon.sort_values("t_s"),
    icc_dram[["t_s","icc_dram_agg_MBs"]].sort_values("t_s"),
    on="t_s", direction="nearest", tolerance=0.1
)
```
