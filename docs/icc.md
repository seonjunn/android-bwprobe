# ICC Bandwidth Framework — SM8750 (Snapdragon 8 Elite)

Documentation for the Linux kernel interconnect (`icc`) framework as observed on the
Samsung Galaxy S25+ (Oryon V2, Android 15, kernel 6.6), focusing on the `icc_set_bw`
tracepoint and its use in the `icc_bw` experiment.

---

## 1. What is ICC

The Linux kernel **interconnect framework** (`drivers/interconnect/`) provides a standard
API for on-chip bus bandwidth and latency negotiation. Drivers request bandwidth on
specific paths via `icc_set_bw(path, avg_bw_kBps, peak_bw_kBps)`.

On Qualcomm SoCs, the interconnect provider is **DCVS** (Dynamic Clock and Voltage Scaling).
DCVS aggregates votes from all ICC clients, selects the highest, and programs the hardware
DDR frequency controller accordingly. The vote cadence is ~40 ms on SM8750.

Key point: ICC reports **software votes**, not hardware measurements. The hardware DRAM
monitor (`bw_hwmon_meas` tracepoint) measures actual DRAM traffic at ~4 ms intervals and
is the ground truth.

---

## 2. Tracepoint Mechanics

### Event format

```
<task>-<pid> [cpu] ... interconnect:icc_set_bw:
  path=chm_apps-qns_llcc dev=soc:qcom,dcvs:llcc:sp node=qns_llcc
  avg_bw=6773792 peak_bw=... agg_avg=6850592 agg_peak=...
```

Fields (all bandwidth values in **kBps**):

| Field | Meaning |
|---|---|
| `path` | `<src_node>-<dst_node>` identifier for this ICC path |
| `dev` | DCVS device that owns this ICC provider |
| `node` | Node being updated in this event (src or dst) |
| `avg_bw` | This client's average bandwidth vote |
| `peak_bw` | This client's peak bandwidth vote |
| `agg_avg` | **Aggregate** of all clients' avg votes at this node |
| `agg_peak` | Aggregate of all clients' peak votes at this node |

### Dual-event pattern

Each `icc_set_bw()` call fires **two tracepoint events** — one for the source node and one
for the destination node of the path. To avoid double-counting:

```
Filter: path=chm_apps-qns_llcc AND node=qns_llcc  → CPU→LLCC, destination only
Filter: path=llcc_mc-ebi        AND node=ebi        → LLCC→DRAM, destination only
```

The old code used `strstr(line, "chm_apps")` which incorrectly matched:
- Both src and dst events of the same update (double-count)
- Unrelated paths like `chm_apps-qhs_i2c` (I2C peripheral traffic)

### Cadence and variance

ICC votes update at ~40 ms intervals (DCVS voting period). With a 5 s measurement window,
expect ~125 bwmon samples but only ~12 ICC events. This means ICC has ~10× higher
coefficient of variation per rep compared to bwmon.

### avg_bw vs agg_avg

- `avg_bw`: One DCVS client's vote. The CPU DCVS client submits one vote; other clients
  (GPU via KGSL, display, video codec, DSP) submit their own. If the CPU client has the
  highest vote, `avg_bw ≈ agg_avg`. If other clients dominate, `avg_bw << agg_avg`.

- `agg_avg`: Sum (or max, depending on aggregation policy) of all client votes at that
  node. This is what DCVS actually programs into hardware and what `bwmon` should reflect.
  **Use `agg_avg` at `node=ebi` for bwmon correlation.**

---

## 3. SM8750 Node Topology

From `/sys/kernel/debug/interconnect/` on SM8750 (Android 15):

