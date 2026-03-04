# Memory Contention Characterization — Snapdragon 8 Elite (Oryon V2)

Random pointer-chase (`bwprobe`) and streaming matrix-vector multiply (`matvec`) on
a Samsung Galaxy S25+. Covers PMU calibration, cache-to-DRAM boundary detection,
latency stratification, compute-vs-memory bottlenecks, and DRAM amplification source isolation.

---

## Platform

| Property | Value |
|---|---|
| SoC | Qualcomm Snapdragon 8 Elite (SM8750), Samsung Galaxy S25+ |
| CPU | Oryon V2 ×8: 2 Prime @ 4.47 GHz (cpu6–7) + 6 Gold (cpu0–5) |
| L2 | 12 MB / Prime cluster; 8 MB / Gold cluster |
| LLCC/SLC | Nominal 12 MB/slice; empirically **~112 MB** (see §llcc_size) |
| DRAM | LPDDR5X, 128-bit bus |
| OS / kernel | Android 15, 6.6.30 |
| Pinned core | cpu6; `bwmon-llcc-prime` covers cpu6–7 |
| CPU freq | Locked to 4,473,600 kHz via `/data/local/tmp/powerctl.sh` |

```
  Prime (cpu6–7)        Gold (cpu0–5)
  ┌──────────────┐      ┌──────────────────────┐
  │  L2  12 MB   │      │  L2  8 MB            │
  └──────┬───────┘      └───────────┬──────────┘
  bwmon-prime ↑          bwmon-gold ↑
         └──────────────┬───────────┘
                        ↓
              ┌─────────────────────┐
              │  LLCC  ~112 MB      │   ← empirical (nominal: 12 MB/slice)
              └─────────┬───────────┘
                        ↓
              ┌─────────────────────┐
              │   LPDDR5X DRAM      │
              └─────────────────────┘
```

---

## Build and Run

```bash
make NDK=/home/seonjunkim/opt/android-ndk-r29         # build all binaries
make NDK=... ADB="adb -P 5307" runprobe               # → /tmp/bwprobe.csv
make NDK=... ADB="adb -P 5307" runmatvec              # → /tmp/matvec.csv
make NDK=... ADB="adb -P 5307" runllcc                # → /tmp/llcc_size.csv
make NDK=... ADB="adb -P 5307" runprefault            # → /tmp/prefault_exp.csv
```

Device prerequisites: root, `perf_event_paranoid=-1`, tracefs at `/sys/kernel/debug/tracing`.

**Pre-flight checklist:**
```bash
adb -P 5307 shell cat /sys/devices/system/cpu/cpu6/cpufreq/scaling_cur_freq  # expect 4473600
adb -P 5307 shell cat /proc/sys/kernel/perf_event_paranoid                   # expect -1
adb -P 5307 shell ls /sys/kernel/debug/tracing/events/dcvs/bw_hwmon_meas/   # expect enable file
adb -P 5307 shell cat /sys/class/thermal/thermal_zone53/temp                 # < 40000 ideal
```

---

## Experiment 1 — bwprobe: Pointer-Chase Sweep

**Design.** Fisher-Yates Hamiltonian cycle (MLP=1, defeats hardware prefetcher). Per rep:
`flush_caches(24 MB)` → warmup 0.5 s → `chase_latency_ns` (1M hops, CNTVCT_EL0) → PMU + 5 s BW + bwmon snapshot. 7 reps × 21 WS points, 2–256 MB.

### Results

| WS (MB) | bwmon-prime (MB/s) | chase (GB/s) | lat (ns) | bus_ratio | Regime |
|---:|---:|---:|---:|---:|---|
| 2–12 | 20–228 | 22.0–23.0 | 0.7–1.1 | — | cache |
| 16–32 | 25–772 | 21.5–21.7 | 0.75–1.24 | — | cache |
| 48–112 | 25–228 | 19.7–21.5 | 1.96–2.79 | — | cache |
| **128** | **9,343 ± 1,961** | **10.6 ± 0.5** | **5.93 ± 0.31** | **1.034** | **DRAM** |
| **160** | **4,722 ± 971** | **12.3 ± 0.5** | **4.79 ± 0.27** | **1.039** | **DRAM** |
| 192 | 34 ± 26 | 21.3 | 2.89 | — | anomaly |
| **256** | **3,665 ± 528** | **12.2 ± 0.5** | **4.82 ± 0.26** | **1.033** | **DRAM** |

