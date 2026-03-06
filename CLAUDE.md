# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

C measurement tools for characterizing CPU DRAM/cache bandwidth on a Samsung Galaxy S25+
(Snapdragon 8 Elite, Oryon V2). Cross-compiles for Android AArch64 via the Android NDK.

**Goal:** Find a per-process metric for memory intensity (reads + writes) to estimate the
contention a workload will cause or experience in the **shared memory subsystem** — LLCC
and DRAM — where all processors (CPU clusters, GPU, DSP, display) compete. The metric must
be sensitive to LLCC-resident workloads, not just DRAM-spilling ones.

## Repository Structure

```
src/
  cpu.h            CACHE_LINE, ARMV8_PMU_TYPE, perf_event_open(), pin_to_cpu()
  timer.h          now_s() [CLOCK_MONOTONIC], rdcnt()/cntfreq()/cntdelta_ns() [CNTVCT_EL0]
  cache.h          flush_caches(n), prefault(buf, sz)
  chase.h          build_chase(sz), run_chase(buf, secs), chase_latency_ns(buf, nhops)
  stats.h          stat_mean(), stat_sd()
  buspoll.h        BusPoll struct + buspoll_thread() — 4ms bus_access polling; cpu_pin field
  pmu.h            open_evt_raw(type, config), open_bus_access()
  hwmon.h          Hwmon struct + hwmon_thread() — reads bwmon + ICC from trace_pipe
  thermal.h        read_skin_c() — skin temperature zone 53 (sys-therm-0)
  matvec_kernels.h matvec_scalar(), matvec_neon() [8-acc], axpy_neon() [y += α·x]

  bwprobe.c        PMU event sweep: WS=2–256 MB, 7 reps; HWMON_PRIME | HWMON_GOLD
  matvec.c         Scalar vs NEON matvec; HWMON_PRIME
  prefault_exp.c   DRAM amplification isolation (baseline/MADV_DONTNEED/MADV_RANDOM)
  llcc_size.c      LLCC capacity: flush-size sweep (24/128/256 MB); HWMON_PRIME
  icc_bw.c         ICC vote vs bwmon correlation, preliminary; HWMON_PRIME | HWMON_ICC
  bw_bench.c       Controlled benchmark: thermal gate + convergence warmup; HWMON_PRIME | HWMON_ICC

analysis/
  bw_bench_analysis.py  Main analysis script (thermal, ICC model, LLCC absorption, WS sweep)
  icc_analysis.py       ICC preliminary analysis
  split_traces.py       Split timeseries traces by workload/rep

data/                   CSV output from make run* (gitignored)
docs/
  report.md        Full experimental report with all tables
```

## Build and Run

```bash
# NDK: /home/seonjunkim/opt/android-ndk-r29
# Device: adb -s 192.168.0.190:33291

make NDK=/home/seonjunkim/opt/android-ndk-r29 ADB="adb -s 192.168.0.190:33291" <target>

# Targets:
#   all           build all binaries → build/
#   runprobe      → /tmp/bwprobe.csv
#   runmatvec     → /tmp/matvec.csv
#   runprefault   → /tmp/prefault_exp.csv
#   runllcc       → /tmp/llcc_size.csv
#   runicc        → data/icc_bw_*.csv
#   runbench      → data/bw_bench_*.csv
#   pulldata      pull all CSVs to data/
#   clean         rm -rf build/

make NDK=/home/seonjunkim/opt/android-ndk-r29 clean all   # clean rebuild
python3 analysis/bw_bench_analysis.py                      # analyze bw_bench data
```

**Device prerequisites:** root, `perf_event_paranoid=-1`, tracefs at `/sys/kernel/debug/tracing`.

## Build Notes

- `-D_GNU_SOURCE` is in `CFLAGS` in the Makefile. Do **not** add it to any header — it must
  be defined before system headers, and the Makefile flag guarantees that.
- All binaries are statically linked (`-static`), API level 35, AArch64.

## Critical Implementation Details

**`exclude_kernel=0` is mandatory on Oryon V2.** Cache refills are attributed to kernel
context; `exclude_kernel=1` causes 23–33× undercounting for `bus_access`. This is enforced
in `open_evt_raw()` in `pmu.h`.

**CPU pinning:** All tools pin to cpu6 (Prime cluster) via `pin_to_cpu(6)` from `cpu.h`.
The `buspoll_thread` also pins; set `BusPoll.cpu_pin = CPU_PIN` at initialization.

**PMU event status (Oryon V2, Android 15):**
- `bus_access` (0x0019) — **accurate**; `exclude_kernel=0` mandatory
- `l2d_cache` (0x0016) — fires on every L1D miss; cannot distinguish L2/LLCC/DRAM
- `l2d_cache_refill` (0x0017) — **broken** (flat ~18K/s)
- `l2d_cache_lmiss_rd` (0x4009) — **broken** (~1 MB/s constant)
- `l3d_cache_allocate` (0x0029) — **broken** (always 0)
- LLCC PMU (type=11) — **inaccessible** on Android 15 (ENOENT)

**bw_hwmon_meas tracepoint** (`dcvs/bw_hwmon_meas`) is the hardware DRAM ground truth,
~4 ms cadence. Cluster-level read-only bandwidth. Only fires when WS > LLCC (~112 MB).
`hwmon_thread` in `hwmon.h` reads `trace_pipe` and accumulates samples with a mutex.

