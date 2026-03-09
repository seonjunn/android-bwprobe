# Memory Bandwidth Characterization — Snapdragon 8 Elite (Oryon V2)

**Goal:** Find a per-process metric that characterizes the *memory intensity* of a workload —
reads and writes — to estimate the contention it will cause or experience in the **shared
memory subsystem** (LLCC and DRAM), where all processors (CPU clusters, GPU, DSP, display)
compete for the same bandwidth.

Platform: Samsung Galaxy S25+, Snapdragon 8 Elite (SM8750), cpu6 (Prime cluster, 4.47 GHz).

---

## Platform

| Property | Value |
|---|---|
| CPU | Oryon V2 ×8: 2 Prime (cpu6–7) + 6 Gold (cpu0–5) |
| L2 | 12 MB / Prime cluster |
| LLCC | Nominal 12 MB/slice; empirically **~112 MB** (9–10 slices accessible) |
| DRAM | LPDDR5X, 128-bit bus |
| OS / kernel | Android 15, 6.6.30 |
| Pinned core | cpu6; `bwmon-llcc-prime` covers cpu6–7 |
| CPU freq | Locked to 4,473,600 kHz via `/data/local/tmp/powerctl.sh` |

Measurement points in the memory hierarchy:

```
  L2 (12 MB)
    ↓ ← bus_access (PMU 0x0019): CPU-side traffic leaving L2 toward shared fabric (reads + writes)
  LLCC (~112 MB, absorbs ~30% of L2 misses regardless of WS)
    ↓ ← bwmon (bw_hwmon_meas): DRAM reads only, fires at WS > LLCC
  LPDDR5X DRAM
```

---

## Key Findings

**1. Metric regime boundaries.** `bus_access` and `bwmon` are at different hierarchy levels:
- `bus_access`: CPU-side traffic entering shared subsystem (LLCC/DRAM path)
- `bwmon`: traffic that reaches DRAM controller (reads only)

In data, `bus_access` rises strongly before DRAM saturation points where `bwmon` is still low
for LLCC-resident behavior (for example, chase WS=64 MB).

**1a. LLCC transparency is determined by temporal reuse, not working-set size.**
Whether the LLCC absorbs L2 misses (opaque) or passes them through to DRAM
(transparent) depends on whether the same cache line is reused before eviction:
- **Temporal reuse (e.g., looping random access):** LLCC absorbs after the first
  cold miss. bwmon silent even at WS=64 MB. Example DNN: batched FC inference
  where weights (< 112 MB) are reused across the batch.
- **No reuse (e.g., single-pass streaming):** LLCC is a transparent staging
  buffer — every byte still traverses DRAM regardless of WS vs. LLCC capacity.
  bwmon fires at WS > L2 (> 12 MB). Example DNN: batch=1 FC inference
  streaming a large weight matrix on every call.

This is why pointer-chase (random, looping, MLP=1, defeats prefetcher) is the
canonical memory-hierarchy probe: it has temporal reuse so the working set
stabilizes at exactly one cache level, and MLP=1 ensures measured throughput
equals `1 / access_latency` at that level.

**2. `bus_access` captures LLCC+DRAM-side pressure for CPU workloads.** `bwmon` only captures
traffic that reaches DRAM, so it can miss LLCC-resident contention.
`bus_access` also counts reads AND writes; for AXPY (2 reads + 1 write),
`bus_access / bwmon = 3/2 = 1.5` — a principled difference, not error.

**3. LLCC absorption is constant at 30%.** `(icc_llcc − icc_dram) / icc_llcc = 30.0% ± 0.1%`
for all workloads at WS = 16–256 MB. This is temporal reuse within the workload, not just
WS fitting in LLCC: even at WS = 256 MB > LLCC = 112 MB, 30% of L2 misses hit in LLCC.

**4. The correct contention boundary is L2→LLCC, not LLCC→DRAM.**
L2 is private to the Prime cluster; traffic absorbed within L2 causes zero cross-processor
contention. The shared subsystem begins at the L2 output port. `bus_access` and `icc_llcc_agg`
both sit at this boundary. `icc_dram_agg` is at the wrong boundary (LLCC→DRAM): it
structurally misses LLCC-resident pressure regardless of precision or cadence.

