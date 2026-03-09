# Memory Bandwidth Measurement — SM8750 (Snapdragon 8 Elite)

Overview of the hardware memory subsystem and the three available measurement
mechanisms on the Samsung Galaxy S25+ (Oryon V2, Android 15, kernel 6.6).
Detailed mechanism documentation is in the companion files:

- `docs/pmu.md` — ARMv8 PMU and the `bus_access` event (0x0019)
- `docs/hwmon.md` — Qualcomm `bw_hwmon` hardware monitor and `bw_hwmon_meas` tracepoint
- `docs/icc.md` — Linux ICC framework, DCVS, and the `icc_set_bw` tracepoint

---

## 1. Memory Hierarchy

```
CPU core (cpu6, Prime cluster, Oryon V2)
  L1D: 96 KB  — private per-core
  L2:  12 MB  — shared within Prime cluster (cpu6–7)
    ↑ bus_access fires here (PMU, per-process): all L2 misses, reads + writes
  LLCC: ~112 MB — shared SLC: all CPU clusters + GPU + DSP + display + audio
    ↑ bwmon fires here (HW monitor, cluster-level): DRAM reads only
  LPDDR5X DRAM (128-bit bus)
```

Cache parameters:

| Level | Size | Scope | Latency |
|---|---|---|---|
| L1D | 96 KB | Per-core | < 1 ns |
| L2 | 12 MB | Prime cluster (cpu6–7) | ~1 ns |
| LLCC | ~112 MB | All clusters + I/O masters | ~2.7 ns |
| DRAM | — | Global | ~5.5 ns |

The LLCC is the central shared resource. Contention for LLCC and DRAM bandwidth
is what matters for system-level performance interference. Any L2 miss (WS > 12
MB) enters the shared subsystem — even if LLCC-resident — so the L2 boundary
is the right place to measure workload pressure.

---

## 2. Measurement Mechanisms

Four metrics are available across three mechanisms. They differ in measurement
boundary, scope, and precision:

| Metric | Mechanism | Boundary | Counts | Scope | Cadence |
|---|---|---|---|---|---|
| `bus_access` | ARMv8 PMU (`docs/pmu.md`) | L2 → LLCC | Reads + writes (incl. prefetcher) | Per-process | ~4 ms (polled) |
| `icc_llcc_agg` | `icc_set_bw` tracepoint (`docs/icc.md`) | L2 → LLCC (DCVS vote) | Software estimate | All CPU clusters | ~40 ms |
| `bwmon` | `bw_hwmon_meas` tracepoint (`docs/hwmon.md`) | LLCC → DRAM | DRAM reads only | Cluster-level | ~4 ms HW |
| `icc_dram_agg` | `icc_set_bw` tracepoint (`docs/icc.md`) | LLCC → DRAM (DCVS vote) | Software estimate | System-wide | ~40 ms |

### The right boundary for contention measurement

L2 is private to the Prime cluster — traffic absorbed within L2 causes zero
contention with other processors. The shared contention domain begins at the
L2 output port, where misses enter the LLCC. Therefore the correct measurement
boundary for characterizing CPU-induced contention is **L2 → LLCC**, not
LLCC → DRAM.

This has a direct consequence for metric selection: `icc_dram_agg` is at the
wrong boundary. It misses all LLCC-resident workload pressure by construction —
if traffic is absorbed by the LLCC, there is nothing to measure at the
LLCC→DRAM boundary regardless of precision or cadence. It is only useful as a
DRAM-level system view or for DDR DCVS analysis.

The two metrics at the right boundary are `bus_access` and `icc_llcc_agg`.
They sit at the LLCC entry but have fundamentally different scopes:

- `bus_access` measures one CPU process's L2 misses — CPU-only, per-process.
- `icc_llcc_agg` (`agg_avg` at `qns_llcc`) aggregates bandwidth votes from
  **all ICC clients** at the LLCC entry: CPU clusters, Hexagon NPU/DSP, display
  engine, audio DSP, video codec, storage. It is a SoC-wide metric, not CPU-only.

For a CPU-only workload with all other processors idle, `icc_llcc_agg` ≈ total
CPU L2 miss traffic + irreducible background (display, audio). Subtracting the
idle baseline isolates the CPU workload contribution. For mixed workloads, it
reflects combined contention pressure from all non-GPU processors.

```
icc_llcc_agg  ≈  CPU (all clusters) + NPU/DSP + display + audio + storage
                 [minus GPU — bypasses ICC via GMU]
```

GPU (Adreno compute) is the one notable exclusion: it bypasses ICC entirely
via the GMU and appears in neither `icc_llcc_agg` nor `icc_dram_agg`.

---

## 3. Measurement Regimes

Which metrics fire depends on working set size **and temporal reuse pattern**:

