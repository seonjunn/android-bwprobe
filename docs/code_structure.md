# Code Structure and Build Guide

This file describes the source layout, what each file does, how to build, and critical
implementation constraints for the android-bwprobe measurement tools.

---

## Repository Layout

```
src/           C source — headers (shared library) + tool programs
analysis/      Python analysis scripts
data/          CSV output from make run* (gitignored)
docs/          Reference documentation
Makefile       Build + ADB deployment
```

---

## Source Headers (`src/*.h`)

These headers are the shared library. Each tool includes only what it needs.

| Header | What it provides |
|---|---|
| `cpu.h` | `CACHE_LINE`, `ARMV8_PMU_TYPE`, `perf_event_open()`, `pin_to_cpu()` |
| `timer.h` | `now_s()` (CLOCK_MONOTONIC), `rdcnt()` / `cntfreq()` / `cntdelta_ns()` (CNTVCT_EL0) |
| `cache.h` | `flush_caches(n)`, `prefault(buf, sz)` |
| `chase.h` | `build_chase()`, `run_chase()`, `chase_latency_ns()` — pointer-chase probe |
| `stats.h` | `stat_mean()`, `stat_sd()` |
| `pmu.h` | `open_evt_raw(type, config)`, `open_bus_access()` — perf_event wrappers |
| `buspoll.h` | `BusPoll` struct + `buspoll_thread()` — 4 ms `bus_access` polling thread |
| `hwmon.h` | `Hwmon` struct + `hwmon_thread()` — reads `bwmon` + ICC from `trace_pipe` |
| `thermal.h` | `read_skin_c()` — skin temperature, zone 53 (sys-therm-0) |
| `matvec_kernels.h` | `matvec_scalar()`, `matvec_neon()` (8-accumulator), `axpy_neon()` (y += α·x) |

---

## Tool Programs (`src/*.c`)

Each `.c` file is a standalone binary. They map to the numbered experiments:

| File | Experiment | What it does | `make` target |
|---|---|---|---|
| `bwprobe.c` | Exp 1 | Pointer-chase PMU event sweep across WS=2–256 MB. Measures latency and `bus_access` at each working-set size. | `runprobe` |
| `matvec.c` | Exp 2 | Scalar vs. NEON (8-accumulator) matvec at WS=128 MB. Demonstrates 9× throughput difference and 1.9× prefetcher amplification. | `runmatvec` |
| `prefault_exp.c` | Exp 3 | Isolates the source of 1.9× DRAM amplification. Tests three conditions: baseline, `MADV_DONTNEED`, `MADV_RANDOM`. Conclusion: source is the HW stream prefetcher. | `runprefault` |
| `llcc_size.c` | Exp 4 | Determines effective LLCC capacity via flush-size sweep (24 / 128 / 256 MB). All three converge: LLCC ≈ 112 MB (onset at WS=128 MB, latency 5.4 ns). | `runllcc` |
| `icc_bw.c` | Exp 5 | Preliminary ICC correlation: sweeps WS={8,32,64,128,256} MB and records `bwmon`, `bus_access`, and ICC tracepoints side by side. Uncontrolled (17°C thermal drift). | `runicc` |
| `bw_bench.c` | Exp 6 | Controlled multi-workload benchmark with thermal gate (<42°C) and convergence warmup (bwmon changes <5%). 4 workloads × 5 WS × 5 reps. Primary data source for all key findings. | `runbench` |

### bw_bench workloads (Exp 6)

| Workload | Pattern | What it tests |
|---|---|---|
| `chase` | Pointer-chase, random, MLP=1 | Latency-bound; LLCC-resident until WS > 112 MB |
| `scalar_matvec` | Sequential read, scalar FMA | FMA-latency-bound; streaming |
| `neon_matvec` | Sequential read, NEON 8-acc | DRAM read bandwidth bound; streaming |
| `neon_axpy` | y += α·x — 2 reads + 1 write | DRAM read+write bound; exercises write path |

---

## Analysis Scripts (`analysis/`)

| Script | Purpose |
|---|---|
| `bw_bench_analysis.py` | Main analysis for Exp 6: thermal filtering, ICC undercounting model, LLCC absorption constant, WS sweep plots |
| `icc_analysis.py` | Preliminary analysis for Exp 5: OLS fits, Pearson r, metric ratio statistics |
| `split_traces.py` | Splits timeseries trace CSVs by (workload, rep) for per-segment inspection |

Run analysis:
```bash
python3 analysis/bw_bench_analysis.py
```

---

## Build and Run

### Prerequisites

