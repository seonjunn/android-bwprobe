"""
burst_sweep_analysis.py — Analyse burst_sweep experiment results.

Three experiments:
  Exp 1: bwmon/bw_alg amplification factor vs WS (neon_matvec dense sweep).
         Prediction: rises from ~0 at WS ≤ 12 MB (L2-resident) to ~1.88 at WS ≫ L2.
         If mechanism is L2 thrashing + LPDDR5X 128B burst, crossover at WS ≈ 12–20 MB.
  Exp 2: Non-temporal store (store_nt): does bwmon count write traffic?
         stnp bypasses cache — pure writes, no read-allocate.
  Exp 3: DRAM ceiling: does bwmon saturate at ≤ 85.33 GB/s or above?

Hypotheses:
  H1: bwmon is at L2→LLCC boundary (100 GB/s is on-chip traffic, not DRAM).
      Evidence: icc_dram ≤ 85.33 GB/s while bwmon > 85.33 GB/s.
  H2: True DRAM peak on Galaxy S25+ exceeds product brief (SM8750-AC ≠ SM8750-3-AB, or burst > sustained).
      Evidence: bwmon saturates above 85.33 GB/s.

Usage: python3 analysis/burst_sweep_analysis.py [data/burst_sweep.csv]
"""

import argparse
import os
import sys
import numpy as np
import pandas as pd

DRAM_SPEC_GBs = 85.33   # Qualcomm product brief SM8750 DRAM peak (GB/s)
L2_MB         = 12      # L2 cache size (Prime cluster, MB)
EXPECTED_AMP  = 1.88    # expected bwmon/bw_alg at DRAM WS (128B burst / 64B demand)

