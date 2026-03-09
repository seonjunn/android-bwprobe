# Memory Bandwidth Characterization — Full Report
## Snapdragon 8 Elite (Oryon V2), Samsung Galaxy S25+

---

## 1. Memory Hierarchy and Measurement Points

```
CPU core (cpu6, Prime cluster)
  L1D: 96 KB (private per core)
  L2:  12 MB (shared, Prime cluster)
    ← bus_access (PMU 0x0019) fires here — L2 misses leaving the CPU cluster
  LLCC: ~112 MB (shared, all clusters + display/audio/GPU)
    ← bw_hwmon_meas fires here — traffic reaching the DRAM controller
  LPDDR5X DRAM (128-bit bus)
```

`bus_access` and `bwmon` measure at **different hierarchy levels**:
- `bus_access`: CPU-side traffic entering the shared subsystem (LLCC/DRAM path)
- `bwmon`: traffic that reaches DRAM controller (reads only)

The LLCC sits between them, absorbing ~30% of all L2 misses before they reach DRAM.

The contention target is the **entire shared memory subsystem** (LLCC + DRAM), not DRAM
alone. All processors (CPU clusters, GPU, DSP, display) share both the LLCC and the DRAM
controller. A CPU workload with WS = 64 MB causes significant LLCC pressure (bus_access
fires) even though bwmon ≈ 0. `bus_access` captures this; `bwmon` does not.

## 2. Metric Definitions

| Metric | What it measures | Regime | Read/Write |
|---|---|---|---|
| `bwmon` (bw_hwmon_meas) | DRAM controller read bandwidth | WS > LLCC (~112 MB) | reads only |
| `bus_access` (PMU 0x0019) | CPU-side traffic toward LLCC/DRAM | CPU working sets beyond L2 residency | reads + writes |
| `icc_llcc_agg` | DCVS vote: total CPU→LLCC demand | WS > L2 | reads + writes |
| `icc_dram_agg` | DCVS vote: LLCC→DRAM demand | WS > LLCC | reads + writes |
| `bw_alg` | Demand traffic / elapsed (computed) | any WS | demand only |

---

## 3. ICC Node Topology (SM8750)

```
CPU → [chm_apps] → qns_llcc → [llcc_mc] → ebi → DRAM
                   ↑                       ↑
             icc_llcc_agg            icc_dram_agg
           (total L2 miss BW)    (LLCC miss BW only)
```

`icc_set_bw` fires **two events** per update (source and destination node).
Filter by destination only to avoid double-counting:

| Filter | Metric captured |
|---|---|
| `path=chm_apps-qns_llcc` + `node=qns_llcc` | `icc_llcc_agg` |
| `path=llcc_mc-ebi` + `node=ebi` | `icc_dram_agg` |

Two fields per event (kBps):
- `avg_bw` — one DCVS client's vote (partial view)
- `agg_avg` — aggregate across all active ICC clients at that node (total node demand)

DCVS client here means a bandwidth-voting requester on the interconnect path
(for example CPU path, display, DSP, video, storage).

**Relationship between icc_llcc and icc_dram:**
```
icc_dram = (1 − LLCC_hit_rate) × icc_llcc
         ≈ 0.70 × icc_llcc    (empirical: 30% LLCC absorption, constant across workloads/WS)
```
These are sequential hops, NOT parallel paths. Adding them double-counts the DRAM portion.

**Verification from bw_bench data (WS=128 MB):**

| Workload | icc_llcc_agg | icc_dram_agg | icc_dram/icc_llcc |
|---|---:|---:|---:|
| chase | 14,349 | 10,070 | 0.702 |
| scalar_matvec | 13,647 | 9,579 | 0.702 |
| neon_matvec | 129,346 | 90,568 | 0.700 |
| neon_axpy | 95,470 | 66,855 | 0.700 |

**Verification of icc_llcc ≈ bw_alg in LLCC regime:**

At WS=16 MB (L2 misses → LLCC, mostly served by LLCC), `icc_llcc / bw_alg`:
- neon_matvec: **1.007** — near-perfect match
- When bwmon ≈ 0 (LLCC-resident): icc_llcc = L2 miss bandwidth = bw_alg

