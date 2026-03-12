# ARM PMU and bus_access — SM8750 (Snapdragon 8 Elite)

The ARMv8 Performance Monitor Unit (PMU) and the `bus_access` event (0x0019)
as used on the Samsung Galaxy S25+ (Oryon V2, Android 15, kernel 6.6).

---

## Part I — Theoretical Model

### 1.1 ARMv8 PMU Architecture

Every ARMv8 processor core has a private Performance Monitor Unit defined by
the ARMv8-A architecture (Chapter D7, "The Performance Monitors Extension").
It provides a fixed cycle counter (`PMCCNTR_EL0`) and a set of configurable
event counters (`PMEVCNTR<n>_EL0`). Each event counter is programmed with a
16-bit event type via `PMEVTYPER<n>_EL0`.

The PMU operates in one of two modes per counter:
- **Counting mode** (default): the counter increments each time the selected
  event fires. Software reads the counter via `read(perf_fd)` or directly via
  `MRS PMEVCNTR<n>_EL0`.
- **Sampling mode**: the counter generates an interrupt at overflow, used for
  profiling.

For bandwidth measurement this project uses counting mode with delta reads over
a fixed wall-clock window.

The ARM specification distinguishes three event attribution classes:
- **Attributable events**: attributed to the PE context (EL0/EL1) in which the
  microarchitectural activity originates. Whether a cache refill is attributed
  to the requesting user-space context (EL0) or the kernel handler context (EL1)
  is **implementation-defined**.
- **Non-attributable events**: count regardless of PE context.

`exclude_kernel=1` in `perf_event_attr` suppresses counting while the PMU
context register indicates EL1/EL2. Its effect on any given attributable event
therefore depends on which context the implementation attributes it to.

### 1.2 bus_access (Event 0x0019)

Defined by ARMv8 architecture as:

> **BUS_ACCESS** (0x0019): Attributable Memory-read or Memory-write operation
> that causes a transfer on the outer bus of the PE.

The "outer bus" is the data bus connecting the last private cache level of the
PE to the next level in the memory hierarchy. On a processor with L1D and L2,
this is typically the L2 output port.

Theoretical properties:
- Fires once per **cache-line transfer** (not per byte, not per instruction).
- Counts both **reads** (demand fetches, hardware prefetcher) and **writes**
  (write-backs, write-allocates).
- Is an **attributable** event — attribution to EL0 vs. EL1 context is
  implementation-defined.
- Is **per-PE** (per-core) in hardware; `perf_event_open` with `pid=0, cpu=-1`
  restricts counting to the calling process across all CPUs it runs on.

Expected behavior with `exclude_kernel=1` (per spec): counts only demand
accesses initiated in user space. Hardware prefetcher events and write-backs
may or may not be counted depending on implementation.

### 1.3 perf_event_open and the Linux PMU Abstraction

Linux exposes PMU counters via the `perf_event_open(2)` syscall. On Android,
PMU access is gated by `/proc/sys/kernel/perf_event_paranoid`; `-1` allows
all events without restriction.

Key `perf_event_attr` fields:

| Field | Meaning |
|---|---|
| `type` | PMU type identifier (device-specific, from sysfs) |
| `config` | Event number (raw event code for the target PMU) |
| `exclude_kernel` | 1 = suppress counting during EL1 execution |
| `exclude_hv` | 1 = suppress counting during EL2 (hypervisor) |
| `disabled` | 1 = counter starts stopped; enable with `PERF_EVENT_IOC_ENABLE` |

`pid=0, cpu=-1` tracks the calling process on any CPU. `pid=0, cpu=N` tracks
the calling process on a specific CPU. `pid=-1, cpu=N` tracks all processes
on CPU N (requires elevated privileges).

The PMU type for ARMv8 PMUv3 is found at:
```
/sys/bus/event_source/devices/armv8_pmuv3/type
```

It is **not** `PERF_TYPE_RAW` (4). On SM8750 (Android 15) it is 10.

### 1.4 Counter Width and Overflow

ARMv8 PMU event counters are 32-bit in hardware, but the Linux kernel wraps
them into 64-bit software counters that handle overflow transparently. Reads
via `read(perf_fd)` return 64-bit values. Overflow is not a concern for
sub-minute benchmark durations.

### 1.5 Memory-Level Parallelism and Hardware Prefetching

These two microarchitectural concepts are central to understanding why different
workloads produce different `bus_access` rates, and why pointer-chase is the
canonical memory-hierarchy probe.

#### Memory-Level Parallelism (MLP)