`bus_ratio` = `bus_access × 64 / elapsed / bwmon_prime`; shown only when bwmon > 1,000 MB/s.

### Findings

1. **DRAM onset at WS = 128 MB.** With flush+warmup, WS ≤ 112 MB is cache-resident
   (`chase_GBs ≈ 21`, `lat ≈ 1–3 ns`). At WS = 128 MB: chase halves (10.6), lat doubles
   (5.93 ns), bwmon jumps 60×. Two independent metrics confirm the boundary.

2. **Three-tier CNTVCT latency.**

   | WS range | lat (ns) | Level |
   |---|---|---|
   | 2–12 MB | 0.7–1.1 | L1D / L2 |
   | 32–112 MB | 1.2–2.8 | LLCC (rises with WS) |
   | 128–256 MB | 4.8–5.9 | DRAM |

3. **`bus_access` (0x0019): 1.034 ± 0.008×** vs bwmon over 21 DRAM-regime rep-points.
   In cache regime, `bus_access ≈ background` (~10–228 MB/s) — correct: no per-process DRAM.
   Formula: `BW_MB_s = count × 64 / elapsed_s / 1e6` with `exclude_kernel = 0`.

4. **Gold cluster background: 842 ± 233 MB/s** — constant across all WS, independent of
   Prime workload. This is Android idle traffic; not cross-cluster interference.

5. **WS = 192 MB anomaly.** Appears cache-resident (bwmon 34 MB/s, chase 21.3 GB/s)
   despite exceeding the 128 MB threshold. Android memory compaction (ZRAM/LMKD) likely
   compresses buffer pages between reps. Treat as unreliable.

---

## Experiment 2 — matvec: Scalar vs Explicit-NEON

**Design.** `y = Ax`, float32, M=4096 (y=16 KB L1D-resident), N=WS/M, x ≤ 64 KB (L1D-resident). A is the sole memory pressure. Per rep: `flush_caches(24 MB)` → scalar warmup + 3 s BW → NEON warmup + 3 s BW. 7 reps.

- **`matvec_scalar`**: auto-vectorized by clang → 4-wide NEON FMLA, 1 accumulator (loop-carried dep on `s`).
- **`matvec_neon`**: explicit 8×`float32x4_t`, 32-wide inner unroll — breaks FMA dep chain.

### Results

| WS (MB) | N | scalar (GB/s) | NEON (GB/s) | NEON/scalar | bwmon-scalar | bwmon-NEON |
|---:|---:|---:|---:|---:|---:|---:|
| 2 | 128 | 12.03 | 69.3 | 5.8× | 0.14 GB/s | 0.02 GB/s |
| 8 | 512 | 7.87 | 67.0 | 8.5× | 0.41 GB/s | 0.43 GB/s |
| 16 | 1024 | 6.75 | 58.5 | 8.7× | 6.95 GB/s | 49.9 GB/s |
| 32 | 2048 | 6.03 | 55.5 | 9.2× | 9.98 GB/s | 80.7 GB/s |
| **128** | **8192** | **5.64** | **53.8** | **9.5×** | **10.6 GB/s** | **98.3 GB/s** |
| 256 | 16384 | 5.56 | 40.8 | 7.3× | 10.6 GB/s | 79.9 GB/s |

### Findings

1. **Scalar is compute-bound at ~5.6 GB/s (large WS).** Clang emits 4-wide NEON FMLA
   with 1 accumulator. FMA latency ≈ 11 cycles: `16B × 4.47 GHz / 11 = 6.5 GB/s` ≈ 5.6 GB/s ✓.

2. **Scalar BW decreases with WS (row-level ILP loss).** At small N (e.g. N=128), the
   OOO engine spans row boundaries → 2–3 rows execute in parallel → ~12 GB/s. At large N
   (N=8192), one row fills the entire OOO window → single accumulator → FMA floor.

3. **NEON with 8 accumulators is DRAM-bandwidth-bound at ~54 GB/s (WS=128 MB).**
   bwmon reaches 98.3 GB/s — **~1.83–1.94× the algorithmic A-read rate**. This amplification
   comes from the hardware stream prefetcher (see §prefault_exp).

4. **NEON becomes DRAM-active at WS ≥ 16 MB** (before scalar at ~32 MB). High-MLP
   sequential streams bypass LLCC fill buffers regardless of WS vs cache size.

