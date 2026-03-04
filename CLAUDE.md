# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

C measurement tools for characterizing CPU DRAM/cache bandwidth on a Samsung Galaxy S25+
(Snapdragon 8 Elite, Oryon V2). Cross-compiles for Android AArch64 via the Android NDK.

## Repository Structure

```
src/               C source files
  common.h         Shared helpers (timer, PMU, pointer-chase, cache flush, statistics)
  bwprobe.c        Random pointer-chase sweep (2–256 MB WS), PMU + bwmon + CNTVCT latency
  matvec.c         Matrix-vector multiply, scalar vs explicit NEON (8-accumulator)
  prefault_exp.c   DRAM amplification isolation (baseline / MADV_DONTNEED / MADV_RANDOM)
  llcc_size.c      Effective LLCC capacity via flush-size sweep (24/128/256 MB)
analysis/
  plot_results.py  Post-processing and figure generation from CSV output
  figures/         Generated figures referenced by docs/slides.md
docs/
  slides.md        Marp presentation source
  slides.pdf       Rendered slides
```

## Build and Run

```bash
# NDK is at /home/seonjunkim/opt/android-ndk-r29
# Device ADB port: 5307

make NDK=/home/seonjunkim/opt/android-ndk-r29                         # build all → build/
make NDK=... ADB="adb -P 5307" runprobe     # → /tmp/bwprobe.csv / .log
make NDK=... ADB="adb -P 5307" runmatvec    # → /tmp/matvec.csv / .log
make NDK=... ADB="adb -P 5307" runprefault  # → /tmp/prefault_exp.csv / .log
make NDK=... ADB="adb -P 5307" runllcc      # → /tmp/llcc_size.csv / .log
make clean                                   # removes build/
```

**Device prerequisites:** root, `perf_event_paranoid=-1`, tracefs at `/sys/kernel/debug/tracing`.

## Critical Implementation Details

**`exclude_kernel=0` is mandatory on Oryon V2.** Cache refills are attributed to kernel
context; `exclude_kernel=1` causes 23–33× undercounting for `bus_access`.

**PMU event status:**
- `bus_access` (0x0019) — accurate DRAM metric; **1.034 ± 0.008×** vs bwmon at WS ≥ 128 MB
- `l2d_cache` (0x0016) — fires on every L1D miss; cannot distinguish L2/LLCC/DRAM
- `l2d_cache_refill` (0x0017) — **broken** (flat ~18K/s)
- `l2d_cache_lmiss_rd` (0x4009) — **broken** (~1 MB/s constant)
- `l3d_cache_allocate` (0x0029) — **broken** (always 0)
- LLCC PMU (type=11) — **inaccessible** on Android 15 (ENOENT)

**bw_hwmon_meas tracepoint** is the cluster-level DRAM ground truth (~4 ms samples).
Background thread reads `trace_pipe` and accumulates `bwmon-llcc-prime` MB/s samples.

**CPU frequency:** Locked to 4,473,600 kHz via `/data/local/tmp/powerctl.sh`.
**CPU pinning:** All tools pin to CPU6 (Prime cluster) via `pin_to_cpu(6)` in common.h.

## Current Empirical Findings

See README.md for full results. Key numbers:
- DRAM onset: **WS = 128 MB** (with flush+warmup; WS ≤ 112 MB stays cache-resident)
- Effective LLCC: **~112 MB** (all flush sizes 24/128/256 MB give same boundary)
- `bus_access` accuracy: **1.034 ± 0.008×** vs bwmon (21 DRAM-regime rep-points)
- CNTVCT latency: ~1 ns (cache), ~2.7 ns (LLCC), ~5.5 ns (DRAM)
- NEON matvec: **54 GB/s** vs scalar 5.6 GB/s (9.5×); DRAM-bandwidth-bound
- bwmon amplification: **~1.94×** from HW stream prefetcher (MADV_RANDOM has no effect)
