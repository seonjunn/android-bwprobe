"""
bw_bench_analysis.py — Analysis of controlled bandwidth benchmark results.

Sections:
  1. Experimental control: thermal gating effectiveness, warmup convergence
  2. LLCC-resident regime: bus_access captures shared-subsystem pressure (bwmon blind)
  3. Metric accuracy vs bwmon (DRAM regime, WS≥128 MB)
  4. ICC undercounting: linear model, theory, per-workload fit
  5. ICC LLCC + DRAM additivity: why their sum does NOT equal bw_alg
  6. WS regime transitions: workload characterisation, bus_access universality
  7. Summary table (WS=128 MB, DRAM regime)

Usage: python3 analysis/bw_bench_analysis.py [--data data/] [--prefix bw_bench]
"""

import argparse
import os
import sys
import numpy as np
import pandas as pd
from scipy import stats

ap = argparse.ArgumentParser()
ap.add_argument("--data",   default="data/")
ap.add_argument("--prefix", default="bw_bench")
args = ap.parse_args()

# --------------------------------------------------------------------------
# Load data
# --------------------------------------------------------------------------
summary_path = os.path.join(args.data, f"{args.prefix}_summary.csv")
df = pd.read_csv(summary_path)

WORKLOADS  = ["chase", "scalar_matvec", "neon_matvec", "neon_axpy"]
WS_LIST    = sorted(df.ws_mb.unique())
DRAM_WS    = [ws for ws in WS_LIST if ws >= 128]   # clearly DRAM-regime

# Classify each rep as DRAM-regime using bwmon threshold
df["is_dram"] = (
    ((df.workload == "chase")        & (df.bwmon_MBs >  2000)) |
    ((df.workload == "scalar_matvec")& (df.bwmon_MBs >  5000)) |
    ((df.workload == "neon_matvec")  & (df.bwmon_MBs > 30000)) |
    ((df.workload == "neon_axpy")    & (df.bwmon_MBs > 30000))
)

print("=" * 70)
print("§1  Experimental Control")
print("=" * 70)

print("\n--- Thermal distribution (skin_c at measurement start) ---")
tbl = df.groupby(["workload", "ws_mb"])["skin_c"].agg(
    mean=lambda x: round(x.mean(), 1),
    max=lambda x: round(x.max(), 1)
).unstack("ws_mb")
print(tbl)

print("\n--- Warmup passes: mean and max across all reps ---")
for wl in WORKLOADS:
    sub = df[df.workload == wl]
    mu  = sub.warmup_passes.mean()
    mx  = sub.warmup_passes.max()
    pct8 = 100 * (sub.warmup_passes == 8).mean()
    print(f"  {wl:<18} mean={mu:.1f}  max={mx}  hit_cap={pct8:.0f}%")

print("\n--- bwmon CV (coefficient of variation) in DRAM regime ---")
print(f"  {'workload':<18}  {'ws_mb':>6}  {'n':>4}  {'mean':>8}  {'cv%':>6}")
for wl in WORKLOADS:
    sub_dram = df[(df.workload == wl) & df.is_dram]
    for ws in DRAM_WS:
        sub = sub_dram[sub_dram.ws_mb == ws]
        if len(sub) < 2:
            continue
        m  = sub.bwmon_MBs.mean()
        cv = 100 * sub.bwmon_MBs.std() / m if m > 0 else np.nan
        print(f"  {wl:<18}  {ws:>6}  {len(sub):>4}  {m:>8.0f}  {cv:>5.1f}%")

# --------------------------------------------------------------------------
print()
print("=" * 70)
print("§2  bus_access vs bwmon: Regime Sensitivity and Measurement Scope")
print("=" * 70)

