# Contention Metric Comparison — bus_access vs. icc_llcc_agg

Decision reference for characterizing memory contention intensity on the shared
subsystem (LLCC + DRAM) of SM8750 (Snapdragon 8 Elite), where CPU clusters,
GPU (Adreno), NPU/DSP (Hexagon), display engine, and audio compete for the
same LLCC and DRAM bandwidth.

---

## The Contention Domain

```
CPU clusters (Prime + Gold)  ─┐
GPU (Adreno)                  ├──→ LLCC (~112 MB shared) ──→ LPDDR5X DRAM
NPU / DSP (Hexagon CDSP)     ─┤
Display engine (MDP)          ┤
Audio DSP (ADSP), Video, UFS ─┘
```

Contention occurs in the LLCC and DRAM — resources shared by all processors.
L2 cache is private to each CPU cluster and causes no cross-processor
contention. The measurement boundary for contention characterization is
therefore **at the LLCC entry**, not within L2 and not at the DRAM boundary
(which misses LLCC-resident pressure).

---

## The Two Candidate Metrics

### bus_access

ARMv8 PMU event 0x0019. Counts 64-byte cache-line transfers across the L2
output port of the **measuring CPU process**. Per-process, CPU-only.

### icc_llcc_agg

`agg_avg` at the `qns_llcc` node from the `icc_set_bw` tracepoint.

**Scope correction:** `qns_llcc` is the LLCC QoS slave — the destination node
for **all** on-chip masters that access the LLCC via the ICC framework. The
`agg_avg` field aggregates bandwidth votes from every ICC client registered at
this node, not just CPU clusters. This includes CPU clusters (`chm_apps`),
Hexagon DSP/NPU (`remoteproc-cdsp`), Audio DSP (`remoteproc-adsp`), display
engine (`ae00000.qcom,mdss_mdp`), video codec (`aa00000.qcom,vidc`), and
storage (`1d84000.ufshc`).

**Current implementation caveat:** `hwmon.h` filters by
`path=chm_apps-qns_llcc AND node=qns_llcc`, capturing only CPU-path-triggered
`icc_set_bw` events. The `agg_avg` value in those events is still the
system-wide aggregate at `qns_llcc`, but new events are only seen when the CPU
DCVS client updates its vote. If a non-CPU processor (e.g., NPU) is active
while the CPU is idle, no `chm_apps` events fire and NPU-triggered updates are
missed. To capture all processors' updates, filter by `node=qns_llcc` across
all paths, not just the CPU path.

**GPU exception:** Adreno GPU compute traffic bypasses the ICC framework
entirely (GMU path). It does not appear in `icc_llcc_agg` regardless of filter.

---

## Pros and Cons

### bus_access

| Dimension | Detail |
|---|---|
| **Accuracy** | High. Hardware counter, no smoothing, no lag. Tracks bwmon at 1.034 ± 0.008× for read-only workloads (Pearson r = 0.9997). Resolves low-bandwidth LLCC-resident workloads (~85 MB/s) clearly. |
| **Background traffic** | Excluded. Counts only the measured process. Score is stable and reproducible across different system states. |
| **Prefetcher traffic** | Included. `bus_access / bw_alg ≈ 1.9×` for streaming. Valid for contention: prefetcher fetches compete for LLCC bandwidth. Note the multiplier when comparing to algorithmic demand. |
| **Temporal resolution** | ~4 ms (polled). Resolves sub-second bandwidth changes. |
| **Processor coverage** | CPU only. Measures one process on one CPU cluster. No analog exists for GPU (GMU), NPU (Hexagon PMU inaccessible on Android 15), or display. |
| **SoC scalability** | Not scalable to a unified cross-processor metric. Each processor type requires a different mechanism with different calibration. |
| **Infrastructure** | `perf_event_open`, `perf_event_paranoid=-1`, root. Oryon V2: `exclude_kernel=0` mandatory (23–33× undercounting otherwise). |

### icc_llcc_agg