**5. bus_access and icc_llcc_agg measure the same physical traffic at the same boundary.**
`icc_llcc_agg ≈ Σ bus_access (all processes, all CPU clusters)`.
`bus_access` is the per-process, workload-isolated version; `icc_llcc_agg` is the all-CPU
system-level version. They differ in scope (per-process vs. cluster) and precision (~4 ms
hardware counter vs. ~40 ms DCVS EMA). For workload characterization, `bus_access` is
preferred (no background contamination, stable across runs). For system-level monitoring,
`icc_llcc_agg` is the available software proxy.

**6. ICC sequential hops: `icc_dram = 0.70 × icc_llcc`.** They are not parallel paths.
Adding them double-counts. Verified: neon_matvec WS=16 MB (LLCC-resident), `icc_llcc / bw_alg = 1.007`.

**7. HW prefetcher amplifies by ~1.9×.** `bus_access / bw_alg ≈ 1.93×` for all streaming
workloads. This is valid for contention characterization: prefetcher fetches compete for LLCC
bandwidth as much as demand fetches. Note when comparing against algorithmic throughput.

**8. icc_dram undercounts DRAM by 10–52%.** DCVS EMA (40 ms). Model:
`icc_dram = 0.81 × bwmon + 1,057 MB/s` (R²=0.978). Retained only for DDR DCVS analysis,
not for contention characterization.

---

## Metric Selection

### CPU contention characterization

| Metric | Precision | Background | Use when |
|---|---|---|---|
| `bus_access` | High (~4 ms, per-process) | Excluded | Characterizing a specific kernel's intrinsic memory intensity |
| `icc_llcc_agg` | Lower (~40 ms DCVS EMA) | Included (all CPU clusters) | System-level CPU memory monitoring; per-process PMU unavailable |

Both are at the correct boundary (L2→LLCC). `icc_dram_agg` is at the wrong
boundary and should not be used for contention characterization.

At DRAM regime: `bus_access = 1.93 × bw_alg` (streaming);
`bus_access / bwmon = 1 + write_streams/read_streams` (e.g. 1.56 for AXPY).

**Robustness across workload regimes.** This selection holds whether workloads
are LLCC-resident or streaming-transparent. In the streaming regime (matvec,
matmul — LLCC transparent), `bwmon` becomes a valid read-only corroboration and
`icc_dram_agg` loses its boundary objection, but neither displaces the two
candidates: `bus_access` still counts writes and is per-process; `icc_llcc_agg`
was already at the correct boundary. In the LLCC-resident regime (looping random
access), `bwmon` and `icc_dram_agg` are blind. The metric ranking is stable
across the full workload space.

### Other metrics

| Goal | Metric |
|---|---|
| DRAM read bandwidth ground truth | `bwmon` (WS > 112 MB only) |
| Algorithmic demand throughput | `bw_alg` |
| System-level DRAM demand / DDR DCVS | `icc_dram_agg` |

### Cross-processor (CPU + GPU + NPU)

No single metric covers all processors. GPU compute bypasses ICC (GMU path) and
is not in `icc_llcc_agg` or `icc_dram_agg`. A complete picture requires
`bus_access` (CPU workload) + kgsl GMU counters (GPU) combined at the DRAM
boundary.

## Source Grounding Notes

- LLCC size (~112 MB): derived from Exp4 flush-size sweep (`llcc_size.c`, 24/128/256 MB flush)
  and documented results in this repo.
- "`bus_access` at L2 boundary": based on PMU event semantics used in code/docs and supported
  by observed regime behavior (`bus_access` reacts before DRAM-only metric `bwmon`).
- `avg_bw` vs `agg_avg` (ICC):
  - `avg_bw` is one DCVS client vote (partial).
  - `agg_avg` is aggregate across all active ICC clients at that node (total node demand).
  - Therefore this project uses `agg_avg` for system-level comparison.

---

## Experiments

**Exp 1 — bwprobe** (`make runprobe`): Pointer-chase sweep WS=2–256 MB.
Latency: L2 ≈ 1 ns, LLCC ≈ 2.7 ns, DRAM ≈ 5.5 ns. DRAM onset at WS=128 MB.
`bus_access / bwmon = 1.034 ± 0.008` (read-only, DRAM regime).