print("""
Goal: shared-subsystem contention (LLCC + DRAM), not DRAM alone.
All processors share both LLCC and DRAM. bus_access fires at WS > L2 (12 MB) —
the moment traffic enters the shared subsystem — regardless of whether it hits
in LLCC or goes to DRAM. bwmon only fires when data misses the LLCC entirely.

Two regimes illustrate the gap:

  L2-resident (WS ≤ 12 MB): no shared-subsystem pressure; both metrics ≈ 0.
  LLCC-regime (WS > 12 MB):  bus_access fires; bwmon depends on access pattern:
    - Random access (chase): good LLCC hit rate → bwmon ≈ background noise only
    - Streaming (matvec, axpy): no temporal reuse → LLCC can't help → bwmon fires too

In both LLCC-regime cases bus_access captures the shared-subsystem pressure.
bwmon misses the random-access case entirely.
""")

ALL_WS = sorted(WS_LIST)

print("  bus_access (MB/s) — fires at WS > 12 MB regardless of access pattern:")
print(f"  {'workload':<18}", end="")
for ws in ALL_WS:
    print(f"  {ws:>5}MB", end="")
print()
for wl in WORKLOADS:
    print(f"  {wl:<18}", end="")
    for ws in ALL_WS:
        sub = df[(df.workload == wl) & (df.ws_mb == ws)]
        v = sub.bus_access_MBs.mean() if not sub.empty else float('nan')
        print(f"  {v:>7.0f}", end="")
    print()

print(f"\n  bwmon (MB/s) — only fires when data misses LLCC (DRAM-regime):")
print(f"  {'workload':<18}", end="")
for ws in ALL_WS:
    print(f"  {ws:>5}MB", end="")
print()
for wl in WORKLOADS:
    print(f"  {wl:<18}", end="")
    for ws in ALL_WS:
        sub = df[(df.workload == wl) & (df.ws_mb == ws)]
        v = sub.bwmon_MBs.mean() if not sub.empty else float('nan')
        print(f"  {v:>7.0f}", end="")
    print()

print(f"\n  bus_access / bwmon ratio — >1 means bus_access sees traffic bwmon misses:")
print(f"  {'workload':<18}", end="")
for ws in ALL_WS:
    print(f"  {ws:>5}MB", end="")
print()
for wl in WORKLOADS:
    print(f"  {wl:<18}", end="")
    for ws in ALL_WS:
        sub = df[(df.workload == wl) & (df.ws_mb == ws)]
        if sub.empty:
            print(f"  {'—':>7}", end="")
            continue
        b = sub.bus_access_MBs.mean()
        bw = sub.bwmon_MBs.mean()
        if bw > 200 and b > 0:
            print(f"  {b/bw:>7.2f}", end="")
        else:
            print(f"  {'—':>7}", end="")
    print()

print("""
Interpretation:
  WS=4 MB  : bus_access ≈ 0 for all workloads — L2-resident, no shared pressure.
  WS=16 MB : bus_access fires for all workloads. bwmon also fires for streaming
             (neon workloads: no LLCC reuse benefit despite WS < LLCC capacity).
             chase: bus_access is low (LLCC absorbs most misses), bwmon ≈ background.
  WS=128 MB: both fire — bus_access / bwmon = 1.0–1.6× depending on write traffic.

  For streaming workloads, the "LLCC-resident" regime (bwmon≈0) does not exist in
  practice: sequential access patterns have no temporal reuse, so LLCC provides no
  buffering and data goes straight to DRAM even at small WS.

  bus_access / bwmon > 1 even in the DRAM regime because:
    (a) bus_access includes write traffic; bwmon counts reads only
    (b) at intermediate WS, LLCC absorbs some L2 misses before they reach DRAM
        → bus_access sees more traffic than bwmon

  chase WS=64 MB: bus_access/bwmon = 0.27 (ratio < 1) — not a contradiction.
    bwmon is CLUSTER-LEVEL (all traffic from cpu6-7, incl. background processes).
    bus_access is PER-PROCESS (only this process's L2 misses).
    bus_access=85 MB/s = chase workload is nearly LLCC-resident.
    bwmon=315 MB/s = ~230 MB/s background system traffic dominates.
    This is exactly the LLCC-resident case: the workload itself causes little DRAM
    traffic, which bwmon can't isolate from background noise — bus_access can.
""")