ap = argparse.ArgumentParser(description=__doc__,
                             formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("path", nargs="?", default="data/burst_sweep.csv",
                help="path to burst_sweep.csv (default: data/burst_sweep.csv)")
args = ap.parse_args()

if not os.path.exists(args.path):
    print(f"ERROR: {args.path} not found. Run: make runburst", file=sys.stderr)
    sys.exit(1)

df = pd.read_csv(args.path)

matvec = df[df.workload == "neon_matvec"].copy()
store  = df[df.workload == "store_nt"].copy()

# ============================================================
# §1  Exp 1 — Amplification factor vs WS
# ============================================================
print("=" * 72)
print("§1  Exp 1 — bwmon/bw_alg Amplification Factor vs WS (neon_matvec)")
print("=" * 72)
print(f"\nL2 size: {L2_MB} MB  |  Expected plateau: {EXPECTED_AMP}×  "
      f"|  DRAM spec: {DRAM_SPEC_GBs} GB/s")
print()

# Per-WS mean
grp = matvec.groupby("ws_mb").agg(
    bwmon   = ("bwmon_MBs",      "mean"),
    bw_alg  = ("bw_alg_MBs",    "mean"),
    bus     = ("bus_access_MBs", "mean"),
    icc_d   = ("icc_dram_agg_MBs", "mean"),
    icc_l   = ("icc_llcc_agg_MBs", "mean"),
    n       = ("rep",            "count"),
).reset_index()

grp["amp_bwmon"] = grp.bwmon  / grp.bw_alg
grp["amp_bus"]   = grp.bus    / grp.bw_alg

# Regime markers
def regime(ws):
    if ws <= L2_MB:      return "L2-resident"
    if ws <= 112:        return "LLCC/transit"
    return "DRAM"

grp["regime"] = grp.ws_mb.apply(regime)

hdr = (f"{'ws_mb':>6}  {'bwmon':>8}  {'bw_alg':>8}  {'bus':>8}  "
       f"{'icc_dram':>8}  {'bwmon/alg':>9}  {'bus/alg':>7}  {'regime':<14}")
print(hdr)
print("-" * len(hdr))
for _, row in grp.iterrows():
    amp_str = f"{row.amp_bwmon:9.3f}" if row.bwmon > 100 else "      N/A"
    bus_str = f"{row.amp_bus:7.3f}"   if row.bus   > 100 else "    N/A"
    icc_str = f"{row.icc_d:8.0f}"    if row.icc_d  > 0  else "     N/A"
    print(f"{row.ws_mb:>6}  {row.bwmon:8.0f}  {row.bw_alg:8.0f}  {row.bus:8.0f}  "
          f"{icc_str}  {amp_str}  {bus_str}  {row.regime:<14}")

# Summarise transition
dram_rows = grp[grp.regime == "DRAM"]
if not dram_rows.empty:
    amp_mean = dram_rows.amp_bwmon.mean()
    amp_sd   = dram_rows.amp_bwmon.std()
    print(f"\nbwmon/bw_alg at DRAM WS: {amp_mean:.3f} ± {amp_sd:.3f} "
          f"(expected {EXPECTED_AMP}×)")

# Find crossover WS (first point where amp_bwmon > 0.5)
cross = grp[(grp.amp_bwmon > 0.5) & (grp.regime != "L2-resident")]
if not cross.empty:
    print(f"Amplification > 0.5× first at WS = {cross.ws_mb.min()} MB "
          f"(L2 = {L2_MB} MB)")

# ============================================================
# §2  H1 resolution: icc_dram vs bwmon
# ============================================================
print()
print("=" * 72)
print("§2  H1 — Is bwmon at DRAM boundary? (icc_dram vs bwmon)")
print("=" * 72)
print(f"\nDRAM spec ceiling: {DRAM_SPEC_GBs} GB/s = {DRAM_SPEC_GBs * 1e3:.0f} MB/s")
print("If icc_dram ≤ spec while bwmon > spec → bwmon is NOT the DRAM boundary.\n")

relevant = grp[grp.bwmon > DRAM_SPEC_GBs * 1e3].copy()
if relevant.empty:
    print("No WS points with bwmon > DRAM spec. Cannot resolve H1 from this data.")
    print("bwmon may not be saturating DRAM at any WS tested.")
else:
    print(f"{'ws_mb':>6}  {'bwmon_MBs':>10}  {'icc_dram_MBs':>12}  "
          f"{'icc_dram/spec':>13}  {'bwmon/spec':>10}  {'H1 evidence?':}")
    print("-" * 72)
    spec_MBs = DRAM_SPEC_GBs * 1e3
    for _, row in relevant.iterrows():
        icc_str = f"{row.icc_d:12.0f}" if row.icc_d > 0 else "         N/A"
        icc_frac = f"{row.icc_d / spec_MBs:13.3f}" if row.icc_d > 0 else "          N/A"
        h1 = "YES (icc_dram ≤ spec < bwmon)" if (row.icc_d > 0 and row.icc_d <= spec_MBs) else "no"
        print(f"{row.ws_mb:>6}  {row.bwmon:10.0f}  {icc_str}  "
              f"{icc_frac}  {row.bwmon / spec_MBs:10.3f}  {h1}")

# ============================================================
# §3  Exp 3 — DRAM ceiling
# ============================================================
print()
print("=" * 72)
print("§3  Exp 3 — DRAM Ceiling: bwmon at large WS")
print("=" * 72)
print()

ceiling_ws = matvec[matvec.ws_mb >= 128].groupby("ws_mb").agg(
    bwmon_mean = ("bwmon_MBs",      "mean"),
    bwmon_sd   = ("bwmon_MBs",      "std"),
    bus_mean   = ("bus_access_MBs", "mean"),
    alg_mean   = ("bw_alg_MBs",     "mean"),
).reset_index()

print(f"{'ws_mb':>6}  {'bwmon_MBs':>10}  {'±':>6}  {'bus_MBs':>10}  "
      f"{'bw_alg_MBs':>11}  {'vs_spec':>7}")
spec_MBs = DRAM_SPEC_GBs * 1e3
for _, row in ceiling_ws.iterrows():
    sd_str = f"{row.bwmon_sd:6.0f}" if not np.isnan(row.bwmon_sd) else "   N/A"
    print(f"{row.ws_mb:>6}  {row.bwmon_mean:10.0f}  {sd_str}  "
          f"{row.bus_mean:10.0f}  {row.alg_mean:11.0f}  "
          f"{row.bwmon_mean / spec_MBs:7.3f}×")

if not ceiling_ws.empty:
    peak = ceiling_ws.bwmon_mean.max()
    peak_ws = ceiling_ws.loc[ceiling_ws.bwmon_mean.idxmax(), "ws_mb"]
    print(f"\nPeak bwmon: {peak:.0f} MB/s = {peak:.0f} / 1000 = {peak/1e3:.2f} GB/s "
          f"at WS = {peak_ws} MB")
    print(f"DRAM spec:  {DRAM_SPEC_GBs:.2f} GB/s")
    if peak > spec_MBs:
        diff = (peak / spec_MBs - 1) * 100
        print(f"bwmon exceeds spec by {diff:.1f}% → "
              "supports H2 (device peak > brief) or H1 (bwmon ≠ DRAM boundary)")
    else:
        print(f"bwmon ≤ spec → consistent with bwmon at DRAM boundary, "
              "spec confirmed for this device")

# ============================================================
# §4  Exp 2 — Non-temporal store
# ============================================================
print()
print("=" * 72)
print("§4  Exp 2 — Non-Temporal Store: Does bwmon Count Writes?")
print("=" * 72)
print("\nstnp bypasses all cache levels → pure DRAM write traffic, no read-allocate.")
print("If bwmon ≈ 0:      bwmon = reads-only confirmed.")
print("If bwmon ≈ bw_alg: bwmon counts writes (no burst amp for non-temporal).")
print("If bwmon ≈ 1.88×:  bwmon counts writes with burst amplification.\n")

if store.empty:
    print("No store_nt data found in CSV.")
else:
    sg = store.groupby("ws_mb").agg(
        bwmon   = ("bwmon_MBs",      "mean"),
        bw_alg  = ("bw_alg_MBs",    "mean"),
        bus     = ("bus_access_MBs", "mean"),
        n       = ("rep",            "count"),
    ).reset_index()
    sg["bwmon_alg"] = sg.bwmon / sg.bw_alg
    sg["bus_alg"]   = sg.bus   / sg.bw_alg

    print(f"{'ws_mb':>6}  {'bwmon_MBs':>10}  {'bw_alg_MBs':>11}  "
          f"{'bus_MBs':>9}  {'bwmon/alg':>9}  {'bus/alg':>7}  interpretation")
    print("-" * 80)
    for _, row in sg.iterrows():
        if row.bwmon < 500:
            interp = "bwmon ≈ 0 → reads-only ✓"
        elif abs(row.bwmon_alg - EXPECTED_AMP) < 0.2:
            interp = f"bwmon/alg ≈ {EXPECTED_AMP}× → writes + burst amp"
        elif abs(row.bwmon_alg - 1.0) < 0.2:
            interp = "bwmon/alg ≈ 1× → writes, no burst amp"
        else:
            interp = f"unexpected ratio {row.bwmon_alg:.2f}"
        print(f"{row.ws_mb:>6}  {row.bwmon:10.0f}  {row.bw_alg:11.0f}  "
              f"{row.bus:9.0f}  {row.bwmon_alg:9.3f}  {row.bus_alg:7.3f}  {interp}")

    # bus_access for store: should fire regardless (non-temporal crosses L2 boundary)
    bus_rows = sg[sg.bus > 100]
    if not bus_rows.empty:
        mean_bus_alg = bus_rows.bus_alg.mean()
        print(f"\nbus_access/bw_alg for store_nt: {mean_bus_alg:.3f} "
              "(expect ≈ 1.0 — non-temporal stores cross L2 boundary)")

# ============================================================
# §5  Summary: hypothesis resolution
# ============================================================
print()
print("=" * 72)
print("§5  Hypothesis Resolution")
print("=" * 72)
print()

# H1 check: any WS where bwmon > spec but icc_dram ≤ spec?
h1_evidence = False
if not grp.empty:
    spec_MBs = DRAM_SPEC_GBs * 1e3
    h1_rows = grp[(grp.bwmon > spec_MBs) & (grp.icc_d > 0) & (grp.icc_d <= spec_MBs)]
    if not h1_rows.empty:
        h1_evidence = True
        print("H1 (bwmon ≠ DRAM boundary): SUPPORTED")
        print(f"  bwmon > {DRAM_SPEC_GBs} GB/s but icc_dram ≤ {DRAM_SPEC_GBs} GB/s at WS: "
              f"{h1_rows.ws_mb.tolist()} MB")
    else:
        print("H1 (bwmon ≠ DRAM boundary): NOT SUPPORTED by this data")
        print("  No WS where bwmon > spec AND icc_dram ≤ spec simultaneously")

# H2 check: bwmon saturates above spec?
if not ceiling_ws.empty:
    peak = ceiling_ws.bwmon_mean.max()
    if peak > spec_MBs and not h1_evidence:
        print(f"\nH2 (device DRAM peak > product brief): SUPPORTED")
        print(f"  bwmon peaks at {peak/1e3:.2f} GB/s > spec {DRAM_SPEC_GBs} GB/s")
    elif peak > spec_MBs and h1_evidence:
        print(f"\nH2 (device DRAM peak > product brief): AMBIGUOUS")
        print(f"  bwmon > spec but H1 also supported — bwmon may not be DRAM-level")
    else:
        print(f"\nH2 (device DRAM peak > product brief): NOT SUPPORTED")
        print(f"  bwmon {peak/1e3:.2f} GB/s ≤ spec {DRAM_SPEC_GBs} GB/s")

# Amplification mechanism check
if not dram_rows.empty:
    amp_mean = dram_rows.amp_bwmon.mean()
    print(f"\nBurst amplification mechanism:")
    print(f"  bwmon/bw_alg at DRAM WS = {amp_mean:.3f} (expected {EXPECTED_AMP})")
    if not cross.empty:
        print(f"  Amplification onset WS = {cross.ws_mb.min()} MB "
              f"(L2 = {L2_MB} MB) — "
              + ("consistent with L2 thrashing" if cross.ws_mb.min() <= 20 else
                 "onset above expected — check data"))

# Exp 2 summary
if not store.empty:
    bwmon_nt = store.bwmon_MBs.mean()
    if bwmon_nt < 500:
        print(f"\nExp 2 (non-temporal store): bwmon ≈ {bwmon_nt:.0f} MB/s ≈ 0")
        print("  → bwmon counts READ traffic only (non-temporal writes invisible to bwmon)")
    else:
        print(f"\nExp 2 (non-temporal store): bwmon = {bwmon_nt:.0f} MB/s ≠ 0")
        print("  → bwmon sees write traffic from non-temporal stores")

# ============================================================
# §6  Optional: matplotlib plots
# ============================================================
try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(1, 3, figsize=(15, 5))
    fig.suptitle("burst_sweep — bwmon amplification, DRAM ceiling, store experiment")

    # Plot 1: bwmon/bw_alg vs WS
    ax = axes[0]
    valid = grp[grp.amp_bwmon > 0]
    ax.semilogx(valid.ws_mb, valid.amp_bwmon, "o-", label="bwmon/bw_alg", color="steelblue")
    ax.semilogx(valid.ws_mb, valid.amp_bus,   "s--", label="bus/bw_alg",  color="orange")
    ax.axhline(EXPECTED_AMP, color="gray", linestyle=":", label=f"expected {EXPECTED_AMP}×")
    ax.axvline(L2_MB, color="red", linestyle="--", alpha=0.5, label=f"L2={L2_MB}MB")
    ax.set_xlabel("Working Set (MB)")
    ax.set_ylabel("Amplification ratio")
    ax.set_title("Exp 1: bwmon/bw_alg vs WS")
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.3)
    ax.set_ylim(bottom=0)

    # Plot 2: bwmon + icc_dram vs WS (DRAM ceiling)
    ax = axes[1]
    ax.semilogx(grp.ws_mb, grp.bwmon / 1e3,  "o-", label="bwmon", color="steelblue")
    valid_icc = grp[grp.icc_d > 0]
    if not valid_icc.empty:
        ax.semilogx(valid_icc.ws_mb, valid_icc.icc_d / 1e3, "s--",
                    label="icc_dram", color="green")
    ax.axhline(DRAM_SPEC_GBs, color="red", linestyle="--", alpha=0.7,
               label=f"spec {DRAM_SPEC_GBs} GB/s")
    ax.axvline(L2_MB, color="gray", linestyle=":", alpha=0.5, label=f"L2={L2_MB}MB")
    ax.set_xlabel("Working Set (MB)")
    ax.set_ylabel("Bandwidth (GB/s)")
    ax.set_title("Exp 3: DRAM ceiling")
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.3)

    # Plot 3: non-temporal store
    ax = axes[2]
    if not store.empty:
        sg_plot = store.groupby("ws_mb")[["bwmon_MBs", "bw_alg_MBs", "bus_access_MBs"]].mean()
        ws_vals = sg_plot.index.values
        x_pos   = np.arange(len(ws_vals))
        w       = 0.25
        ax.bar(x_pos - w, sg_plot.bw_alg_MBs / 1e3,  w, label="bw_alg",  color="steelblue")
        ax.bar(x_pos,     sg_plot.bwmon_MBs  / 1e3,  w, label="bwmon",   color="orange")
        ax.bar(x_pos + w, sg_plot.bus_access_MBs / 1e3, w, label="bus",  color="green")
        ax.set_xticks(x_pos)
        ax.set_xticklabels([f"{ws}MB" for ws in ws_vals])
        ax.set_ylabel("Bandwidth (GB/s)")
        ax.set_title("Exp 2: non-temporal store")
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3, axis="y")
    else:
        ax.text(0.5, 0.5, "No store_nt data", ha="center", va="center",
                transform=ax.transAxes)

    plt.tight_layout()
    out = "data/burst_sweep_plots.png"
    plt.savefig(out, dpi=150)
    print(f"\nPlots saved to {out}")

except ImportError:
    print("\n(matplotlib not available — skipping plots)")
except Exception as e:
    print(f"\n(plot error: {e})")