**ICC tracepoint** (`interconnect/icc_set_bw`) is DCVS software bandwidth vote, ~40 ms
cadence. Filter by both path AND destination node to avoid double-counting src+dst events:
- `path=chm_apps-qns_llcc` + `node=qns_llcc` → `icc_llcc_agg` (total L2 miss BW)
- `path=llcc_mc-ebi` + `node=ebi` → `icc_dram_agg` (LLCC miss BW to DRAM)
Use `agg_avg` field (aggregate across all clients), not `avg_bw` (single client's vote).

**CPU frequency:** Locked to 4,473,600 kHz via `/data/local/tmp/powerctl.sh`.

## Memory Hierarchy and Metric Regimes

```
CPU core (cpu6, Prime cluster)
  L1D: 96 KB private
  L2:  12 MB shared (Prime cluster)
    ← bus_access fires here: all L2 misses, reads + writes, WS > 12 MB
  LLCC: ~112 MB shared (all clusters + display/audio/GPU/DSP)
    ← bwmon fires here: DRAM reads only, WS > LLCC
  LPDDR5X DRAM (128-bit bus)
```

| WS range | bus_access | bwmon |
|---|---|---|
| ≤ 12 MB (L2-resident) | ≈ 0 | ≈ 0 |
| 12–112 MB (LLCC-resident) | fires | ≈ 0 |
| > 112 MB (DRAM regime) | fires | fires |

## Key Empirical Findings

**Metric selection for shared-subsystem contention:** Use `bus_access`. It fires at WS > 12 MB
— i.e., whenever traffic enters the shared subsystem (LLCC or DRAM) — counts reads + writes,
and equals 1.93× bw_alg for all streaming workloads. `bwmon` is insufficient: it only fires
at WS > LLCC, missing LLCC-resident workloads that still compete for shared bandwidth.

**LLCC absorption = 30.0% ± 0.1%** constant across all workloads and all WS ≥ 16 MB.
`icc_dram = 0.70 × icc_llcc`. The two ICC metrics are sequential hops, not parallel paths —
adding them double-counts.

**HW prefetcher amplification ≈ 1.9×:** `bwmon / bw_alg ≈ 1.9×` for streaming workloads
(LPDDR5X 128B burst per 64B miss). For AXPY: `1.9 × 2/3 = 1.25×` (prefetcher amplifies
the 2 demand reads; bw_alg counts 3 streams). `MADV_RANDOM` has no effect.

**bus_access counts reads + writes; bwmon counts reads only.** For AXPY (2 reads + 1 write):
`bus_access / bwmon = 3/2 = 1.5` — not a calibration error.

**ICC undercounting model:** `icc_dram = 0.812 × bwmon + 1,057 MB/s` (R²=0.978).
Background floor β=1,057 MB/s from display/audio/DSP clients. Crossover at bwmon=5,615 MB/s.
Worst undercount: neon_axpy at WS=128 MB: icc_dram/bus_access = 0.483 (52% undercount).

**bw_bench controlled results at WS=128 MB:**

| Workload | bwmon (MB/s) | bus_access (MB/s) | bus/bwmon | Bound |
|---|---:|---:|---:|---|
| chase | 10,577 | 10,997 | 1.04 | latency |
| scalar_matvec | 10,834 | 11,060 | 1.02 | FMA latency |
| neon_matvec | 100,666 | 106,777 | 1.06 | DRAM read BW |
| neon_axpy | 88,811 | 138,520 | 1.56 | DRAM r+w BW |

**Latency (bwprobe):** L2 ≈ 1 ns, LLCC ≈ 2.7 ns, DRAM ≈ 5.5 ns.
**DRAM onset:** WS = 128 MB (with 24 MB flush + warmup; WS ≤ 112 MB stays cache-resident).
**bus_access accuracy:** 1.034 ± 0.008× vs bwmon at WS ≥ 128 MB (read-only workloads).

## bw_bench Design (Exp 6)

- Workloads: chase, scalar_matvec, neon_matvec, neon_axpy (y += α·x)
- WS: {4, 16, 64, 128, 256} MB; 5 reps per (workload, WS)
- Thermal gate: wait for skin_c < 42°C before each rep (timeout 120 s)
- Convergence warmup: repeat 1s windows until bwmon changes < 5%; min passes by WS regime
- Output summary: `data/bw_bench_summary.csv`
  - Columns: `workload,ws_mb,rep,bwmon_MBs,icc_llcc_agg_MBs,icc_dram_agg_MBs,bw_alg_MBs,bus_access_MBs,warmup_passes,skin_c`
- Timeseries: `data/bw_bench_trace_{bwmon,icc,bus}.csv`, `data/bw_bench_context.csv`
- Analysis: `python3 analysis/bw_bench_analysis.py`

## AXPY bw_alg Accounting

neon_axpy has 3 traffic streams per element: read x, read y, write y.
`bw_alg = passes × 3 × arr_bytes / elapsed` — all 3 streams counted.
`bus_access / bwmon = 3/2 = 1.5` because bwmon counts reads only (2 streams),
bus_access counts all 3. Prefetcher amplification: `bwmon = 1.88 × (2/3) × bw_alg = 1.25 × bw_alg`.