# --------------------------------------------------------------------------
print()
print("=" * 70)
print("§3  Metric Accuracy vs bwmon (DRAM regime, WS≥128 MB)")
print("=" * 70)

dram = df[df.is_dram & (df.ws_mb >= 128)].copy()

print(f"\n  n_dram_reps = {len(dram)}")
print(f"\n  {'metric':<22}  {'ratio_mean':>10}  {'ratio_std':>10}  "
      f"{'t-stat':>8}  {'p-val':>8}  {'r_pearson':>10}")

for col, label in [
    ("bus_access_MBs",   "bus_access / bwmon"),
    ("icc_dram_agg_MBs", "icc_dram / bwmon"),
    ("icc_llcc_agg_MBs", "icc_llcc / bwmon"),
]:
    valid = dram[(dram[col] > 0) & (dram.bwmon_MBs > 0)].copy()
    ratio = valid[col] / valid.bwmon_MBs
    t, p  = stats.ttest_1samp(ratio, 1.0)
    r, _  = stats.pearsonr(valid.bwmon_MBs, valid[col])
    print(f"  {label:<22}  {ratio.mean():>10.4f}  {ratio.std():>10.4f}  "
          f"{t:>8.2f}  {p:>8.4f}  {r:>10.4f}")

# --------------------------------------------------------------------------
print()
print("=" * 70)
print("§4  ICC Undercounting: Linear Model & Theory")
print("=" * 70)

print("""
Theory: DCVS issues ICC votes every ~40 ms. The vote reflects the measured
bandwidth in the PREVIOUS window (exponential moving average). When actual
bandwidth changes rapidly (high-BW streaming), the vote lags the hardware
measurement. Additionally, DCVS applies a safety margin (α < 1).

Model: icc_dram_agg = α × bwmon + β
  α < 1  : DCVS conservatism / exponential smoothing lag
  β > 0  : background floor from non-CPU ICC clients (display, audio, DSP)

Crossover at bwmon* = β / (1 − α):
  below bwmon* : icc_dram > bwmon  (background floor dominates)
  above bwmon* : icc_dram < bwmon  (α-term makes votes smaller than measured)
""")

# OLS per workload
valid_dram = dram[(dram.icc_dram_agg_MBs > 0) & (dram.bwmon_MBs > 0)].copy()

# Global OLS
bwm = valid_dram.bwmon_MBs.values
icc = valid_dram.icc_dram_agg_MBs.values
sl, ic, r2r, p_sl, se_sl = stats.linregress(bwm, icc)
r2 = r2r ** 2
crossover = ic / (1 - sl) if sl < 1 else np.inf
print(f"  Global OLS (all workloads, WS≥128 MB, DRAM reps):")
print(f"    icc_dram = {sl:.4f} × bwmon + {ic:.0f}  (R²={r2:.4f})")
print(f"    Crossover bwmon* = {crossover:.0f} MB/s  (icc overcounts below this)")

print(f"\n  Per-workload OLS:")
print(f"  {'workload':<18}  {'α (slope)':>10}  {'β (intercept)':>14}  {'R²':>6}  "
      f"{'bwmon*':>10}")
for wl in WORKLOADS:
    sub = valid_dram[valid_dram.workload == wl]
    if len(sub) < 3:
        continue
    sl2, ic2, r_, _, _ = stats.linregress(sub.bwmon_MBs, sub.icc_dram_agg_MBs)
    r2_ = r_ ** 2
    cross = ic2 / (1 - sl2) if sl2 < 1 else np.inf
    print(f"  {wl:<18}  {sl2:>10.4f}  {ic2:>14.0f}  {r2_:>6.4f}  {cross:>10.0f}")

