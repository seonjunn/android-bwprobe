# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

This is a C measurement tool (`bwprobe`) for characterizing CPU DRAM/cache bandwidth on a Samsung Galaxy S25+ (Snapdragon 8 Elite, Oryon V2). It cross-compiles for Android AArch64 via the Android NDK and runs on-device via `adb`.

## Build and Run

```bash
# NDK is at /home/seonjunkim/opt/android-ndk-r29
# Device is accessible at adb -P 5307

# Build (cross-compile for Android aarch64, API 35)
make NDK=/home/seonjunkim/opt/android-ndk-r29

# Push to device and run (writes CSV to /tmp/bwprobe.csv, summary to /tmp/bwprobe.log)
make NDK=/home/seonjunkim/opt/android-ndk-r29 ADB="adb -P 5307" runprobe

# Clean
make clean
```

**Device prerequisites:** root access, `perf_event_paranoid=-1`, tracefs at `/sys/kernel/debug/tracing`.

## Code Structure

- **`bwprobe.c`** — main program: sweeps working-set sizes (2–256 MB), runs a random pointer-chase workload, measures all PMU events simultaneously alongside the `bw_hwmon_meas` hardware tracepoint, and prints calibration results. CSV output includes `hops` and `chase_GBs` (= hops × 64 / elapsed) as a sanity-check column.
- **`common.h`** — shared helpers: `perf_event_open` syscall wrapper (not in Android NDK libc), timing (`CLOCK_MONOTONIC`), CPU affinity (`pin_to_cpu`), and the Fisher-Yates random pointer-chase buffer builder (`build_chase` / `prefault` / `run_chase`).

**Workload note:** `run_chase` executes 8 *sequential* dependent pointer dereferences per loop iteration — there is no actual MLP. Each dereference depends on the previous `p`, so the chain is fully serialized by memory latency.

## Critical Implementation Details

**`exclude_kernel=0` is mandatory on Oryon V2.** Cache refill operations are attributed to kernel context on this SoC; using `exclude_kernel=1` causes 23–33× undercounting for `l2d_cache_refill` and `bus_access`.

**PMU event status on Oryon V2:**
- `bus_access` (0x0019) — best DRAM metric at WS ≥ 128 MB; **1.00–1.01× accurate vs bw_hwmon with locked frequency**, thermally stable
- `l2d_cache` (0x0016) — fires per L1D miss; at WS ≥ 128 MB undercounts bwmon (0.35–0.63×); fires on ~0.02× of hops at WS=4 MB (L1D-resident), ~1.0× at WS ≥ 7 MB
- `l2d_cache_refill` (0x0017) — **broken**: flat ~18K/s regardless of WS
- `l2d_cache_lmiss_rd` (0x4009) — **broken**: ~1 MB/s constant
- `l3d_cache_allocate` (0x0029) — **broken**: always 0
- LLCC PMU (type=11) — **inaccessible** on Android 15 (returns `ENOENT`)

**bw_hwmon_meas tracepoint** (`dcvs/bw_hwmon_meas`) is the cluster-level DRAM ground truth (~4 ms samples). Only reports CPU-cluster DRAM misses — cache-resident workloads (WS ≤ ~6 MB) appear as background (~45 MB/s). DRAM signal rises above noise at WS ≥ 7 MB (779 MB/s) and reaches full saturation at WS ≥ 128 MB (~21 GB/s).

**CPU frequency:** Locked to max (4,473,600 kHz) via `/data/local/tmp/powerctl.sh` on the device. With locked frequency, thermal throttling of the CPU clock does not affect measurements even at elevated skin temperatures (44–48°C observed). The `thermstat` script (zone 53=skin, zone 55=battery) can be used for temperature monitoring.

**CPU pinning:** The binary pins to CPU6 (Oryon V2 Prime cluster) so that `bwmon-llcc-prime` tracepoint measurements align with the workload.