| Dimension | Detail |
|---|---|
| **Accuracy** | Lower. DCVS EMA over ~40 ms window introduces lag and amplitude smoothing. Low-bandwidth workloads may not rise above the background floor. High-bandwidth streaming is accurate (neon_matvec WS=16 MB: `icc_llcc_agg / bw_alg = 1.007`). |
| **Background traffic** | Included in `agg_avg`. All active ICC clients at `qns_llcc` contribute — CPU (all clusters), DSP/NPU, display, audio, storage. Background fluctuates across runs. |
| **Prefetcher traffic** | Included (same reasoning as bus_access). |
| **Temporal resolution** | ~40 ms (DCVS voting period). ~12 events per 5 s window. Cannot resolve sub-second bursts. |
| **Processor coverage** | CPU clusters + all non-GPU ICC clients (DSP/NPU, display, audio, video, storage) via `agg_avg`. GPU excluded (GMU). **Potentially the widest-coverage single metric on the platform, minus GPU.** |
| **SoC scalability** | Good for non-GPU processors. Single metric, single measurement point, consistent units for CPU + NPU + display traffic combined. |
| **Infrastructure** | Tracefs only, no PMU. Root required. Shared `trace_pipe` with bwmon. |

---

## Workload Isolation: Can We Attribute Traffic to a Single Processor?

If CPU and NPU workloads are run **in isolation** (one active at a time), the
`agg_avg` at `qns_llcc` approximates that processor's contribution:

```
isolated_processor_traffic ≈ icc_llcc_agg(workload) − icc_llcc_agg(idle)
```

The idle baseline captures the irreducible background from display, audio, and
other always-on ICC clients. Subtracting it leaves the workload's contribution.

Two caveats:
1. **Event trigger gap.** If filtering by `path=chm_apps-qns_llcc` (CPU path
   only), NPU-active/CPU-idle periods produce no new events. The fix is to
   filter by `node=qns_llcc` regardless of path — then any processor's ICC
   update triggers a new `agg_avg` sample.
2. **Background stability.** The baseline is not perfectly constant (display
   refresh rate, background app scheduling). For clean attribution, measure idle
   baseline in the same system state immediately before or after the workload.

With these caveats addressed, isolated execution + idle subtraction is a
practical approach for attributing LLCC-entry pressure to each processor
without per-process PMU access.

---

## Side-by-Side Comparison

| Dimension | `bus_access` | `icc_llcc_agg` |
|---|---|---|
| **Mechanism** | ARMv8 PMU hardware counter | Linux ICC DCVS software vote |
| **Measurement boundary** | L2 output port → LLCC | LLCC entry (all ICC clients) |
| **What it counts** | Cache-line transfers (reads + writes) of the measured process | Aggregated bandwidth vote from all active ICC clients at `qns_llcc` |
| **Scope** | Per-process, one CPU | All non-GPU ICC clients: CPU clusters + NPU/DSP + display + audio + video + storage |
| **GPU coverage** | No | No (Adreno bypasses ICC via GMU) |
| **Background** | Excluded — workload signal only | Included — all active ICC clients contribute |
| **Precision** | High — hardware counter, no smoothing | Lower — DCVS EMA, ~40 ms lag, may not resolve low-BW workloads |
| **Temporal resolution** | ~4 ms | ~40 ms |
| **Low-BW sensitivity** | Yes — resolves ~85 MB/s LLCC-resident workload | Limited — 85 MB/s workload may be buried in background floor |
| **Infrastructure** | `perf_event_open`, `perf_event_paranoid=-1`, root; `exclude_kernel=0` mandatory on Oryon V2 | Tracefs only, root required; shared `trace_pipe` with bwmon |
| **Cross-processor attribution** | Not possible — CPU-only, no NPU/GPU analog | Possible via isolation + idle subtraction (with event trigger caveat) |
| **Use case** | Characterize one CPU workload's intrinsic memory intensity | Monitor total non-GPU memory pressure; attribute NPU/DSP traffic via isolation |

---

## kgsl GMU Counters — GPU Bandwidth Measurement

GPU (Adreno) compute traffic bypasses the Linux ICC framework entirely. The GPU uses
its own dedicated path through the GMU (Graphics Management Unit), a firmware
co-processor that manages GPU power states and issues DDR frequency votes
independently of the CPU DCVS stack.

### What kgsl GMU counters measure

The `kgsl` kernel driver exposes GPU performance counters via
`/sys/class/kgsl/kgsl-3d0/`. The GMU internally tracks GPU DRAM bandwidth demand
to decide DDR operating points. What is accessible depends on driver version:

- **DDR vote:** The GMU submits a DDR bandwidth vote (similar in role to
  `icc_set_bw`) via the proprietary `adreno_devbw` device, not through the standard
  ICC framework. This vote reflects the GPU's DRAM demand as seen by the firmware.
- **GPU hardware counters:** Via `kgsl` devfreq or Snapdragon Profiler's proprietary
  path, individual Adreno hardware performance counters (read bandwidth, write
  bandwidth, cache hit rate) can be sampled. These are analogous to PMU events on
  the CPU side.

### Analogy to CPU metrics

| GPU mechanism | Closest CPU analog | Key difference |
|---|---|---|
| GMU DDR vote (adreno_devbw) | `icc_dram_agg` at `ebi` | DRAM boundary, firmware estimate, EMA smoothed — not an LLCC-entry measurement |
| Adreno hardware BW counter (via Snapdragon Profiler) | `bus_access` | Hardware ground truth; but measures at GPU NOC, not LLCC entry; inaccessible from standard Android userspace |

The GMU vote is the closer of the two to what is accessible without proprietary
tools: it is a software bandwidth estimate at the DRAM boundary, analogous to
`icc_dram_agg`. It suffers the same structural limitation as `icc_dram_agg` —
it cannot capture LLCC-resident GPU pressure.

There is **no known GPU analog of `bus_access` at the LLCC entry** accessible from
Android 15 userspace. The Adreno LLCC interface (GPU System Cache / GSC) exists in
hardware, but its performance counters require either the proprietary Snapdragon
Profiler connection or a kernel module. From standard `kgsl` devfreq sysfs, only the
DRAM-boundary DDR vote is exposed.

### Implication for cross-processor contention

The GPU has the same structural measurement blind spot as other processors for
LLCC-resident pressure:

```
GPU workload
  → LLCC (GPU system cache, shared with CPU)
      [GMU DDR vote / kgsl counters are blind here if data is LLCC-resident]
  → LPDDR5X DRAM
      [GMU DDR vote fires here — firmware DRAM estimate]
```

A GPU workload whose data fits in the LLCC (e.g., convolution filters reused across
many spatial positions) generates LLCC pressure that is invisible to both `kgsl`
DDR vote counters and the CPU-side `bwmon`. The only path to measuring this on
SM8750 is via the proprietary Adreno performance counter interface.

**In summary:** kgsl GMU counters are most analogous to `icc_dram_agg` (DRAM
boundary, software estimate), not to `bus_access` (LLCC entry, hardware counter,
per-process). They do not plug the GPU coverage gap for LLCC-resident contention.

---

## Decision Summary

| Goal | Recommended metric | Reason |
|---|---|---|
| CPU kernel characterization (offline ranking) | `bus_access` | Per-process, no background, high precision, stable across runs |
| CPU + NPU/DSP joint contention monitoring | `icc_llcc_agg` | Single metric, same boundary, covers all non-GPU ICC clients |
| Attribution via isolation | `icc_llcc_agg` (with idle subtraction) | Workload-specific contribution without per-process PMU |
| GPU contention (DRAM-level) | kgsl GMU DDR vote (adreno_devbw) | Analogous to `icc_dram_agg`; DRAM boundary only; LLCC-resident GPU pressure unmeasurable from standard Android userspace |

**Robustness note.** This ranking is stable across workload regimes. In the
streaming regime (matvec, matmul — LLCC transparent), `bwmon` becomes a valid
read-only ground truth and `icc_dram_agg` loses its boundary objection. But the
two candidates do not lose their advantages: `bus_access` still counts writes and
is per-process; `icc_llcc_agg` was already at the correct boundary. In the
LLCC-resident regime (looping random access), `bwmon` and `icc_dram_agg` are
blind. The same two metrics remain the correct choices across the full workload
space.

For the **ultimate goal** of characterizing per-workload memory contention
intensity across CPU, GPU, and NPU:
- CPU: `bus_access` (primary), `icc_llcc_agg` (fallback)
- NPU/DSP: `icc_llcc_agg` with isolation + idle subtraction
- GPU: requires a separate GPU-specific measurement path (GMU counters)
- Combined DRAM-level view: `icc_dram_agg` at `ebi` (all ICC clients; still
  excludes GPU; wrong boundary for LLCC-resident pressure but useful for
  DRAM-level budget analysis)