At WS=16 MB with DRAM traffic (scalar_matvec, bwmon=7,159 MB/s): `icc_llcc / bw_alg = 1.74`
— HW prefetcher causes extra L2→LLCC traffic beyond demand; here `icc_llcc ≈ bwmon / 0.70`.

**Verification of icc_dram ≈ bwmon in DRAM regime:**

At WS=128 MB: `icc_dram / bwmon`:
- chase: 0.952, scalar: 0.884, neon_matvec: 0.899, neon_axpy: 0.753

neon_axpy undercounts most (0.753) because **icc_dram estimates total DRAM (reads+writes)
while bwmon measures reads only**. For AXPY (2 reads + 1 write), bwmon captures 2/3 of
total DRAM traffic → icc_dram / bwmon = 0.9 × (2/3)⁻¹... actually:
`icc_dram/bwmon = icc_dram/bus_access × bus_access/bwmon`. For AXPY: bus_access = 1.56 × bwmon
and icc_dram/bus_access ≈ 0.48. The 52% undercount of total DRAM (DCVS conservatism).

---

## 4. bus_access vs bwmon: Reads vs Total Traffic

`bus_access / bwmon = (demand_reads + demand_writes) / demand_reads`

For workloads with writes, this ratio exceeds 1:

| Workload | bus_access / bwmon | Expected | Explanation |
|---|---:|---:|---|
| chase | 1.040 | 1.0 | read-only; small PMU overhead |
| scalar_matvec | 1.021 | ~1.0 | y tiny (16 KB), effectively read-only |
| neon_matvec | 1.061 | ~1.0 | A is read-only streaming |
| neon_axpy | **1.560** | **3/2 = 1.5** | 2 reads (x, y) + 1 write (y) |

This is not a calibration error. For CPU workloads, `bus_access` captures total traffic
entering the shared subsystem (reads + writes), while `bwmon` captures DRAM reads only.
In the DRAM regime, `bus_access ≈ bwmon × (reads+writes)/reads`.

---

## 5. HW Prefetcher Amplification

At DRAM regime (WS ≥ 128 MB), `bwmon / bw_alg`:

| Workload | bwmon / bw_alg | Explanation |
|---|---:|---|
| chase | 0.64 ± 0.33 | LLCC absorbs ~36% of random hops (LRU temporal reuse) |
| scalar_matvec | 1.90 ± 0.01 | LPDDR5X 128B burst per 64B cache-line miss |
| neon_matvec | 1.88 ± 0.05 | Same |
| neon_axpy | 1.25 ± 0.02 | 1.88 × (2 demand reads) / (3 traffic streams) = 1.25 |

For neon_axpy: bw_alg counts 3 streams (read x + read y + write y). The HW prefetcher
amplifies the 2 demand reads: `bwmon = 1.88 × 2/3 × bw_alg = 1.25 × bw_alg` ✓.

`bus_access / bw_alg` at DRAM regime:
- chase: 0.66, scalar: 1.93, neon_matvec: 1.94, neon_axpy: 1.93

All streaming workloads show `bus_access / bw_alg ≈ 1.93×` regardless of read/write mix —
confirming that the HW prefetcher amplifies both reads and writes equally.

---

## 6. ICC Undercounting Model

**Model:** `icc_dram_agg = α × bwmon + β`

- `α < 1`: DCVS exponential moving average (40 ms cadence) smooths peaks, creating lag
- `β > 0`: background system floor from display/audio/DSP ICC clients

```
Undercounting fraction = (bwmon − icc_dram) / bwmon = (1 − α) − β / bwmon

Maximum undercounting at high BW: approaches (1 − α)
Crossover at bwmon* = β / (1 − α): icc_dram = bwmon exactly
Below bwmon*: icc_dram > bwmon (background floor dominates)
```

Global OLS (WS≥128 MB, all workloads, n=40):
`icc_dram = 0.812 × bwmon + 1,057 MB/s` (R²=0.978)
Crossover: **bwmon* = 5,615 MB/s**

Per-workload at WS=128 MB:

| Workload | bwmon (MB/s) | icc_dram undercount | Notes |
|---|---:|---:|---|
| chase | 10,577 | +4.8% | moderate BW; β floor partially offsets α |
| scalar_matvec | 10,834 | +11.6% | same BW as chase but weaker β offset |
| neon_matvec | 100,666 | +10.1% | high BW; α dominates |
| neon_axpy | 88,811 | +24.7% vs bwmon | icc_dram tracks total; bwmon only reads |

---

## 7. WS Regime Transitions

DRAM onset per workload (from bwmon sweep):

| Workload | L2-resident (bwmon≈0) | LLCC-resident | DRAM-regime |
|---|---|---|---|
| All | WS ≤ 4 MB (≤ L2 = 12 MB) | — | — |
| neon_matvec, neon_axpy | — | — | WS ≥ 16 MB (system LLCC contention) |
| scalar_matvec | — | partial | WS ≥ 16 MB |
| chase | — | WS ≤ 64 MB (boundary zone) | WS ≥ 128 MB |

The effective LLCC capacity for user data is ~50–80 MB under typical system load (not the
full 112 MB nominal capacity, which is shared with display/audio/GPU/DSP).

---

## 8. Experiment Results

### Exp 1 — bwprobe: Pointer-Chase Latency and Bandwidth Sweep

WS={2,…,256} MB. Fisher-Yates random cycle (MLP=1, defeats prefetcher). 7 reps per WS.

| Regime | WS | bwmon | latency |
|---|---|---:|---:|
| L1D/L2 | ≤ 12 MB | ~20 MB/s | 0.7–1.1 ns |
| LLCC | 16–112 MB | 25–772 MB/s | 1.0–2.8 ns |
| DRAM | ≥ 128 MB | 3,665–10,600 MB/s | 4.8–5.9 ns |

`bus_access / bwmon = 1.034 ± 0.008` at WS≥128 MB (read-only: expected ~1.0 + small PMU overhead).

### Exp 2 — matvec: Scalar vs NEON Matrix-Vector Multiply

WS=128 MB (M=4096, N=8192). y=Ax, y always L1D-resident.

| Kernel | bwmon | bw_alg | DRAM amplification |
|---|---:|---:|---:|
| scalar | 6,028 MB/s | 3,100 MB/s | 1.94× |
| NEON (8 acc) | **54,102 MB/s** | **27,898 MB/s** | 1.94× |

NEON 9.5× higher DRAM bandwidth by breaking the single-accumulator FMA dependency chain.

### Exp 3 — prefault_exp: DRAM Amplification Source Isolation

MADV_RANDOM has **no effect** (bwmon/bw_alg stays 1.94×). Amplification is from the
**hardware stream prefetcher** (or LPDDR5X 128B burst per 64B miss; indistinguishable
from userspace). MADV_DONTNEED collapses physical pages to zero-page, confounding the test.

### Exp 4 — llcc_size: LLCC Capacity Measurement

Flush-size sweep (24/128/256 MB) to verify LLCC boundary. All flush sizes give the same
result: **LLCC capacity ≈ 112–120 MB** (latency transition at WS=112→128 MB, 2.7→5.4 ns).
Nominal "12 MB per slice" refers to one slice; SM8750 cpu6 accesses ~9–10 slices total.

### Exp 5 — icc_bw: ICC vs bwmon Correlation (Preliminary)

WS={8,32,64,128,256} MB, 3 workloads (chase/scalar/NEON matvec), 3 reps each.
Uncontrolled: thermal drift of 17°C over experiment, single warmup pass.

Global OLS: `icc_dram = 0.863 × bwmon + 525` (R²=0.993, n=31 DRAM reps).
Pearson r(bwmon, bus_access) = 0.9997; r(bwmon, icc_dram) = 0.9966.

### Exp 6 — bw_bench: Controlled Multi-Workload Benchmark

**Controls:** thermal gate (<42°C), convergence warmup (5% bwmon tolerance), 5 reps,
flush_caches(24 MB), cpu freq locked.

**Workloads:**

