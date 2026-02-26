# Memory Contention Characterization on Qualcomm Snapdragon 8 Elite

## Device Under Test

| Property       | Value                                                                       |
|----------------|-----------------------------------------------------------------------------|
| Model          | Samsung Galaxy S25+ (SM-S936U)                                              |
| SoC            | Qualcomm Snapdragon 8 Elite (SM8750)                                        |
| CPU            | Qualcomm Oryon V2 × 8 cores                                                 |
| CPU topology   | Cluster 0: cpu0–5 @ 3.53 GHz (Gold), Cluster 1: cpu6–7 @ 4.47 GHz (Prime) |
| Cache (per core)| L1D 96 KB, L2 12 MB/cluster (Oryon V1 reference; V2 mobile unconfirmed)   |
| LLCC           | 12 MB shared across all clients (CPU, GPU, NPU)                             |
| DRAM           | LPDDR5X, 4 × 16-bit channels, 9.6–10.67 Gbps → **77–85 GB/s peak**        |
| OS             | Android 15, Linux kernel 6.6.30                                             |
| Access         | root, `perf_event_paranoid = -1`                                            |

---

## Build and Run

### Prerequisites

- [Android NDK](https://developer.android.com/ndk/downloads) (r25 or later; tested with r29)
- `adb` in your `PATH` with the device connected

### Configuration

Set the following environment variables or pass them on the `make` command line:

| Variable           | Default                  | Description                                      |
|--------------------|--------------------------|--------------------------------------------------|
| `NDK`              | `$ANDROID_NDK_HOME`      | Path to the unpacked Android NDK root            |
| `API`              | `35`                     | Android API level to target                      |
| `ARCH`             | `aarch64`                | Target CPU architecture                          |
| `ADB`              | `adb`                    | ADB command (override to select a specific device, e.g. `adb -s <serial>`) |
| `DESTDIR`          | `/data/local/tmp`        | On-device directory to push the binary           |

The recommended way to point at a specific NDK installation is to export
`ANDROID_NDK_HOME` once in your shell:

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r29
```

To target a specific device, either set `ANDROID_SERIAL` (respected by `adb`
automatically) or pass an explicit `ADB` override:

```bash
export ANDROID_SERIAL=192.168.0.2:40411   # adb uses this automatically
# — or —
make ADB="adb -s 192.168.0.2:40411" runprobe
```

### Build

```bash
make              # builds ./bwprobe for Android (aarch64, API 35 by default)
```

Override variables inline if needed:

```bash
make NDK=/opt/android-ndk-r29 API=34
```

### Run on device

```bash
make runprobe
```

This pushes `bwprobe` to `$DESTDIR` on the device, executes it, and writes:

- `/tmp/bwprobe.csv` — raw per-sample CSV output (stdout from the binary)
- `/tmp/bwprobe.log` — summary / human-readable log (stderr from the binary)

The summary is printed to the terminal at the end of the run.

### Clean

```bash
make clean        # removes the local bwprobe binary
```

---

## 1. The Problem: Two Distinct Contention Points

Three processor types share both the LLCC and DRAM:

```
CPU (8× Oryon V2) ──┐
GPU (Adreno 830)  ──┼──► LLCC (12 MB shared cache) ──► DRAM (77–85 GB/s)
NPU (Hexagon CDSP)──┘
```

When GPU ML inference or NPU inference runs concurrently with a CPU real-time task,
they can contend at two distinct levels:

**Contention point 1 — LLCC capacity and bandwidth:**
All processors share the 12 MB LLCC.  A GPU workload holding 10 MB of texture data
and a CPU workload holding 8 MB of working set together exceed LLCC capacity, evicting
each other's data and forcing extra DRAM accesses for both.  Even without capacity
overflow, the LLCC interconnect has finite bandwidth that all clients share.  This
contention occurs even when each processor's working set fits in cache individually.

**Contention point 2 — DRAM bandwidth:**
LLCC misses from all processors compete for the 77–85 GB/s DDR bandwidth ceiling.
This is the classical bandwidth contention: once working sets exceed LLCC, every
processor's cache miss goes to the same DRAM bus.

These two contention points require different measurement approaches.  No single
metric covers both.

---

## 2. Memory Hierarchy: Theoretical Bandwidth

Understanding where measured numbers sit relative to hardware limits:

| Level | Size           | Theoretical peak BW       | Notes                                    |
|-------|----------------|---------------------------|------------------------------------------|
| L1D   | 96 KB/core     | ~640 GB/s/core            | 4 ld/cycle × 64 B × 4.47 GHz            |
| L2    | 12 MB/cluster  | ~330 GB/s/cluster         | Oryon V1 measured; V2 mobile estimated   |
| LLCC  | 12 MB shared   | Not published             | Must serve 8 CPU cores + GPU + NPU       |
| DRAM  | —              | **77–85 GB/s** (peak)     | 4 × 16-bit LP5X, 9.6–10.67 Gbps         |

**Critical implication — DRAM saturation risk:**
Our calibrated measurement shows a single Oryon V2 Prime core running a random
pointer-chase achieves ~11.6 GB/s DRAM throughput (~14% of peak).  Oryon's
out-of-order engine supports 50+ in-flight memory requests per core; with a
prefetcher-friendly workload, per-core DRAM throughput can approach the L2
bandwidth figure (~82 GB/s).  Eight cores running simultaneously would trivially
exceed the 77–85 GB/s DRAM ceiling — any CPU workload that reaches DRAM is a
realistic contention threat to GPU and NPU.

---

## 3. Measurement Mechanisms

### 3.1 ARM PMUv3 via `perf_event_open(2)` — per-process, per-core

The CPU exposes hardware performance counters through the standard Linux perf API.

```c
struct perf_event_attr a = {
    .type           = 10,      /* ARMV8_PMU_TYPE (from sysfs type file) */
    .config         = 0x0016,  /* event code — see table below */
    .exclude_kernel = 0,       /* MANDATORY on Oryon V2 — see note below */
    .disabled       = 1,
};
int fd = perf_event_open(&a, 0 /*pid*/, -1 /*any cpu*/, -1, 0);
/* ... run workload ... */
uint64_t count;
read(fd, &count, sizeof(count));
double bw_MB_s = (double)count * 64.0 / elapsed_s / 1e6;
```

**Critical note — `exclude_kernel=0` is mandatory on Oryon V2:**
Cache refill operations are attributed to kernel context on this SoC.
Setting `exclude_kernel=1` causes 23–33× undercounting:

| Event             | excl_kernel=1 | excl_kernel=0 | Ratio |
|-------------------|---------------|---------------|-------|
| l2d_cache_refill  | 866           | 20,030        | 23×   |
| bus_access        | 4,188         | 136,466       | 33×   |
| l1d_cache_refill  | 174,953       | 275,903       | 1.6×  |

**Event behavior on Oryon V2 (calibrated):**

| Event                | Config  | ARM spec meaning         | Behavior on Oryon V2                         |
|----------------------|---------|--------------------------|----------------------------------------------|
| `l2d_cache`          | 0x0016  | L2 data cache access     | Every L1D miss; **dual use — see § 5.2**    |
| `bus_access`         | 0x0019  | Memory bus transaction   | **1.04× accurate at WS=128 MB; thermally stable** |
| `l1d_cache_refill`   | 0x0003  | L1D miss                 | ~0.78× undercount of DRAM traffic            |
| `l2d_cache_refill`   | 0x0017  | L2 miss → system cache   | **Broken — flat ~18K/s regardless of WS**   |
| `l2d_cache_lmiss_rd` | 0x4009  | L2 long-latency miss     | **Broken — ~1 MB/s constant**                |
| `l3d_cache_allocate` | 0x0029  | L3 allocation            | **Broken — always 0 on Oryon V2**            |
| `HW_CACHE_MISSES`    | generic | Generic LLC miss         | Returns 0 — generic event not mapped         |

Note: `l2d_cache_refill` (0x0017) is the event that would naturally proxy LLCC demand
(L2→LLCC misses), and `l2d_cache_lmiss_rd` (0x4009) would proxy DRAM misses.  Both
are broken on this SoC, leaving `l2d_cache` (L1D misses) as the only usable CPU
pressure signal via PMU.

### 3.2 `bw_hwmon_meas` Tracepoint — CPU-cluster LLCC→DRAM traffic

Qualcomm's DCVS subsystem includes a hardware bandwidth monitor (bwmon) in the LLCC
fabric.  It measures LLCC-to-DRAM traffic attributed per CPU cluster every ~4 ms:

```
Tracepoint : dcvs/bw_hwmon_meas
Fields     : dev (string), mbps (uint64), us (uint64), wake (int)
Devices    : bwmon-llcc-prime  → cpu6–7 (Prime cluster)
             bwmon-llcc-gold   → cpu0–5 (Gold cluster)
```

**What it measures — and what it does not:**
`bw_hwmon` sits between the CPU cluster and LLCC and counts DRAM traffic attributable
to that cluster.  It does NOT capture:
- GPU DRAM traffic (KGSL uses a separate hardware path)
- NPU DRAM traffic
- CPU traffic that hits in LLCC (cache-resident loads are invisible to bwmon)

At cache-resident working sets, bwmon shows only background system traffic (~24–53
MB/s from display controller and other drivers) regardless of how much LLCC bandwidth
the CPU is consuming.  The workload's LLCC-level traffic is not visible here.  bwmon
only becomes informative once the CPU workload generates enough DRAM misses to rise
above background noise (~100 MB/s threshold in practice).

```bash
echo 1 > /sys/kernel/debug/tracing/events/dcvs/bw_hwmon_meas/enable
echo 1 > /sys/kernel/debug/tracing/tracing_on
cat /sys/kernel/debug/tracing/trace_pipe
# output: ... bw_hwmon_meas: dev: bwmon-llcc-prime, mbps = 11504, us = 4002, wake = 0
```

It cannot attribute traffic to individual processes within a cluster — it is
cluster-wide.  For per-process attribution, use PMU events (§ 3.1).

### 3.3 `memlat_dev_meas` Tracepoint — per-core memory intensity ratio

The DCVS subsystem also exposes per-core memory intensity:

```
Tracepoint : dcvs/memlat_dev_meas
Key fields : dev_id (core number 0–7), inst (instruction count),
             mem (memory event count), ratio (inst/mem = inverse intensity)
Rate       : ~8 ms per core
```

Low ratio (few instructions per memory event) signals a memory-bound phase regardless
of working set size.  The `mem` field is driven by CPUCP hardware (event 0x1000),
which is inaccessible via `perf_event_open`.  This tracepoint gives intensity but
not bandwidth in MB/s.

### 3.4 `icc_set_bw` Tracepoint — demand-side bandwidth votes

Linux's interconnect framework logs bandwidth requests from each device:

```
Tracepoint : interconnect/icc_set_bw
Fields     : path_name, dev, avg_bw (kBps), peak_bw (kBps)
```

Active paths confirmed on this device:

| path_name           | dev                         | Processor | Meaning                               |
|---------------------|-----------------------------|-----------|---------------------------------------|
| `chm_apps-qns_llcc` | `soc:qcom,dcvs:llcc:sp`     | CPU       | Aggregated CPU DRAM demand; kBps      |
| `llcc_mc-ebi`       | `soc:qcom,dcvs:ddr:sp`      | Total     | All-processor DRAM aggregate; kBps    |
| `qnm_nsp-ebi`       | `32300000.remoteproc-cdsp`  | CDSP/NPU  | NSP DDR demand; fires during NPU      |
| `alm_gpu_tcu-ebi`   | `3da0000.kgsl-smmu`         | GPU SMMU  | GPU page table walks only; usually 0  |

**Important:** these are bandwidth *votes* (what each driver's DCVS governor is
requesting), not measured traffic.  A vote may lead or lag actual usage.  For CPU,
`bw_hwmon_meas` is more accurate.  For NPU, `qnm_nsp-ebi` is the best available
signal but remains unverified against actual throughput.

The GPU (`3d00000.qcom,kgsl-3d0`) **never** appears in `icc_set_bw` traces.  KGSL
manages GPU DDR bandwidth via `kgsl_busmon` devfreq → GMU, bypassing the Linux
interconnect framework entirely.  See § 3.5.

### 3.5 `kgsl_buslevel` / `kgsl_pwrstats` — GPU DRAM (KGSL path)

GPU memory traffic is only observable through KGSL-specific tracepoints:

```
Tracepoint : kgsl/kgsl_buslevel    (fires every ~100 ms)
  bus      : GPU DDR level index (0–11; max in DT = 18,596,632 kBps = 18.2 GB/s)
  avg_bw   : hardware-measured GPU DRAM bandwidth — likely MB/s (KGSL source: bus_ab_mbytes)
             minimum observed at near-idle: ~534 MB/s (floor, even at 0.03% GPU busy)

Tracepoint : kgsl/kgsl_pwrstats    (fires on GPU power state changes)
  ram_time / total_time  : fraction of time GPU RAM was occupied
  ram_wait / total_time  : fraction of time GPU stalled waiting for RAM — contention signal
```

The unit of `avg_bw` is likely MB/s based on KGSL source code (`bus_ab_mbytes`) but
has not been verified against an independent measurement on this device.  The 534 MB/s
floor at idle reflects hardware minimum clock levels, not actual workload traffic.

### 3.6 LLCC PMU — inaccessible on Android 15

The LLCC hardware PMU (perf type=11) exists in the kernel but is blocked on Android 15:
every `perf_event_open` with type=11 returns `ENOENT`.  The sysfs entry has no
`format/` or `events/` subdirectories.

This is the most significant measurement gap.  Direct LLCC visibility would provide:
- Per-client (CPU / GPU / NPU) LLCC hit and miss rates
- LLCC eviction events attributable to each client
- Confirmation of LLCC capacity contention in mixed-workload scenarios

Without it, LLCC-level contention between processors must be inferred indirectly
(e.g., elevated DRAM traffic on bwmon when both CPU and GPU are active simultaneously,
compared to their individual baselines).

---

## 4. Calibration Experiment

### Method

Tool: `bwprobe` (this directory).  Runs a random pointer-chase workload at each
working-set size, measuring all PMU events simultaneously alongside `bw_hwmon_meas`.

```
Working sets  : 2, 4, 8, 16, 32, 64, 128, 256 MB
Duration      : 5 s per point (+ 0.5 s warm-up)
CPU           : pinned to cpu6 (Oryon V2 Prime, 4.47 GHz)
Workload      : random pointer-chase (Fisher-Yates Hamiltonian cycle, 64-byte stride,
                8-way unrolled for memory-level parallelism)
                defeats all hardware prefetchers; forces demand cache/DRAM accesses
Reference     : bw_hwmon_meas.mbps (bwmon-llcc-prime, ~4 ms samples, averaged)
Ratio         : PMU estimate / bw_hwmon_actual  (1.0 = perfect agreement with bwmon)
```

### Results

Measured on device (Galaxy S25+, Oryon V2 Prime @ cpu6, 5 s per WS point):

| WS (MB)  | bw_hwmon (MB/s) | l2d_cache BW   | l2d_cache ratio | bus_access ratio | Regime                         |
|----------|-----------------|----------------|-----------------|------------------|--------------------------------|
| 2        | 32 (bg)         | ~8.4 GB/s      | 261×            | 0.36×            | L2-resident                    |
| 4        | 32 (bg)         | ~0.5 GB/s      | 17×             | 0.36×            | L2-resident                    |
| 8        | 53 (bg)         | ~23.6 GB/s     | 446×            | 0.69×            | LLCC-resident                  |
| 16       | 29 (bg)         | ~19.7 GB/s     | 681×            | 0.36×            | LLCC-resident                  |
| 32       | 24 (bg)         | ~21.7 GB/s     | 903×            | 0.42×            | LLCC-resident                  |
| 64       | 31 (bg)         | ~22.1 GB/s     | 713×            | 0.36×            | Transitional (DRAM below noise)|
| **128**  | **11,624**      | **~13.2 GB/s** | **1.07×**       | **1.04×**        | **DRAM-resident ← use this**   |
| 256      | 5,111           | ~13.9 GB/s     | 2.71×           | **1.03×**        | Thermal throttle (CPU 2.2 GHz) |

"bg" = bwmon shows background system DRAM only (~24–53 MB/s from display + drivers);
workload DRAM contribution is below the noise floor.

"l2d_cache BW" = `l2d_cache_events × 64 B / elapsed_s`, i.e., the L1D-miss
traffic volume as seen by the CPU.  This measures LLCC demand at cache-resident WS
and DRAM demand at DRAM-resident WS.

---

## 5. Interpreting the Calibration Results

### 5.1 What bw_hwmon actually measures

`bwmon-llcc-prime` counts DRAM traffic originating from the Prime CPU cluster (cpu6–7)
that has missed LLCC.  It does NOT count traffic that hits in LLCC.

Evidence: at WS = 2–64 MB the CPU is issuing thousands of L1D misses per second —
visible in `l2d_cache` showing ~8–24 GB/s of demand traffic — yet bwmon reports only
background levels (~24–53 MB/s).  Those L1D misses are being served by L2 or LLCC
and never reach DRAM.  bwmon is silent throughout.

This means bwmon is **not** a ground truth for total CPU memory pressure.  It is
ground truth only for CPU-cluster DRAM traffic, and only when that traffic exceeds
the background noise floor (~100 MB/s).

### 5.2 The `l2d_cache` event as a dual-purpose metric

Because `l2d_cache` (0x0016) fires on every L1D miss regardless of where the miss
resolves, it measures different things depending on working set:

| Working set regime         | What l2d_cache × 64 B / elapsed measures          |
|----------------------------|----------------------------------------------------|
| WS fits in L2              | L2 access rate (L1D→L2 bandwidth demand)           |
| WS fits in LLCC, not L2   | LLCC access rate (L2 miss → LLCC bandwidth demand) |
| WS exceeds LLCC            | DRAM access rate (~1.07× of bw_hwmon at WS=128 MB) |

At cache-resident WS, the "ratio" column in the calibration table is not an error —
it correctly shows that the CPU is driving significant LLCC traffic (8–24 GB/s) while
DRAM traffic remains near zero.  The large ratio simply reflects that bwmon has nothing
to compare against (its denominator is background noise, not workload DRAM).

For contention prediction, `l2d_cache` is the right metric for LLCC-level pressure
and `bw_hwmon` is the right metric for DRAM-level pressure.  Both are needed.

### 5.3 Why WS = 64 MB still reads as cache-resident

The L2 (12 MB/cluster) + LLCC (12 MB shared) gives roughly 24 MB of effective cache
for a single-threaded workload.  A 64 MB working set clearly exceeds this.  Yet bwmon
shows only background at WS = 64 MB.

The explanation is temporal: the 8-way pointer-chase at LLCC-miss latency (~50–100 ns)
revisits each 64-byte line slowly.  In a 5-second window, only a fraction of the 64 MB
working set is accessed.  The DRAM traffic from the workload works out to approximately
2–4 MB/s — completely buried in the 31 MB/s background.  The true DRAM-detection
threshold is not the cache capacity (~24 MB) but the point where workload DRAM traffic
exceeds the noise floor, which happens around WS = 128 MB with 8-way MLP.

### 5.4 The latency-tolerance effect: why LLCC demand > L2 demand

In the calibration table, WS = 2 MB (L2-resident) shows ~8.4 GB/s of L1D-miss
traffic, while WS = 8–64 MB (LLCC-resident) shows ~20–24 GB/s.  This is not a
contradiction — it reflects Oryon V2's aggressive out-of-order execution.

At L2 latency (~15 cycles), the OoO engine can queue fewer in-flight requests before
the pipeline stalls.  At LLCC latency (~50–100 cycles), the engine queues more
outstanding misses to hide the longer latency, achieving higher aggregate throughput.
Oryon supports 50+ in-flight memory requests per core (from architecture analysis),
so longer latency paradoxically unlocks higher bandwidth from the CPU's perspective.

At DRAM latency (~80–100 ns = 360–450 cycles), the OoO engine queues even more
in-flight misses, but the DRAM bandwidth ceiling (77–85 GB/s) is now the binding
constraint.  A single-core random-access workload achieves ~11.6 GB/s, implying
~15 effective in-flight requests (`15 × 64 B / 100 ns ≈ 9.6 GB/s` plus LLCC partial
hits bringing the effective latency down toward ~44 ns).

---

## 6. Scale Analysis: Measured vs Theoretical

### DRAM-resident (WS = 128 MB)

```
Single core measured:   11.6 GB/s  (random, anti-prefetch, 8-way MLP)
DRAM theoretical peak:  77–85 GB/s
Single-core utilization: ~14%
8-core projection:       ~93 GB/s  → exceeds DRAM peak → saturation risk
```

Any multi-core workload that pushes individual cores into the DRAM-resident regime
will realistically compete for the full DRAM bandwidth, leaving little headroom for
GPU or NPU.

### Cache-resident LLCC demand (WS = 8–64 MB)

```
Single core measured:   ~20–24 GB/s  (LLCC-resident, from l2d_cache × 64B)
L2 theoretical:         ~330 GB/s / cluster (shared by up to 6 cores on Gold)
LLCC: not published;    estimated >100 GB/s to sustain all clients
Single-core utilization: <10% of L2 cluster bandwidth
8-core projection:       ~160–192 GB/s  → possible LLCC bandwidth saturation
```

At LLCC-resident working sets, a single core is not a LLCC bandwidth threat, but all
eight cores simultaneously running cache-resident workloads could saturate the LLCC
interconnect — especially while the GPU is also making LLCC requests.

---

## 7. CPU Bandwidth: Measurement Conclusion

### For DRAM contention (WS ≥ 128 MB)

**Cluster-level ground truth:** `bw_hwmon_meas.mbps`

```c
/* Enable once at startup */
write(open(".../events/dcvs/bw_hwmon_meas/enable", O_WRONLY), "1", 1);
write(open(".../tracing_on", O_WRONLY), "1", 1);

/* Stream from trace_pipe in background thread */
int fd = open(".../trace_pipe", O_RDONLY | O_NONBLOCK);
/* Parse: "bw_hwmon_meas: dev: bwmon-llcc-prime, mbps = <N>, us = <T>, wake = <W>" */
/* mbps = CPU cluster DRAM bandwidth over the last ~4 ms window */
```

**Per-process ground truth:** `bus_access` (0x0019)

```c
struct perf_event_attr a = {
    .type           = 10,      /* ARMV8_PMU_TYPE */
    .config         = 0x0019,  /* bus_access — thermally stable */
    .exclude_kernel = 0,       /* mandatory */
    .disabled       = 1,
};
int fd = perf_event_open(&a, target_pid, -1, -1, 0);
/* ... measurement window of elapsed_s seconds ... */
uint64_t count;
read(fd, &count, sizeof(count));
double bw_MB_s = (double)count * 64.0 / elapsed_s / 1e6;
```

Calibrated accuracy at WS = 128 MB:

| Event        | Ratio vs bw_hwmon | WS = 256 MB (throttle) | Usable?         |
|--------------|-------------------|------------------------|-----------------|
| `bus_access` | **1.04×**         | **1.03×** (stable)     | Yes, WS ≥ 128 MB|
| `l2d_cache`  | **1.07×**         | 2.71× (degrades)       | 128 MB only     |

`bus_access` is preferred because it counts memory bus transactions directly tied to
DRAM transfers, making it insensitive to CPU frequency or residual LLCC state.

### For LLCC contention pressure (any WS)

Use `l2d_cache` (0x0016) as the LLCC demand signal:

```c
.config = 0x0016;  /* l2d_cache — L1D miss rate = LLCC/DRAM request rate */
double pressure_GB_s = (double)count * 64.0 / elapsed_s / 1e9;
```

This is not a DRAM measurement at cache-resident WS — it is a measurement of how
hard the CPU is hitting the memory hierarchy.  At DRAM-resident WS it converges to
bw_hwmon (1.07× ratio).

### Decision guide

```
What level of contention are you characterizing?

LLCC-level pressure (any WS):
  → l2d_cache × 64 / elapsed_s   (L1D miss rate; valid at all WS sizes)
  → memlat_dev_meas.ratio         (inverse intensity; dimensionless, per-core)

DRAM-level pressure (WS ≥ 128 MB):
  Cluster-granularity → bw_hwmon_meas.mbps     (hardware-exact, ~4 ms)
  Per-process         → bus_access (0x0019)    (1.04× accurate, thermally stable)

WS < 128 MB + need DRAM signal:
  → bw_hwmon_meas.mbps            (will show background unless multiple cores active)
  → no reliable per-process DRAM metric available at cache-resident WS
```

---

## 8. Multi-Processor Contention Coverage

### 8.1 Per-processor measurement sources

| Processor | Contention level | Best metric                            | Unit      | Type              |
|-----------|-----------------|----------------------------------------|-----------|-------------------|
| CPU       | LLCC demand     | `l2d_cache × 64 / elapsed`             | MB/s      | PMU (per-process) |
| CPU       | DRAM actual     | `bw_hwmon_meas.mbps`                   | MB/s      | HW measured       |
| CPU       | DRAM per-proc   | `bus_access × 64 / elapsed`            | MB/s      | PMU (per-process) |
| GPU       | DRAM actual     | `kgsl_buslevel.avg_bw`                 | MB/s (*)  | HW measured (*)   |
| GPU       | DRAM contention | `kgsl_pwrstats.ram_wait / total_time`  | fraction  | HW measured       |
| NPU/CDSP  | DRAM demand     | `icc_set_bw path=qnm_nsp-ebi`         | kBps      | SW vote only      |
| Total     | DRAM aggregate  | `icc_set_bw path=llcc_mc-ebi`         | kBps      | SW vote only      |
| LLCC      | Per-client      | LLCC PMU type=11                       | —         | **Inaccessible**  |

(*) `kgsl_buslevel.avg_bw` unit is believed to be MB/s (KGSL source: `bus_ab_mbytes`)
but has not been verified against an independent measurement on this device.  There
is a minimum floor of ~534 MB/s even at near-idle (0.03% GPU busy), which reflects
hardware minimum DDR clock levels, not actual workload traffic.

### 8.2 The measurement heterogeneity problem

The three processor types are not measured by the same kind of signal:

- **CPU**: actual hardware measurement (`bw_hwmon`), accurate, ~4 ms latency
- **GPU**: likely hardware measurement (`kgsl_buslevel`), unit unverified, ~100 ms latency
- **NPU**: software demand votes (`icc_set_bw`), may lead or lag actual traffic, fires
  irregularly (only when the driver updates its vote)
- **LLCC per-client**: completely absent

A contention prediction model built from these sources will have different confidence
levels for each processor.  CPU DRAM contention can be detected accurately and quickly.
GPU DRAM contention can be approximately detected but requires unit verification.
NPU DRAM contention is estimated from demand-side votes only.

### 8.3 LLCC contention: inference only

Because LLCC PMU is inaccessible, LLCC capacity contention between processors cannot
be measured directly.  The best available inference approach:

1. Establish baseline bwmon readings for CPU-only and GPU-only workloads separately.
2. Run both simultaneously; elevated bwmon (vs sum of individual baselines) indicates
   LLCC eviction causing additional DRAM misses — an indirect contention signal.
3. `memlat_dev_meas.ratio` drops when CPU is being evicted from LLCC by GPU
   activity (more memory events per instruction as cache hit rate falls).

This is indirect and requires careful experimental design to be meaningful.

---

## 9. Thermal Effects

Oryon V2 Prime throttles from 4.47 GHz to 2.2 GHz after 7–15 s of sustained
memory-intensive load at WS ≥ 128 MB.  This drops achievable DRAM bandwidth by
~40–55% and confounds PMU-based measurements.

For reliable measurements:
- Use 5 s measurement windows with 0.5 s warm-up.  Throttling is typically not
  observed within the first 5 s at WS ≤ 128 MB.
- Monitor bwmon for a sudden drop mid-measurement as a throttle indicator.
- `bus_access` is preferred over `l2d_cache` at WS = 256 MB because it remains
  accurate under throttle (1.03× vs 2.71× for `l2d_cache`).
- For throttle-robust control, normalise by current CPU frequency rather than using
  raw events-per-second.

---

## 10. Open Questions and Future Work

| Question                                      | Required action                                      |
|-----------------------------------------------|------------------------------------------------------|
| `kgsl_buslevel.avg_bw` unit (MB/s vs kBps?)  | Run known GPU workload; compare to Snapdragon Profiler GPU DDR counter |
| NPU `qnm_nsp-ebi` votes vs actual throughput  | Run inference; simultaneously read icc_set_bw and any CDSP counter |
| LLCC contention signal (indirect)              | Run CPU+GPU simultaneously; compare bwmon to individual baselines |
| WS = 64 MB DRAM floor detection               | Longer measurement window (20+ s) to see if workload DRAM emerges |
| Oryon V2 mobile L2 cache size (per-cluster)   | Sweep WS in finer steps (2–16 MB) to find L2→LLCC transition knee |

---

## Appendix: Side Note for Future Agent — Qualcomm Snapdragon Profiler

> **If you have access to Qualcomm Snapdragon Profiler connected to this device,
> the following actions would close the most significant open gaps:**

**Priority 1 — Verify `kgsl_buslevel.avg_bw` unit (GPU DDR BW):**
- Run a GPU-intensive workload (e.g., a Vulkan compute shader allocating and reading
  a large buffer).
- Read `kgsl_buslevel.avg_bw` from the tracepoint simultaneously.
- Compare the tracepoint value against Snapdragon Profiler's GPU memory counters
  (specifically the Adreno UCHE read/write bandwidth in bytes/s).
- If `avg_bw × 1 = profiler_GB_s × 1000` then unit is MB/s; if `avg_bw / 1024 =
  profiler_GB_s` then unit is kBps.
- Document the conversion factor and update this report.

**Priority 2 — Check LLCC per-client counters:**
- In Snapdragon Profiler, navigate to the system-level or memory counter groups.
- Look for any counter group labeled "LLCC", "SLC", or "system cache" with per-client
  (CPU / GPU / NPU) breakdown.
- If found: enumerate counter IDs and determine if they are accessible via
  `perf_event_open` (try type=11 with the counter ID as config).
- If not exposed in Profiler either: confirm that LLCC per-client visibility is
  fundamentally unavailable on this platform, not just a kernel driver limitation.

**Priority 3 — Verify NPU icc_set_bw accuracy:**
- Run a known NPU inference workload with a known model (e.g., MobileNet at known
  throughput in ops/s).
- Record `icc_set_bw path=qnm_nsp-ebi` avg_bw values during inference.
- Compare against Snapdragon Profiler's CDSP/HVX bandwidth counters if available.
- If Profiler shows measured NPU DDR throughput that significantly differs from the
  icc_set_bw vote, document the ratio as a correction factor.