5. **WS=256 MB: NEON drops 25%** (40.8 vs 53.8 GB/s) due to increased DRAM row-buffer
   pressure at 2× the page count. Thermal ruled out (same skin temperature).

---

## PMU Event Status on Oryon V2

| Event | Config | Status |
|---|---|---|
| `bus_access` | 0x0019 | **Accurate** — 1.034 ± 0.008× vs bwmon; `exclude_kernel=0` mandatory |
| `l2d_cache` | 0x0016 | **Functional** — fires on every L1D miss; cannot distinguish L2/LLCC/DRAM |
| `l1d_cache_refill` | 0x0003 | Functional (not deeply characterized) |
| `l2d_cache_refill` | 0x0017 | **Broken** — flat ~18K/s regardless of WS |
| `l2d_cache_lmiss_rd` | 0x4009 | **Broken** — ~1 MB/s constant |
| `l3d_cache_allocate` | 0x0029 | **Broken** — always 0 |
| `HW_CACHE_MISSES` | generic | **Broken** — returns 0 |
| LLCC PMU | type=11 | **Inaccessible** — ENOENT on Android 15 |

`exclude_kernel = 0` is mandatory on Oryon V2 — cache refills are attributed to kernel context,
causing 23–33× undercounting with `exclude_kernel = 1`.

---

## Summary

| Metric | Value | Condition |
|---|---|---|
| `bus_access` accuracy | **1.034 ± 0.008×** | WS ≥ 128 MB, bwmon > 1,000 MB/s; 21 rep-points |
| DRAM onset | **128 MB** | pointer-chase; all flush sizes (24/128/256 MB) give same boundary |
| Effective LLCC/SLC size | **~112 MB** | empirical; nominally "12 MB/slice" — multiple slices |
| CNTVCT latency — DRAM | **~5.5 ns/hop** | WS ≥ 128 MB (vs ~1 ns cache, ~2.7 ns LLCC) |
| bwmon (pointer-chase, 128 MB) | **9,343 MB/s** | 7-rep mean |
| Gold cluster background | **842 ± 233 MB/s** | all WS, all reps |
| Scalar matvec BW | **5.6 GB/s** A-reads | compute-bound (FMA latency) |
| NEON matvec BW | **53.8 GB/s** A-reads | DRAM-bandwidth-bound (WS=128 MB) |
| NEON speedup vs scalar | **9.5×** (WS=128 MB) | 8 accumulators break FMA dep chain |
| bwmon amplification | **~1.88–1.94×** | HW stream prefetcher (MADV_RANDOM has no effect) |

---

## Metric Selection

```c
/* Per-process DRAM BW (WS ≥ 128 MB, freq locked) */
struct perf_event_attr a = {
    .type = 10,        /* armv8_pmuv3 */
    .config = 0x0019,  /* bus_access */
    .exclude_kernel = 0,  /* MANDATORY on Oryon V2 */
};
double bw_MB_s = (double)count * 64.0 / elapsed_s / 1e6;
/* Accuracy: 1.034 ± 0.008× vs bwmon at WS ≥ 128 MB */
```

```
Decision:
  Per-process DRAM, WS ≥ 128 MB, freq locked  → bus_access × 64 / elapsed
  Per-process DRAM, smaller WS or variable freq → bw_hwmon_meas (cluster-level only)
  Cluster DRAM ground truth                     → bwmon-llcc-prime / bwmon-llcc-gold
  L1D miss pressure                             → l2d_cache × 64 / elapsed
  Apparent DRAM per algorithmic byte            → bwmon / 1.9 (HW prefetcher 1.9× amplification)
```

---

## Experiment 3 — prefault_exp: DRAM Amplification Source

`bwmon / bw_matvec ≈ 1.94×` at WS=128 MB despite A being read-only during measurement.
Three variants at fixed WS=128 MB test which mechanism causes the amplification.

| Variant | bwmon | bw_alg | amp | Interpretation |
|---|---:|---:|---:|---|
| baseline | 11,676 MB/s | 6,005 MB/s | **1.94×** | Reference |
| `MADV_DONTNEED` on A | 43 MB/s | 5,996 MB/s | 0.007× | Collapses physical pages → zero-page → L1D; confounded |
| `MADV_RANDOM` on A | 11,547 MB/s | 6,005 MB/s | **1.92×** | No change — MADV_RANDOM is OS readahead only |

