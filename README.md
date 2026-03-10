# Memory Bandwidth Characterization — Snapdragon 8 Elite (Oryon V2)

**Goal:** Find a per-process metric that characterizes the *memory intensity* of a workload
(reads + writes) to estimate the contention it will cause or experience in the **shared memory
subsystem** (LLCC and DRAM), where all processors (CPU clusters, GPU, DSP, display) compete.

Platform: Samsung Galaxy S25+, Snapdragon 8 Elite (SM8750), cpu6 (Prime cluster, 4.47 GHz).

---

## Platform

| Property | Value |
|---|---|
| CPU | Oryon V2 ×8: 2 Prime (cpu6–7) + 6 Gold (cpu0–5) |
| L2 | 12 MB / Prime cluster |
| LLCC | ~112 MB (9–10 slices; confirmed by Exp 4) |
| DRAM | LPDDR5X, 128-bit bus |
| OS / kernel | Android 15, 6.6.30 |
| Pinned core | cpu6; `bwmon-llcc-prime` covers cpu6–7 |
| CPU freq | Locked to 4,473,600 kHz via `/data/local/tmp/powerctl.sh` |

## Memory Hierarchy and Measurement Points

```
CPU core (cpu6, Prime cluster)
  L1D: 96 KB  — private per-core
  L2:  12 MB  — shared within Prime cluster (cpu6–7)
    ↑ bus_access (PMU 0x0019): all L2 misses toward shared fabric, reads + writes
  LLCC: ~112 MB — shared by all CPU clusters, GPU, DSP, display, audio
    ↑ bwmon (bw_hwmon_meas): DRAM reads only, fires when WS > LLCC
  LPDDR5X DRAM (128-bit bus)
```

---

## Key Findings

**Use `bus_access` for shared-subsystem contention characterization:**

- Fires at WS > L2 (> 12 MB) — captures LLCC-resident pressure that `bwmon` misses entirely.
- Counts reads **and** writes — `bus_access/bwmon = 1.56` for AXPY (2 reads + 1 write).
- Per-process isolation — `bwmon` is cluster-level and includes background traffic.
- `icc_llcc_agg` is the equivalent system-level proxy (~40 ms DCVS EMA vs. ~4 ms hardware).

LLCC absorbs **30.0% ± 0.1%** of all L2 misses, constant across workloads and WS sizes.
Absorption depends on *temporal reuse*, not working-set size vs. LLCC capacity.

---

## Quick Start

```bash
make NDK=/home/seonjunkim/opt/android-ndk-r29 ADB="adb -s 192.168.0.190:33291" runbench
python3 analysis/bw_bench_analysis.py
```

See [`docs/code_structure.md`](docs/code_structure.md) for all build targets, prerequisites,
and detailed per-file descriptions.

---

## Documentation

| Document | What it covers |
|---|---|
| [`docs/code_structure.md`](docs/code_structure.md) | Source layout, what each file does, build instructions, critical implementation notes |
| [`docs/bandwidth_measurement.md`](docs/bandwidth_measurement.md) | Memory hierarchy overview; all four metrics compared; regime table; metric selection guide |
| [`docs/pmu.md`](docs/pmu.md) | ARM PMU architecture; `bus_access` event semantics; `exclude_kernel=0` requirement |
| [`docs/hwmon.md`](docs/hwmon.md) | Qualcomm `bw_hwmon` hardware monitor; `bw_hwmon_meas` tracepoint |
| [`docs/icc.md`](docs/icc.md) | Linux ICC framework; DCVS; `icc_set_bw` tracepoint fields and node topology |
| [`docs/contention_metric.md`](docs/contention_metric.md) | Metric decision guide: `bus_access` vs `icc_llcc_agg` vs `bwmon` |
| [`docs/report.md`](docs/report.md) | Full experimental report: all six experiments with data tables and analysis |

**Suggested reading order for a new reader:**
1. This README (platform + key conclusions)
2. `bandwidth_measurement.md` (measurement mechanisms and regimes)
3. `contention_metric.md` (why `bus_access` wins)
4. `report.md` (data)
5. `pmu.md` / `hwmon.md` / `icc.md` (mechanism deep dives)
6. `code_structure.md` (when working with the code)

---

## Open Questions

| Question | Next step |
|---|---|
| HW prefetcher disable (confirm 1.9×) | Write `CPUACTLR_EL1` via kernel module |
| Multi-processor DRAM contention | CPU + GPU simultaneous run vs. baseline |
| WS=192 MB ZRAM anomaly (bwprobe) | Use `mlock()` to prevent page compaction |
| LLCC PMU access | Try Snapdragon Profiler via proprietary driver |
| ICC lag quantification | Longer rep windows (≥30 s) for cross-correlation |