| Node | Hardware Entity | ICC Role |
|---|---|---|
| `chm_apps` | CPU cluster Network-on-Chip master port | Source of CPU→LLCC ICC path |
| `qns_llcc` | LLCC QoS slave (System Level Cache) | Destination of CPU→LLCC path |
| `llcc_mc` | LLCC Memory Controller | Source of LLCC→DRAM path |
| `ebi` | External Bus Interface (LPDDR5X controller) | Destination of LLCC→DRAM path |
| `3d00000.qcom,kgsl-3d0` | Adreno GPU | ICC client but bypasses for compute (see §6) |
| `remoteproc-cdsp` | Compute DSP (HexDSP / Hexagon NPU) | ICC client for AI workloads |
| `remoteproc-adsp` | Audio DSP | ICC client for audio streaming |
| `ae00000.qcom,mdss_mdp` | Display engine (MDP) | ICC client for display reads |
| `aa00000.qcom,vidc` | Video codec (Iris) | ICC client for video decode/encode |
| `1d84000.ufshc` | UFS Host Controller (storage) | ICC client for storage I/O |

### CPU bandwidth path

```
CPU (cpu6, Prime cluster)
  ↓ cache miss
chm_apps  [ICC source: avg_bw = CPU DCVS vote]
  ↓ icc_set_bw(path=chm_apps-qns_llcc, ...)
qns_llcc  [ICC dest:   agg_avg = all clients' aggregate for LLCC demand]
  ↓ LLCC hit or miss
  → if miss:
llcc_mc   [ICC source: avg_bw = LLCC→DRAM DCVS vote]
  ↓ icc_set_bw(path=llcc_mc-ebi, ...)
ebi       [ICC dest:   agg_avg = all clients' aggregate DRAM demand]
  ↓ LPDDR5X bus
DRAM
```

Expected relationship at DRAM-saturating WS:
```
bwmon-prime ≈ icc_dram_agg / 1e3  (MB/s)
```
With some lag since ICC votes update every ~40 ms while bwmon updates every ~4 ms.

---

## 4. CPU Bandwidth Vote (chm_apps → qns_llcc)

`icc_llcc_agg_MBs` (agg_avg at qns_llcc) represents the total CPU cluster → LLCC demand
as seen by DCVS. This includes:
- Prime cluster (cpu6–7) demand
- Gold cluster (cpu0–5) demand (if any)
- Any other chm_apps clients

