# Memory Bandwidth Characterization — Snapdragon 8 Elite (Oryon V2)

**Goal:** Find a per-process metric that characterizes the *memory intensity* of a workload —
reads and writes — to estimate the DRAM bandwidth contention it will cause or experience.

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
    ↓ ← bus_access (PMU 0x0019): all L2 misses, reads + writes, fires at WS > 12 MB
  LLCC (~112 MB, absorbs ~30% of L2 misses regardless of WS)
    ↓ ← bwmon (bw_hwmon_meas): DRAM reads only, fires at WS > LLCC
  LPDDR5X DRAM
```

---

## Key Findings

**1. Metric regime boundaries.** `bus_access` measures at the L2 boundary (all L2 misses),
`bwmon` at the DRAM controller. The LLCC sits between them and absorbs ~30% of L2 misses:
- WS ≤ 12 MB (L2-resident): both metrics near zero
- 12 MB < WS < LLCC: `bus_access` fires; `bwmon` ≈ 0
- WS > LLCC: both fire; `bus_access / bwmon = (reads + writes) / reads`

**2. `bus_access` captures reads AND writes; `bwmon` captures reads only.** For AXPY
(2 reads + 1 write), `bus_access / bwmon = 3/2 = 1.5` — a principled difference, not error.
For DRAM contention estimation, `bus_access` is the correct metric.

**3. LLCC absorption is constant at 30%.** `(icc_llcc − icc_dram) / icc_llcc = 30.0% ± 0.1%`
for all workloads at WS = 16–256 MB. This is temporal reuse within the workload, not just
WS fitting in LLCC: even at WS = 256 MB > LLCC = 112 MB, 30% of L2 misses hit in LLCC.

**4. ICC metrics are at different hierarchy levels.** `icc_llcc_agg` = total L2 miss bandwidth
(all traffic entering LLCC). `icc_dram_agg` = LLCC miss bandwidth (going to DRAM). They are
sequential hops: `icc_dram = 0.70 × icc_llcc`. Adding them double-counts. Verified:
at WS=16 MB (LLCC-resident) for neon_matvec, `icc_llcc / bw_alg = 1.007`.

**5. HW prefetcher amplifies by ~1.9×.** `bwmon / bw_alg ≈ 1.9×` for all streaming workloads
(LPDDR5X 128B burst per 64B miss). For AXPY: `bwmon / bw_alg = 1.9 × 2/3 = 1.25` (bw_alg
counts 3 streams; prefetcher amplifies the 2 demand reads). `bus_access / bw_alg ≈ 1.93×`
for all streaming workloads (reads and writes amplified equally by the prefetcher).

**6. ICC undercounts DRAM by 10–52%.** DCVS votes every 40 ms via EMA. Model:
`icc_dram = 0.81 × bwmon + 1,057 MB/s` (R²=0.978). Background floor (β=1,057 MB/s from
display/audio/DSP) causes ICC to *overcount* when `bwmon < 5,615 MB/s`. Undercount is worst
for high-bandwidth write workloads (neon_axpy: 52% undercount vs total DRAM traffic).

---

## Metric Selection

| Goal | Metric | Condition |
|---|---|---|
| **Contention estimation (universal)** | `bus_access` | WS > 12 MB (L2 boundary); reads + writes |
| DRAM read bandwidth (ground truth) | `bwmon` | WS > 112 MB only |
| Demand throughput (any WS) | `bw_alg` | Always; measures algorithm rate, not DRAM |
| Real-time DRAM monitoring | `icc_dram_agg` | WS > LLCC; ~10% undercount (read-only) |
| L2 + LLCC pressure | `icc_llcc_agg` | WS > 12 MB; ≈ `bus_access` |

At DRAM regime: `bus_access = 1.93 × bw_alg` (streaming) and
`bus_access = bwmon × (1 + write_streams / read_streams)`.

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