print("""
Theoretical explanation of when undercounting is worst:
  The undercounting FRACTION = (bwmon - icc_dram) / bwmon
                             = (1 - α) - β / bwmon

  As bwmon → ∞ : fraction → (1 - α)  [maximum undercounting = DCVS conservatism]
  As bwmon → 0 : fraction → −∞       [icc overcounts; background floor dominates]

  Undercounting is WORST at HIGH bandwidth (neon_matvec, neon_axpy at large WS).
  At moderate BW (scalar_matvec, chase) the β floor partially cancels α deficit.
""")

# Compute undercounting fraction per workload at WS=128
print("  Observed undercounting fraction (bwmon - icc_dram) / bwmon at WS=128 MB:")
for wl in WORKLOADS:
    sub = df[(df.workload == wl) & (df.ws_mb == 128) & (df.icc_dram_agg_MBs > 0)]
    if sub.empty:
        continue
    frac = ((sub.bwmon_MBs - sub.icc_dram_agg_MBs) / sub.bwmon_MBs).mean()
    bwmon_m = sub.bwmon_MBs.mean()
    print(f"  {wl:<18}  bwmon={bwmon_m:6.0f} MB/s  undercount={100*frac:+.1f}%")

# --------------------------------------------------------------------------
print()
print("=" * 70)
print("§5  icc_llcc + icc_dram vs bw_alg")
print("=" * 70)

print("""
Question: Does icc_llcc_agg + icc_dram_agg equal the calculated bandwidth?

Answer: NO — and it cannot, by definition.

Explanation of the node topology:
  CPU → [chm_apps] → qns_llcc (LLCC input) → [llcc_mc] → ebi (DRAM)

  icc_llcc_agg = total traffic ENTERING the LLCC fabric (from all CPU clients)
  icc_dram_agg = total traffic EXITING the LLCC to DRAM (cache misses only)

  These are SEQUENTIAL HOPS, not parallel paths:
  - Every DRAM access first passes through LLCC (counted in icc_llcc)
  - icc_dram ⊆ icc_llcc (DRAM traffic is a SUBSET of LLCC traffic)
  - Adding them DOUBLE-COUNTS the portion that reaches DRAM

  Correct relationship:
    icc_dram_agg  ≈  (1 − LLCC_hit_rate) × icc_llcc_agg
    or equivalently:
    icc_llcc_agg  ≈  icc_dram_agg + LLCC_absorbed_traffic

  Why icc_llcc > bw_alg:
    HW stream prefetcher fetches ahead → bwmon > bw_alg
    DCVS votes for icc_llcc_agg include prefetch demand → icc_llcc > bw_alg

  LLCC absorption = (icc_llcc - icc_dram) / icc_llcc:
    Measures what fraction of CPU→LLCC traffic was served from cache
""")

print("  Observed ratios at WS=128 MB (DRAM regime):")
sub128 = df[(df.ws_mb == 128) &
            (df.icc_llcc_agg_MBs > 0) & (df.icc_dram_agg_MBs > 0) & (df.bw_alg_MBs > 0)]

print(f"  {'workload':<18}  "
      f"{'sum/bw_alg':>10}  {'sum/bwmon':>10}  "
      f"{'llcc_abs%':>10}  {'icc_d/bwm':>10}")
for wl in WORKLOADS:
    sub = sub128[sub128.workload == wl]
    if sub.empty:
        continue
    llcc  = sub.icc_llcc_agg_MBs.mean()
    dram  = sub.icc_dram_agg_MBs.mean()
    bwalg = sub.bw_alg_MBs.mean()
    bwm   = sub.bwmon_MBs.mean()
    s     = llcc + dram
    abs_  = 100 * (llcc - dram) / llcc if llcc > 0 else np.nan
    print(f"  {wl:<18}  "
          f"{s/bwalg:>10.2f}  {s/bwm:>10.2f}  "
          f"{abs_:>9.1f}%  {dram/bwm:>10.3f}")