| Workload | Bound | bwmon at WS=128 MB | bus_access at WS=128 MB | bw_alg at WS=128 MB |
|---|---|---:|---:|---:|
| chase | latency | 10,577 MB/s | 10,997 MB/s | 11,142 MB/s |
| scalar_matvec | FMA latency | 10,834 MB/s | 11,060 MB/s | 5,730 MB/s |
| neon_matvec | DRAM BW | 100,666 MB/s | 106,777 MB/s | 54,977 MB/s |
| neon_axpy | DRAM BW (r+w) | 88,811 MB/s | 138,520 MB/s | 71,964 MB/s |

Note: `neon_axpy` bus_access >> bwmon because AXPY writes y (2 reads + 1 write → bus_access/bwmon = 3/2).

**bwmon CV at WS=128 MB:** neon_axpy=0.2%, neon_matvec=1.1%, scalar=5.8%, chase=8.7%.

**ICC undercounting at WS=128 MB:**

| Workload | icc_dram / bwmon | icc_dram / bus_access |
|---|---:|---:|
| chase | 0.952 | 0.916 |
| scalar_matvec | 0.884 | 0.866 |
| neon_matvec | 0.899 | 0.848 |
| neon_axpy | 0.753 | 0.483 |

For neon_axpy, icc_dram / bus_access = 0.483: DCVS underestimates total DRAM by 52%
for write-heavy workloads (40 ms lag is longest relative to the 5 s window at high BW).

**LLCC absorption** (icc_llcc − icc_dram) / icc_llcc:
**30.0% ± 0.1%** for all workloads at all WS ≥ 16 MB. Structural constant.

---

## 9. PMU Event Status (Oryon V2, Android 15)

| Event | Status | Notes |
|---|---|---|
| `bus_access` (0x0019) | ✓ Accurate | `exclude_kernel=0` mandatory; kernel handles cache refills |
| `l2d_cache` (0x0016) | ✓ Fires | Counts every L1D miss; cannot distinguish L2/LLCC/DRAM |
| `l2d_cache_refill` (0x0017) | ✗ Broken | Flat ~18K/s |
| `l2d_cache_lmiss_rd` (0x4009) | ✗ Broken | ~1 MB/s constant |
| `l3d_cache_allocate` (0x0029) | ✗ Broken | Always 0 |
| LLCC PMU (type=11) | ✗ No access | ENOENT on Android 15 |

**`exclude_kernel=0` is mandatory**: cache refills are attributed to kernel context on
Oryon V2. `exclude_kernel=1` causes 23–33× undercounting for `bus_access`.

---

## 10. ICC Client Map (SM8750)

| ICC path | Client |
|---|---|
| `chm_apps → qns_llcc` | CPU clusters (Prime + Gold) |
| `llcc_mc → ebi` | All clients' DRAM traffic (aggregate) |
| `3d00000.qcom,kgsl-3d0` | GPU — bypasses ICC (uses GMU hardware DDR counters) |
| `remoteproc-cdsp` | Compute DSP (HexDSP/NPU) |
| `remoteproc-adsp` | Audio DSP |
| `ae00000.qcom,mdss_mdp` | Display engine |
| `aa00000.qcom,vidc` | Video codec |
| `1d84000.ufshc` | UFS storage |

Background floor β ≈ 1,057 MB/s in icc_dram_agg from non-CPU clients above.

---

## 11. Claim Provenance (Data vs Semantics)

This section clarifies where key statements come from.

- LLCC effective size around 112 MB:
  - Source type: experiment result in this repo (Exp4), based on flush-size sweep.
  - Evidence path: `llcc_size.c` method + report tables/summary text.
- "`bus_access` measures at L2 boundary":
  - Source type: PMU event semantics (documentation/code comments) plus data consistency.
  - Data consistency: `bus_access` responds in LLCC-resident regimes where DRAM monitor
    `bwmon` can stay near background, indicating it is upstream of DRAM.
- "`agg_avg` over `avg_bw`":
  - Source type: tracepoint field meaning + implementation choice.
  - Reason: `avg_bw` is per-client partial vote; `agg_avg` is node aggregate used for
    total-demand comparisons.