| Requirement | Value |
|---|---|
| NDK | `/home/seonjunkim/opt/android-ndk-r29` (AArch64 cross-compiler) |
| Device | `adb -s 192.168.0.190:33291` (Samsung Galaxy S25+) |
| Device root | Required (perf_event, tracefs, powerctl) |
| `perf_event_paranoid` | Must be `-1` on device |
| tracefs | Must be mounted at `/sys/kernel/debug/tracing` |
| CPU freq | Locked to 4,473,600 kHz via `/data/local/tmp/powerctl.sh` |

### Build commands

```bash
# Build all binaries → build/
make NDK=/home/seonjunkim/opt/android-ndk-r29 ADB="adb -s 192.168.0.190:33291" all

# Clean rebuild
make NDK=/home/seonjunkim/opt/android-ndk-r29 clean all

# Build and run a specific experiment
make NDK=/home/seonjunkim/opt/android-ndk-r29 ADB="adb -s 192.168.0.190:33291" runbench

# Pull all CSVs to data/
make NDK=/home/seonjunkim/opt/android-ndk-r29 ADB="adb -s 192.168.0.190:33291" pulldata
```

### Make targets

| Target | Output | Description |
|---|---|---|
| `all` | `build/` | Compile all binaries |
| `runprobe` | `/tmp/bwprobe.csv` | Exp 1: pointer-chase PMU sweep |
| `runmatvec` | `/tmp/matvec.csv` | Exp 2: scalar vs. NEON matvec |
| `runprefault` | `/tmp/prefault_exp.csv` | Exp 3: amplification source isolation |
| `runllcc` | `/tmp/llcc_size.csv` | Exp 4: LLCC capacity sweep |
| `runicc` | `data/icc_bw_*.csv` | Exp 5: ICC vs. bwmon correlation |
| `runbench` | `data/bw_bench_*.csv` | Exp 6: controlled benchmark |
| `pulldata` | `data/` | Pull all CSVs from device |
| `clean` | — | `rm -rf build/` |

### Build flags

All binaries are statically linked (`-static`), API level 35, AArch64. `-D_GNU_SOURCE`
is set in `CFLAGS` in the Makefile. **Do not add it to any header** — it must be defined
before system headers, and the Makefile flag guarantees that.

---

## Critical Implementation Details

### `exclude_kernel=0` is mandatory

On Oryon V2, cache refills are attributed to the **kernel context**, not the user context,
because the hardware walker runs at EL1. Setting `exclude_kernel=1` causes 23–33× undercounting
of `bus_access`. This is enforced unconditionally in `open_evt_raw()` in `pmu.h`.

### CPU pinning

All tools pin to cpu6 (Prime cluster) via `pin_to_cpu(6)` from `cpu.h`. The `buspoll_thread`
also pins — always set `BusPoll.cpu_pin = CPU_PIN` at initialization before starting the thread.

### PMU event status on Oryon V2 / Android 15

| Event | Code | Status |
|---|---|---|
| `bus_access` | 0x0019 | **Working** — accurate with `exclude_kernel=0` |
| `l2d_cache` | 0x0016 | Works but fires on every L1D miss (cannot distinguish L2/LLCC/DRAM) |
| `l2d_cache_refill` | 0x0017 | **Broken** — flat ~18 K/s regardless of WS |
| `l2d_cache_lmiss_rd` | 0x4009 | **Broken** — ~1 MB/s constant |
| `l3d_cache_allocate` | 0x0029 | **Broken** — always 0 |
| LLCC PMU (type=11) | — | **Inaccessible** on Android 15 (ENOENT) |

### `hwmon_thread` — trace_pipe parsing

`hwmon_thread()` reads `/sys/kernel/debug/tracing/trace_pipe` and parses two tracepoint types:

- **`dcvs/bw_hwmon_meas`** — hardware DRAM read bandwidth, ~4 ms cadence. Only fires when
  WS > LLCC (~112 MB).
- **`interconnect/icc_set_bw`** — DCVS software bandwidth vote, ~40 ms cadence. Filter by
  both path AND destination node to avoid double-counting:

  | Filter | Field | Metric |
  |---|---|---|
  | `path=chm_apps-qns_llcc` + `node=qns_llcc` | `agg_avg` | `icc_llcc_agg` |
  | `path=llcc_mc-ebi` + `node=ebi` | `agg_avg` | `icc_dram_agg` |

  Use `agg_avg` (aggregate across all ICC clients), not `avg_bw` (single client's vote).
  Units in trace are kBps; divide by 1000 for MB/s.

### Output CSV columns (bw_bench)

Summary file `data/bw_bench_summary.csv`:

```
workload, ws_mb, rep, bwmon_MBs, icc_llcc_agg_MBs, icc_dram_agg_MBs, bw_alg_MBs, bus_access_MBs, warmup_passes, skin_c
```

Timeseries files: `data/bw_bench_trace_{bwmon,icc,bus}.csv`, `data/bw_bench_context.csv`.