| WS | Reuse? | Pattern | bus_access | bwmon | Notes |
|---|---|---|---|---|---|
| ≤ 12 MB | any | any | ≈ 0 | ≈ background | L2-resident; no shared-subsystem traffic |
| 12–112 MB | yes | random/looping | fires (low) | ≈ background | LLCC absorbs reused lines |
| 12–112 MB | no | streaming | fires | fires | No reuse → LLCC transparent |
| > 112 MB | any | any | fires | fires | WS exceeds LLCC capacity |

DRAM onset by workload (bw_bench data):
- Streaming (matvec, axpy): WS ≥ 16 MB — no temporal reuse; every line must come from DRAM.
- Random looping (chase): WS ≥ 128 MB — LLCC absorbs reused lines up to WS ~112 MB.

### 3.1 Why Temporal Reuse Determines LLCC Behavior

Whether the LLCC acts as a cache (absorbs traffic) or a transparent pipe
(every access still reaches DRAM) depends on one thing: whether the same cache
line is accessed again before it is evicted.

**With reuse — LLCC opaque:** the data is used more than once while it fits in
the LLCC. After the first cold miss brings the line from DRAM, subsequent
accesses are LLCC hits. bwmon stays quiet.

**Without reuse — LLCC transparent:** each cache line is consumed once and
discarded. `DRAM → LLCC → CPU`, then evicted. Every byte still traverses the
DRAM bus. bwmon fires even when WS ≪ LLCC capacity.

This is distinct from the question of whether WS *fits* in the LLCC. A 16 MB
working set easily fits in the 112 MB LLCC, but if each cache line in that 16
MB is accessed only once (single-pass streaming), the LLCC provides no benefit
and bwmon fires at full bandwidth.

### 3.2 Workload Examples — DNN Context

The same two regimes appear directly in neural network inference:

| DNN Operation | Reuse? | LLCC behavior | bwmon fires? |
|---|---|---|---|
| Batch=1 FC inference (large weight matrix) | No | Transparent | Yes — weights stream through DRAM on every call |
| Batched FC inference (weights fit in LLCC) | Yes (across batch) | Opaque after 1st sample | No — subsequent samples served from LLCC |
| Convolution (small filters) | Yes (filters reused per spatial position) | Opaque for filters | No for filter traffic |
| Attention KV-cache (fits in LLCC) | Yes (K/V reused across queries) | Opaque | No |
| Large activation tensor (single-pass) | No | Transparent | Yes |

**Practical consequence for profiling:** a DNN deployment where the model fits
in the LLCC and runs repeatedly (e.g., continuous inference) can have
significant LLCC pressure (`bus_access` fires) while bwmon stays near zero.
Using bwmon alone would conclude "no memory pressure" — which is wrong.
`bus_access` is necessary to see this.

---

## 4. Metric Relationships

### 4.1 bus_access vs. icc_llcc_agg — Same Boundary, Different Granularity

Both sit at the LLCC entry boundary. They differ in scope and precision:

```
[L2 miss — workload process on CPU]
    ↓
bus_access fires  (per-process, CPU only)

[Any LLCC-bound request — CPU, NPU, DSP, display, audio, storage]
    ↓
icc_llcc_agg updated  (SoC-wide aggregate via DCVS vote; GPU excluded)
```

| Dimension | bus_access | icc_llcc_agg |
|---|---|---|
| Scope | Per-process (one CPU workload) | All non-GPU ICC clients (CPU + NPU/DSP + display + audio + storage) |
| Background | Excluded | Included (all active ICC clients) |
| Precision | High (hardware counter, ~4 ms) | Lower (DCVS EMA, ~40 ms, amplitude sensitivity at low BW) |

**Prefetcher note:** Both metrics include hardware prefetcher traffic
(bus_access counts it directly; icc_llcc_agg reflects it through the DCVS
vote). This is correct for contention characterization — prefetcher fetches
compete for LLCC bandwidth just as demand fetches do. The amplification
(`bus_access / bw_alg ≈ 1.9×` for streaming) should be noted when comparing
against algorithmic throughput, but it is not a measurement artifact.

### 4.2 bus_access vs. bwmon — Different Boundaries

```
bus_access fires  (L2 → LLCC boundary)
    ↓ LLCC lookup
    ├── LLCC hit  → LLCC serves it; bwmon does NOT fire
    └── LLCC miss → bwmon fires  (LLCC → DRAM boundary, reads only)
```

bwmon is the hardware ground truth for DRAM reads. It is complementary to
bus_access but at a downstream boundary and read-only. For contention
characterization it is insufficient: bwmon is blind to LLCC-resident workloads
(LLCC hit path), and is cluster-level (cannot isolate workload from background).

### 4.3 icc_llcc_agg vs. icc_dram_agg — Sequential Hops, Not Parallel

These two ICC metrics are at sequential points on the same data path. Do not
add them.

```
icc_dram_agg = 0.70 × icc_llcc_agg   (30% LLCC absorption, constant across workloads)
```

`icc_llcc_agg` (at `qns_llcc`) = all L2 misses entering the LLCC.
`icc_dram_agg` (at `ebi`) = the 70% of those that miss the LLCC and reach DRAM.