**Exp 2 — matvec** (`make runmatvec`): Scalar vs NEON (8-accumulator) matvec, WS=128 MB.
NEON **54 GB/s** vs scalar **6 GB/s** (9×). Both show 1.94× prefetcher amplification.

**Exp 3 — prefault_exp** (`make runprefault`): Amplification source isolation.
`MADV_RANDOM` has no effect; amplification is from the **HW stream prefetcher**.

**Exp 4 — llcc_size** (`make runllcc`): LLCC capacity via flush-size sweep.
All three flush sizes (24/128/256 MB) agree: **LLCC ≈ 112 MB** (onset at WS=128 MB, 5.4 ns).

**Exp 5 — icc_bw** (`make runicc`): Preliminary ICC correlation (uncontrolled, 17°C drift).
OLS: `icc_dram = 0.863 × bwmon + 525`. r(bwmon, bus_access) = 0.9997.

**Exp 6 — bw_bench** (`make runbench`): Controlled benchmark (thermal gate, convergence warmup).
4 workloads × 5 WS × 5 reps. Key results at WS=128 MB:

| Workload | bwmon | bus_access | bus/bwmon | Bound |
|---|---:|---:|---:|---|
| chase | 10,577 MB/s | 10,997 MB/s | 1.04 | latency |
| scalar_matvec | 10,834 MB/s | 11,060 MB/s | 1.02 | FMA latency |
| neon_matvec | 100,666 MB/s | 106,777 MB/s | 1.06 | DRAM read BW |
| **neon_axpy** | **88,811 MB/s** | **138,520 MB/s** | **1.56** | **DRAM r+w BW** |

LLCC absorption: **30.0% ± 0.1%** constant across all workloads and WS sizes.

Full analysis: [`analysis/bw_bench_analysis.py`](analysis/bw_bench_analysis.py)
Detailed report: [`docs/report.md`](docs/report.md)

---

## Code Structure

Headers are split by responsibility; each `.c` tool includes only what it needs.

```
src/
  cpu.h            CACHE_LINE, ARMV8_PMU_TYPE, perf_event_open(), pin_to_cpu()
  timer.h          now_s(), rdcnt(), cntfreq(), cntdelta_ns()
  cache.h          flush_caches(), prefault()
  chase.h          build_chase(), run_chase(), chase_latency_ns()
  stats.h          stat_mean(), stat_sd()
  buspoll.h        BusPoll struct + buspoll_thread() — 4ms bus_access polling thread
  pmu.h            open_evt_raw(), open_bus_access()
  hwmon.h          Hwmon struct + hwmon_thread() — bwmon + ICC tracepoint reader
  thermal.h        read_skin_c() — skin temperature, zone 53
  matvec_kernels.h matvec_scalar(), matvec_neon(), axpy_neon()

  bwprobe.c        PMU event sweep across WS=2–256 MB
  matvec.c         Scalar vs NEON matvec bandwidth comparison
  prefault_exp.c   DRAM amplification source isolation
  llcc_size.c      LLCC capacity via flush-size sweep
  icc_bw.c         ICC vote vs bwmon correlation (preliminary)
  bw_bench.c       Controlled multi-workload benchmark (thermal gate + convergence warmup)
```

---

## Build and Run

```bash
# NDK at /home/seonjunkim/opt/android-ndk-r29, device at 192.168.0.190:33291
make NDK=/home/seonjunkim/opt/android-ndk-r29 ADB="adb -s 192.168.0.190:33291" <target>
# Targets: runprobe  runmatvec  runprefault  runllcc  runicc  runbench
make clean
```

**Device prerequisites:** root, `perf_event_paranoid=-1`, tracefs at `/sys/kernel/debug/tracing`.

---

## Open Questions

| Question | Next step |
|---|---|
| HW prefetcher disable (confirm 1.9×) | Write `CPUACTLR_EL1` via kernel module |
| Multi-processor DRAM contention | CPU + GPU simultaneous run vs baseline |
| WS=192 MB ZRAM anomaly (bwprobe) | Use `mlock()` to prevent page compaction |
| LLCC PMU access | Try Snapdragon Profiler via proprietary driver |
| ICC lag quantification | Longer rep windows (≥30 s) for cross-correlation |