print("""
  The sum (llcc + dram) / bwmon is typically 1.6–2.0×:
    - icc_llcc ≈ bwmon (total CPU→LLCC demand ≈ hardware DRAM measurement)
    - icc_dram ≈ 0.8 × bwmon (DCVS smoothing)
    - sum ≈ 1.8 × bwmon — pure double-counting artifact

  LLCC absorption ~30% means ~30% of CPU→LLCC requests hit in LLCC (no DRAM needed).
  This is consistent across all DRAM-regime workloads.
""")

# --------------------------------------------------------------------------
print()
print("=" * 70)
print("§6  Workload Characterisation & WS Regime Transitions")
print("=" * 70)

print("\n  bwmon mean (MB/s) by workload and WS:")
pivot = df.groupby(["workload", "ws_mb"]).bwmon_MBs.mean().unstack("ws_mb").round(0)
print(pivot.to_string())

print("\n  bus_access / bwmon ratio by workload and WS:")
df["bus_bwmon"] = np.where(df.bwmon_MBs > 100, df.bus_access_MBs / df.bwmon_MBs, np.nan)
ratio_pivot = df.groupby(["workload", "ws_mb"]).bus_bwmon.mean().unstack("ws_mb").round(3)
print(ratio_pivot.to_string())

print("""
Interpretation:
  bus_access / bwmon ≈ 1.05-1.10 in DRAM regime (WS ≥ 128 MB) — accurate for all workloads.
  bus_access / bwmon >> 1 at WS ≤ 16 MB for neon workloads:
    → bus_access (0x0019) counts L2→LLCC misses (external to CPU cluster),
      NOT just LLCC→DRAM misses. At WS > L2 (≈12 MB Oryon V2) but < LLCC,
      bus_access captures L2 miss bandwidth while bwmon stays low.
  Universal applicability:
    WS > 12 MB (Oryon L2 size): bus_access reflects total L2 miss bandwidth
    WS > LLCC (≈112 MB):        bus_access ≈ bwmon × 1.05 (DRAM bandwidth)
    WS ≤ 12 MB:                  both metrics near zero (L2-resident, no misses)
""")

print("  bw_alg mean (MB/s) — theoretical bandwidth demand (works at ALL WS sizes):")
alg_pivot = df.groupby(["workload", "ws_mb"]).bw_alg_MBs.mean().unstack("ws_mb").round(0)
print(alg_pivot.to_string())

print("""
  bw_alg IS a universal metric: it measures algorithm throughput × memory traffic
  regardless of whether data is cache-resident or DRAM-bound.
  Limitation: bw_alg does NOT measure actual DRAM bandwidth — it measures
  theoretical demand. The ratio bwmon / bw_alg (HW amplification) varies with
  prefetcher behaviour, cache hit rate, and access pattern.
""")

# HW amplification in DRAM regime
print("  HW prefetcher amplification (bwmon / bw_alg) in DRAM regime (WS≥128 MB):")
dram2 = df[df.is_dram & (df.ws_mb >= 128) & (df.bw_alg_MBs > 0)].copy()
for wl in WORKLOADS:
    sub = dram2[dram2.workload == wl]
    if sub.empty:
        continue
    amp = (sub.bwmon_MBs / sub.bw_alg_MBs)
    print(f"  {wl:<18}  {amp.mean():.3f} ± {amp.std():.3f}×")

# --------------------------------------------------------------------------
print()
print("=" * 70)
print("§7  Summary Table (DRAM regime, WS=128 MB)")
print("=" * 70)

sub128 = df[df.ws_mb == 128].copy()
metrics = ["bwmon_MBs", "bus_access_MBs", "icc_dram_agg_MBs", "icc_llcc_agg_MBs", "bw_alg_MBs"]
for wl in WORKLOADS:
    sub = sub128[sub128.workload == wl]
    print(f"\n  {wl}:")
    for m in metrics:
        v = sub[m]
        tag = " (DRAM)" if wl in ["neon_matvec","neon_axpy"] and m=="bwmon_MBs" else ""
        print(f"    {m:<22} mean={v.mean():8.0f}  cv={100*v.std()/v.mean():.1f}%{tag}")