`icc_dram_agg` is at the wrong boundary for contention characterization: it
structurally misses LLCC-resident pressure regardless of measurement precision.
It is retained only for DRAM-level system analysis and DDR DCVS correlation.

---

## 5. Metric Selection Guide

### For CPU contention characterization

The primary choice is between `bus_access` and `icc_llcc_agg`. Both are at the
correct boundary (L2→LLCC). The tradeoff is precision vs. background inclusion:

| | `bus_access` | `icc_llcc_agg` |
|---|---|---|
| **Precision** | High — hardware counter, ~4 ms, per-process | Lower — DCVS EMA, ~40 ms, may not resolve low-BW workloads |
| **Background** | Excluded — workload signal only | Included — all CPU clusters (Gold + Prime) |
| **Use for** | Characterizing a specific workload's intrinsic memory intensity | System-level CPU memory pressure (all processes combined) |
| **Cross-processor** | CPU only (no GPU/NPU analog) | CPU clusters only; does not cover GPU (GMU bypass) |

**Recommendation:**
- Use `bus_access` as the primary metric for workload characterization. It gives a stable, reproducible, per-workload score — the kernel's intrinsic memory pressure — without background contamination.
- Use `icc_llcc_agg` where per-process PMU is unavailable, or as a system-level corroboration. Interpret it as "total CPU cluster memory pressure" rather than per-workload.
- The background floor in `icc_llcc_agg` adds noise that makes workload-to-workload comparison less reliable across different system states. It is more appropriate for runtime contention monitoring than offline kernel characterization.

**Robustness across workload regimes:** The selection of `bus_access` and `icc_llcc_agg` as the two candidates is stable regardless of whether workloads are LLCC-resident or streaming-transparent.

- In the *streaming regime* (LLCC transparent — matvec, matmul), `bwmon` becomes a valid read-only ground truth and `icc_dram_agg` loses its boundary objection. However, the two candidates do not lose their advantages: `bus_access` still counts writes (e.g., `bus_access / bwmon = 1.56` for AXPY) and is still per-process; `icc_llcc_agg` was already at the correct boundary. In this regime `icc_dram_agg ≈ 0.70 × icc_llcc_agg` (fixed 30% LLCC absorption) — a constant scaling, not equivalence. `bwmon` is useful as a corroborating read-only ground truth but does not displace `bus_access`.
- In the *LLCC-resident regime* (looping random access), `bwmon` and `icc_dram_agg` are blind; only `bus_access` and `icc_llcc_agg` capture the workload.

The metric ranking is therefore robust: the same two metrics remain the correct choices across the full workload space.

### For other goals

| Goal | Metric | Reason |
|---|---|---|
| DRAM read bandwidth (ground truth) | `bwmon` | Hardware measurement at LLCC→DRAM |
| DDR DCVS vote / DDR scaling analysis | `icc_dram_agg` | Software vote at DRAM boundary |
| Algorithmic demand throughput | `bw_alg` | Demand only, no prefetcher, any WS |
| Background system DRAM load | `bwmon − bus_access` (approx.) | Cluster total minus per-process workload |

### Cross-processor contention (CPU + GPU + NPU)

No single metric covers all processors. On SM8750:
- GPU compute traffic bypasses ICC entirely (GMU path) → `icc_llcc_agg` and `icc_dram_agg` do not see it.
- DSP/NPU (CDSP) uses ICC → appears in `icc_dram_agg` at `ebi`, but not in `icc_llcc_agg` which is specific to the `chm_apps` CPU path.
- A complete cross-processor picture requires: `bus_access` (CPU workload) + kgsl GMU counters (GPU) + `icc_dram_agg` for DSP/display/audio, combined at the DRAM boundary.

---

## 6. Key Empirical Results

### bw_bench controlled results (WS=128 MB)

| Workload | bwmon (MB/s) | bus_access (MB/s) | bus/bwmon | Bound |
|---|---:|---:|---:|---|
| chase | 10,577 | 10,997 | 1.04 | Latency |
| scalar_matvec | 10,834 | 11,060 | 1.02 | FMA latency |
| neon_matvec | 100,666 | 106,777 | 1.06 | DRAM read BW |
| neon_axpy | 88,811 | 138,520 | 1.56 | DRAM R+W BW |

### icc_bw experiment (WS sweep, n=31 DRAM reps)

| Metric pair | Ratio to bwmon | 95% CI | Pearson r |
|---|---|---|---|
| `bus_access` | 1.052 ± 0.055 | [1.032, 1.073] | 0.9997 |
| `icc_dram_agg` | 0.924 ± 0.107 | [0.885, 0.963] | 0.9966 |

### Hardware prefetcher amplification

```
bwmon / bw_alg ≈ 1.90×   (scalar_matvec, neon_matvec — read-only streaming)
bwmon / bw_alg ≈ 1.25×   (neon_axpy — only 2 of 3 streams reach DRAM)
bus_access / bw_alg ≈ 1.90×   (consistent across all streaming workloads)
```