At large WS (DRAM regime), `icc_llcc_agg` typically exceeds `icc_dram_agg` because:
- Some LLCC reads hit in cache (don't reach DRAM)
- LLCC prefetch/eviction adds to CPU→LLCC traffic

---

## 5. DRAM Vote (llcc_mc → ebi)

`icc_dram_agg_MBs` (agg_avg at ebi) represents total DRAM demand across **all ICC clients**.
This is the closest ICC equivalent to `bwmon-prime + bwmon-gold`.

Empirically (WS=128 MB, NEON matvec):
- `bwmon` ≈ 104 GB/s (hardware measurement)
- `icc_dram_agg` ≈ 79 GB/s (DCVS software vote, ~40 ms lag)
- Ratio ≈ 0.76× — DCVS smoothing and update lag explain the gap

For pointer-chase (lower BW, longer window):
- Ratio improves to ~0.8–0.9× as the ICC cadence lag is less significant

---

## 6. Why GPU KGSL Is Excluded

KGSL (Kernel Graphics Support Layer) does **not** use the Linux ICC framework for GPU
compute traffic. Instead, the Adreno GPU uses the **GMU** (Graphics Management Unit), a
dedicated hardware power management engine that directly programs DDR frequency through
a separate path.

GPU DRAM bandwidth appears in `bwmon-llcc-gold` (when GPU traffic routes through the Gold
cluster NOC) but **never** in `icc_set_bw` events during GPU compute workloads.

To measure GPU DRAM demand:
```bash
# GPU bus level from GMU hardware DDR counters
adb shell cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage
# Or via kgsl tracepoints: kgsl_buslevel.avg_bw
```

---

## 7. Experiment Results

### WS Sweep (per-workload mean, MB/s)

Run with `make NDK=... ADB="adb -s 192.168.0.190:33291" runicc`.

| WS (MB) | Workload | bwmon | icc_dram_agg | bw_alg | bus_access | Regime |
|---:|---|---:|---:|---:|---:|---|
| 8 | chase | ~25 | — | — | — | cache |
| 8 | scalar | ~25 | — | — | — | cache |
| 8 | neon | ~25 | — | — | — | cache |
| 128 | chase | ~10,600 | ~8,500 | ~10,600 | ~11,000 | DRAM |
| 128 | scalar | ~11,400 | ~8,500 | ~5,600 | ~11,700 | DRAM |
| 128 | neon | ~104,000 | ~79,000 | ~54,000 | ~113,000 | DRAM |
| 256 | neon | ~80,000 | ~65,000 | ~41,000 | ~90,000 | DRAM |

*(Values from previous single-WS experiment; WS sweep results to be updated after run.)*

### Key Ratios at WS=128 MB

| Workload | bwmon/alg | icc_dram_agg/bwmon | bus/bwmon |
|---|---:|---:|---:|
| chase | 1.0× | ~0.80× | ~1.03× |
| scalar | 1.89× | ~0.75× | ~1.03× |
| NEON | 1.93× | ~0.76× | ~1.09× |

- `bus_access` tracks bwmon at 1.03–1.09× (accurate PMU)
- `icc_dram_agg` tracks at ~0.75–0.80× (DCVS smoothing, 40 ms lag)
- The amplification ratio (bwmon/alg ≈ 1.9×) is consistent: hardware stream prefetcher

---

## 8. Reading the Trace Data

### File schemas

**`icc_bw_trace_bwmon.csv`**
```
t_s,bwmon_MBs
0.123456,10421.0
0.127891,10388.0
...
```
One row per `bw_hwmon_meas` event for `bwmon-llcc-prime`. Cadence ~4 ms.

**`icc_bw_trace_icc.csv`**
```
t_s,node,avg_kBps,agg_kBps
0.041234,qns_llcc,6773792,6850592
0.041290,ebi,5442816,5516288
...
```
One row per filtered `icc_set_bw` event (destination node only). Cadence ~40 ms.

**`icc_bw_trace_bus.csv`**
```
t_s,bus_access_MBs
0.004001,10892.3
0.008003,11204.7
...
```
One row per 4 ms poll of the `bus_access` (0x0019) PMU counter.

**`icc_bw_context.csv`**
```
t_s,event,workload,ws_mb,rep,bw_alg_MBs
2.001234,start,chase,128,1,-1.0
7.012345,end,chase,128,1,10618.4
...
```
Rep start/end markers for aligning timeseries to workload phases.

### Pandas merge recipe

```python
import pandas as pd

bwmon = pd.read_csv("data/icc_bw_trace_bwmon.csv")
icc   = pd.read_csv("data/icc_bw_trace_icc.csv")
bus   = pd.read_csv("data/icc_bw_trace_bus.csv")
ctx   = pd.read_csv("data/icc_bw_context.csv")

# Filter ICC to DRAM node only
icc_dram = icc[icc.node == "ebi"].copy()
icc_dram = icc_dram.rename(columns={"agg_kBps": "icc_dram_agg_kBps"})

# Merge-asof: align bwmon and ICC on nearest timestamp
bwmon_sorted = bwmon.sort_values("t_s")
icc_sorted   = icc_dram.sort_values("t_s")
merged = pd.merge_asof(bwmon_sorted, icc_sorted[["t_s","icc_dram_agg_kBps"]],
                       on="t_s", direction="nearest", tolerance=0.1)
merged["icc_dram_agg_MBs"] = merged["icc_dram_agg_kBps"] / 1e3

# Annotate with rep context
starts = ctx[ctx.event == "start"][["t_s","workload","ws_mb","rep"]].rename(
    columns={"t_s": "t_start"})
ends = ctx[ctx.event == "end"][["t_s","workload","ws_mb","rep"]].rename(
    columns={"t_s": "t_end"})
reps = starts.merge(ends, on=["workload","ws_mb","rep"])
# Tag each bwmon sample with its workload/rep
```