When a CPU encounters a cache miss, the memory subsystem takes time to serve it
(~1 ns for L2, ~2.7 ns for LLCC, ~5.5 ns for DRAM on this device). Modern
out-of-order CPUs do not stall during this time: they scan ahead in the
instruction stream and issue other independent memory operations in parallel.
The number of cache misses simultaneously in-flight is called **memory-level
parallelism (MLP)**.

```
MLP = 1  (serial):  miss₁ → wait → miss₂ → wait → ...
          throughput = 1 / latency_per_miss

MLP = N  (parallel):  miss₁ ─┐
                      miss₂  ├─ all in flight together
                      ...    ─┘
          throughput ≈ N / latency_per_miss
```

High MLP is what enables NEON matvec to reach ~100 GB/s despite DRAM latency
being 5.5 ns. The CPU issues many independent loads in parallel, keeping
hundreds of misses in flight at once via out-of-order execution and SIMD
instruction-level parallelism.

#### Hardware Prefetching

The CPU contains a **stream prefetcher** — a hardware circuit that monitors
recent memory access addresses and speculatively issues fetch requests for data
it predicts will be needed soon. For a sequential access pattern (`A[0]`,
`A[1]`, `A[2]`...), the prefetcher recognizes the stride and prefetches `A[16]`,
`A[32]`... before the CPU instruction even reaches them. The data arrives in
LLCC or L2 by the time it is demanded, hiding DRAM latency entirely.

The prefetcher raises effective MLP automatically for any regular access pattern
(sequential, fixed-stride). It is responsible for the ~1.9× bandwidth
amplification seen in streaming workloads: `bwmon / bw_alg ≈ 1.9×` because the
prefetcher issues extra fetches beyond the demand footprint.

**The prefetcher cannot help with pointer-chase.** Each hop reads an address
stored inside the current cache line:

```c
next_addr = *(uintptr_t *)current_addr;   // address of next hop is data of current
```

The prefetcher cannot predict `next_addr` without first reading `current_addr`.
So it issues zero prefetches. Every hop waits the full memory latency before the
next address is known. MLP = 1 by construction.

#### Why MLP = 1 and No Prefetching Matter for Hierarchy Profiling

A benchmark with high MLP or active prefetching conflates throughput and
latency: you measure how fast misses can be *pipelined*, not how long a single
miss takes. Pointer-chase with MLP = 1 guarantees:

- Each hop reports the **true latency** of whichever cache level serves it.
- The throughput of the loop directly equals `1 / latency_per_hop`.
- Changing WS moves the working set from one cache level to the next, producing
  a clean step change in measured latency.

This is the reason pointer-chase is used in `llcc_size.c` and `bwprobe.c` to
map the L2/LLCC/DRAM boundaries: latency jumps from ~0.7 ns to ~2.7 ns to
~5.5 ns as WS crosses 12 MB and 112 MB respectively. No streaming benchmark
can reveal these boundaries, because the prefetcher hides the latency and the
LLCC becomes transparent regardless of WS (see `docs/hwmon.md` §3.2).

---

## Part II — Implementation

### 2.1 Platform Setup (src/cpu.h)

The Android NDK `libc` headers do not expose `perf_event_open`. The project
declares its own syscall wrapper:

```c
#define ARMV8_PMU_TYPE   10      /* /sys/bus/event_source/devices/armv8_pmuv3/type */
#define CACHE_LINE       64UL

static inline long perf_event_open(struct perf_event_attr *attr,
                                   pid_t pid, int cpu,
                                   int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}
```

CPU pinning (`pin_to_cpu(6)`) is applied to the workload thread before opening
the PMU fd, so `cpu=-1` effectively tracks a single known core.

### 2.2 Opening bus_access (src/pmu.h)

```c
static inline int open_evt_raw(uint32_t type, uint64_t config)
{
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.type           = type;
    a.config         = config;
    a.size           = sizeof(a);
    a.disabled       = 1;
    a.exclude_kernel = 0;   /* MANDATORY on Oryon V2 — see §3.1 */
    a.exclude_hv     = 1;
    return (int)perf_event_open(&a, 0, -1, -1, 0);
}

static inline int open_bus_access(void)
{
    return open_evt_raw(ARMV8_PMU_TYPE, 0x0019);
}
```

### 2.3 Polling Thread (src/buspoll.h)

`buspoll_thread` polls the counter every 4 ms — matching the bwmon hardware
cadence — and writes instantaneous bandwidth to a CSV trace:

```
bandwidth_MBs = (curr − prev) × CACHE_LINE / elapsed_s / 1e6
```

The thread pins to the same CPU as the workload to avoid cache effects from
migration. It enables the counter with `PERF_EVENT_IOC_ENABLE` (not at open
time, since `disabled=1`), then loops until `BusPoll.running = 0`.

Trace output (`t_s,bus_access_MBs` rows) is written by the poll thread alone;
no lock is needed on the file handle.