**Conclusion.** Amplification is from the **Oryon V2 hardware stream prefetcher**: for
sequential access with A >> LLCC, prefetched lines are evicted before demand arrives →
each cache line fetched twice from DRAM. `MADV_RANDOM` does not disable the CPU's
architectural hardware prefetcher. Alternative: LPDDR5X 128B burst per 64B cache-line miss
(indistinguishable from userspace). Disabling requires `CPUACTLR_EL1` (EL1 privilege only).

---

## Experiment 4 — llcc_size: Cache Capacity Measurement

Tests whether the 112 MB cache boundary is real or an artifact of incomplete flush.
Runs pointer-chase across WS=12–256 MB with three flush-buffer sizes. 5 reps each.

| WS (MB) | flush=24 lat (ns) | flush=128 lat (ns) | flush=256 lat (ns) | Regime |
|---:|---:|---:|---:|---|
| 12 | 0.72 | 0.72 | 0.72 | cache (L1D/L2) |
| 32 | 1.68 | 1.23 | 1.24 | cache (LLCC) |
| 64 | 4.92† | 2.31 | 2.30 | cache (LLCC) |
| 96 | 2.65 | 2.63 | 2.62 | cache (LLCC) |
| 112 | 2.72 | 2.73 | 2.74 | cache (near boundary) |
| **128** | **5.35** | **5.37** | **5.50** | **DRAM** |
| 160 | 4.65 | 4.79 | 4.94 | DRAM |

†flush=24 artifact: 24 MB scrub buffer interferes with LLCC loading during warmup transition.

**Key result: all three flush sizes give the same DRAM boundary (WS=112→128 MB).**

- flush=128 MB ≡ flush=256 MB → 128 MB flush completely evicts the LLCC → LLCC ≤ 128 MB.
- WS=112 MB fits in cache, WS=128 MB does not → LLCC capacity ≈ **112–120 MB**.
- The nominal "12 MB LLCC" spec refers to one slice; Snapdragon 8 Elite cpu6 accesses
  multiple LLCC slices totaling ~112 MB.
- flush=24 MB artifacts for small WS (chase_GBs falsely low due to CPU DVFS not ramping;
  bwmon=-1 for all these points confirms they are cache-resident with no DRAM traffic).

---

## Experiment 5 — icc_set_bw: Kernel Interconnect View

`icc_set_bw` tracepoint fires when DCVS updates hardware bandwidth votes (~40 ms cadence).
Captured during matvec at WS=128 MB:

| ICC Path | avg_bw | Meaning |
|---|---:|---|
| `chm_apps → qns_llcc` | ~81 GB/s | CPU cluster → LLCC fabric BW vote |
| `llcc_mc → ebi` | ~57 GB/s | LLCC → DRAM controller BW vote |

DCVS reads bwmon every ~4 ms, votes ICC every ~40 ms. `icc_set_bw` reports aggregate
system-wide votes — not per-process. Use `bus_access` for per-process attribution.

**GPU is excluded**: KGSL bypasses the Linux interconnect framework entirely (uses GMU
hardware path). GPU DRAM traffic never appears in any `icc_set_bw` event. Use
`kgsl_buslevel.avg_bw` (set by GMU hardware DDR counters) for GPU DRAM demand.

```bash
adb shell 'echo "" > /sys/kernel/debug/tracing/trace; \
  echo 1 > /sys/kernel/debug/tracing/events/interconnect/icc_set_bw/enable; \
  echo 1 > /sys/kernel/debug/tracing/tracing_on; sleep 5; \
  echo 0 > /sys/kernel/debug/tracing/tracing_on; \
  cat /sys/kernel/debug/tracing/trace | grep icc_set_bw'
```

Note: `trace_pipe` cannot be used simultaneously with the hwmon background thread (use
static `trace` buffer instead).

---

## Open Questions

| Question | Next step |
|---|---|
| HW prefetcher disable (confirm 1.94×) | Write `CPUACTLR_EL1` via kernel module |
| Clean-page amplification | File-backed mmap (not DONTNEED) for true clean-init test |
| Multi-processor LLCC contention | CPU + GPU simultaneous run vs baseline |
| WS=192 MB ZRAM anomaly | Use `mlock()` to prevent page compaction |
| LLCC PMU | Try Snapdragon Profiler via proprietary driver |