## Calibration Findings (empirical, fine-grained sweep, freq locked)

`chase_GBs` = hops × 64 / elapsed is the actual pointer-chase throughput — the key sanity-check column.
`l2d/chase` = l2d_cache_BW / chase_GBs — fraction of hops that miss L1D.

Selected points from the 21-point sweep (2–256 MB):

| WS (MB) | bwmon (MB/s) | chase_GBs | l2d/chase | bus_access/bwmon | Interpretation |
|---------|-------------|-----------|-----------|------------------|----------------|
| 4       | 245 (bg)    | 20.9      | 0.02×     | —                | 98% L1D hits — WS fits in L1D |
| 5       | 170 (bg)    | 21.1      | 0.38×     | —                | L1D boundary — 62% miss |
| 7       | 779         | 15.9      | 1.03×     | —                | L1D fully missed; DRAM starts |
| 8       | 1,393       | 10.1      | 1.02×     | 0.47×            | DRAM-active; chase now latency-limited |
| 64      | 1,678       | 10.0      | 1.01×     | 0.58×            | DRAM-active (not background!) |
| **128** | **21,378**  | **7.1**   | **1.05×** | **1.01×**        | **Full DRAM — calibration point** |
| 160     | 17,892      | 8.8       | 1.05×     | 1.00×            | Full DRAM confirmed |
| 256     | 15,394      | 9.2       | 1.05×     | 0.99×            | Full DRAM confirmed |

**Key findings:**

1. **L1D boundary is 4–5 MB.** l2d/chase = 0.02 at WS=4 MB (98% L1D hits), jumps to 0.38 at WS=5 MB. Sharp transition in one 1 MB step. Previous estimate of "4–8 MB" is now refined.

2. **DRAM traffic starts at WS ≥ 7–8 MB** (not 128 MB as the coarse sweep suggested). At WS=8 MB, bwmon=1,393 MB/s and chase_GBs drops from ~21 to ~10 GB/s. The earlier coarse-sweep "background-level bwmon at 32–64 MB" was a measurement state artifact (cool, idle device, possibly warm LLCC from prior runs). The LLCC (12 MB) does NOT absorb 32–64 MB working sets in general.

3. **`chase_GBs` transitions from ~20 (timer-limited) to ~10 (DRAM-latency-limited) at WS=7–8 MB.** At WS ≤ 5 MB, `now_s()` overhead (~22 ns/8 hops) dominates. At WS ≥ 8 MB, DRAM latency dominates. Intermediate values (~16 GB/s at WS=6–7 MB) reflect the crossover.

4. **`bus_access` is 1.00–1.01× accurate at WS=128–256 MB with locked frequency** (44–48°C skin, charging). Re-confirmed stable.

5. **WS=192 MB anomaly.** bwmon drops to 879 MB/s (background-level) after two heavy 128/160 MB runs. chase_GBs=14.5 and l2d/chase=0.03 — behaves as cache-resident. Likely OS memory reclamation (Android ZRAM) or transient LLCC state effect. Treat as unreliable.

6. **Measurement limitation:** `now_s()` inside the hot loop prevents isolation of L2 vs LLCC latency. A fixed hop-count design with external timing is needed.

## Measurement Architecture

`bwprobe` runs a background `pthread` that reads `trace_pipe` and accumulates `bwmon-llcc-prime` MB/s samples. The main thread takes before/after snapshots of the hwmon accumulator around each measurement window to compute a window-averaged DRAM bandwidth as ground truth, then computes ratios for each PMU event.

DRAM-regime accuracy statistics are accumulated only for WS ≥ `DRAM_WS_MIN_MB` (128 MB) AND when bwmon > 1000 MB/s (real DRAM traffic, not background). WS=128, 160, and 256 MB all meet this threshold; all sub-DRAM points (WS < 128 MB) measure LLCC-miss DRAM traffic but are excluded from the PMU calibration ratio.