### 2.4 Event Counter Lifecycle

```
open_bus_access()           → fd, counter stopped (disabled=1)
PERF_EVENT_IOC_ENABLE       → counter starts incrementing
read(fd, &val, 8)           → current cumulative count (64-bit)
PERF_EVENT_IOC_RESET        → reset to 0 (not used in buspoll; delta is taken)
PERF_EVENT_IOC_DISABLE      → counter stops
close(fd)                   → counter destroyed
```

---

## Part III — Experimental Findings

### 3.1 exclude_kernel = 0 Is Mandatory on Oryon V2

**Setting `exclude_kernel=1` causes 23–33× undercounting of `bus_access`.**

Expected behavior per spec: `exclude_kernel=1` suppresses counting during EL1
execution. Because `bus_access` is an attributable event, it should still count
user-space-initiated cache misses even if the refill itself is handled in EL1.

Observed behavior on Oryon V2: cache-line refills are **attributed to EL1**
(kernel context). When `exclude_kernel=1`, the PMU does not count the event
because at the cycle the L2 transfer completes, the privilege level register
shows EL1. The result is that 96–97% of `bus_access` events are suppressed.

This is an implementation choice permitted by the ARM specification ("attribution
is implementation-defined"). The fix — `exclude_kernel=0` — is enforced
unconditionally in `open_evt_raw()`.

### 3.2 Other PMU Events on Oryon V2

| Event | Code | Observed Status |
|---|---|---|
| `bus_access` | 0x0019 | Accurate (with exclude_kernel=0) |
| `l2d_cache` | 0x0016 | Fires on every L1D miss; no destination info |
| `l2d_cache_refill` | 0x0017 | Broken — flat ~18K/s regardless of WS |
| `l2d_cache_lmiss_rd` | 0x4009 | Broken — ~1 MB/s constant |
| `l3d_cache_allocate` | 0x0029 | Broken — always 0 |
| LLCC PMU | type=11 | Inaccessible — ENOENT on Android 15 |

`l2d_cache` fires but cannot distinguish whether the miss was served from L2,
LLCC, or DRAM. `bus_access` is the only event that reliably captures L2 misses
with good accuracy.

### 3.3 What bus_access Actually Counts on Oryon V2

`bus_access` on this platform counts **all cache-line transfers across the L2
output port**, including:
- Demand reads (user-space loads that miss L1D and L2).
- Hardware stream prefetcher requests.
- Write-backs from L2 to LLCC (dirty evictions).
- Write-allocates (read-for-ownership on a store miss).

It does **not** count L1D hits or L2 hits — only transfers that cross the L2
boundary toward the shared subsystem.

### 3.4 Regime Behavior

The event fires only when the working set exceeds L2 capacity (~12 MB for the
Prime cluster). Whether it indicates LLCC pressure vs. DRAM pressure depends
on the access pattern, not just WS size.

| WS | Pattern | bus_access | Notes |
|---|---|---|---|
| ≤ 12 MB | any | ≈ 0 | L2-resident; no shared-subsystem traffic |
| 12–112 MB | random | fires (low rate) | LLCC absorbs ~70% of misses |
| 12–112 MB | streaming | fires (high rate) | Streaming bypasses LLCC; all reach DRAM |
| > 112 MB | any | fires (high rate) | All misses reach DRAM |

This makes `bus_access` sensitive to **any** shared-subsystem pressure —
including LLCC-resident workloads that are invisible to bwmon. See
`bandwidth_measurement.md` §3 for the full regime comparison.

### 3.5 Accuracy vs. bwmon (Ground Truth)

At WS=128 MB (DRAM-bound), read-only workloads:
```
bus_access / bwmon = 1.034 ± 0.008   (icc_bw experiment, n=31 reps)
```

The ~3% excess over bwmon is attributed to write-back traffic (L2→LLCC dirty
evictions) that `bus_access` counts but bwmon (read-only) does not.

For AXPY (2 reads + 1 write per element):
```
bus_access / bwmon = 1.56   (≈ 3/2: bus counts R+W; bwmon counts R only)
```

### 3.6 Per-Process Isolation

At LLCC-resident WS (e.g., chase WS=64 MB):
- `bus_access` = ~85 MB/s (workload L2 misses to LLCC only).
- `bwmon` = ~315 MB/s (~230 MB/s background from other processes on the cluster).

`bus_access` isolates workload pressure from background at moderate bandwidth
levels. bwmon cannot make this distinction — it is cluster-level.

### 3.7 Spatial Burst Amplification

For read-only streaming workloads at WS=128 MB:
```
bwmon      / bw_alg ≈ 1.88×   (scalar_matvec, neon_matvec)
bus_access / bw_alg ≈ 2.0×    (same workloads)
bus_access / bwmon  ≈ 1.06×   (same workloads — write-back overhead only)
```

**Root cause: LPDDR5X minimum burst size combined with L2 thrashing.**

LPDDR5X specifies a minimum burst length of BL16. With the 64-bit bus on SM8750:

```
BL16 × 8 bytes = 128 bytes per DRAM access (minimum)
```

Every L2 miss that reaches DRAM returns 128 bytes — the demanded 64B line plus
the spatially adjacent 64B line. This is a hardware constraint, not a software
choice. For a streaming workload with WS=128 MB and L2=12 MB:

```
WS / L2 ≈ 10.7×  →  L2 continuously thrashes
```

The adjacent 64B line from the 128B burst fills into L2 but is evicted before
the CPU demands it, because the streaming pattern fills L2 at 10.7× its
capacity. Every 64B of algorithmic demand therefore generates a 128B DRAM
transfer — 64B used, 64B wasted.

```
bwmon / bw_alg ≈ 128B / 64B = 2.0×  (ideal, zero reuse)
               = 1.88×               (empirical — occasional spatial reuse
                                       occurs before L2 eviction)
```

**Why bus_access/bw_alg ≈ 2.0× (two events per burst):**

`BUS_ACCESS` fires once per 64B transaction crossing the L2 boundary (the
coherency granule is one cache line). When the 128B DRAM burst deposits two
64B lines at the L2 input port, two `BUS_ACCESS` events fire:

```
bus_access = 2 events × 64B = 128B per burst ≈ bwmon_bytes

bus_access / bwmon  ≈ (2×64B) / 128B = 1.0  + write-backs ≈ 1.06
bus_access / bw_alg = (bus_access / bwmon) × (bwmon / bw_alg) = 1.06 × 1.88 ≈ 2.0×
```

The 6% excess of `bus_access` over bwmon comes from dirty write-backs (the
y-vector and stack dirty lines evicted from L2 to LLCC), which `bus_access`
counts as write transactions but bwmon (reads only) does not.

**Why the stream prefetcher does not explain the amplification:**

A software or hardware prefetcher that shifts demand-fetches earlier in time
does not amplify traffic — the same N lines are fetched, just sooner. The
amplification here comes from DRAM burst granularity (128B) exceeding the cache
line size (64B), with L2 thrashing preventing reuse of the extra bytes. The
prefetcher's role is to hide latency (keeping the demand pipeline full), not to
increase total byte count.

**AXPY (2 reads + 1 write per element):**

bwmon sees only reads (x and y_read), each amplified by the 1.88× burst factor:

```
bwmon / bw_alg = 1.88 × (2 read streams / 3 total streams) = 1.25×
```

bus_access includes all L2 boundary traffic (reads + write-backs of dirty y):

```
bus_access / bwmon = 1.56   (≈ 3/2: 3 traffic streams vs. 2 read streams)
```

### 3.8 Trace File Schema

Written by `buspoll_thread` to `BusPoll.trace_fp`:

```
t_s,bus_access_MBs
0.004001,10892.3
0.008003,11204.7
```

- `t_s`: Wall-clock seconds since `BusPoll.t0` (`CLOCK_MONOTONIC`).
- `bus_access_MBs`: Instantaneous bandwidth over the last 4 ms interval.

One row per poll; ~250 rows per second. No locking needed (sole writer).

---

## References

- **ARM Architecture Reference Manual for Armv8-A, Chapter D7** — PMU event
  definitions including BUS_ACCESS (0x0019): "Attributable Memory-read or
  Memory-write operation that causes a transfer on the outer bus of the PE."
  Attribution to EL0 vs. EL1 is implementation-defined.

- **JEDEC LPDDR5/5X specification (JESD209-5C)**:
  <https://www.jedec.org/sites/default/files/docs/JESD209-5C.pdf>
  BL16 burst length; 16-bit device width; 128B minimum burst on a 64-bit
  channel (4 devices × 16 bits × 16 transfers = 1024 bits = 128 bytes).

- **LPDDR5 physical structure (systemverilog.io)**:
  <https://www.systemverilog.io/design/lpddr5-tutorial-physical-structure/>
  Accessible BL16 walkthrough with diagrams.

- **Qualcomm SM8750-3-AB Product Brief** — DRAM peak bandwidth 85.33 GB/s:
  <https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/Snapdragon-8-Elite-SM8750-3-AB-Product-Brief.pdf>

- **PhoneDB SM8750-AC detailed specs** (corroborates 85.33 GB/s):
  <https://phonedb.net/index.php?m=processor&id=1027&c=qualcomm_snapdragon_8_elite_sm8750-ac___sun&d=detailed_specs>
