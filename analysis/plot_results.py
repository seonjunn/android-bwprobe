#!/usr/bin/env python3
"""
plot_results.py -- visualise bwprobe and matvec CSV data.

Reads:  /tmp/bwprobe_clean.csv   (21 WS points x 7 reps)
        /tmp/matvec.csv          (8 WS points x 7 reps)

Writes: analysis/figures/fig1_bwprobe_bw_lat.png
        analysis/figures/fig2_bwprobe_pmu.png
        analysis/figures/fig3_matvec.png
"""

import csv
import collections
import os
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import seaborn as sns

# ── Style ──────────────────────────────────────────────────────────────────
sns.set_theme(style="whitegrid", context="paper", font_scale=1.25)
PALETTE = sns.color_palette("deep")
C_CACHE = PALETTE[0]   # blue  -- cache-resident
C_DRAM  = PALETTE[3]   # red   -- DRAM-active
C_ANOM  = PALETTE[7]   # grey  -- anomaly
C_NEON  = PALETTE[1]   # green -- NEON kernel
C_SCAL  = PALETTE[2]   # orange-- scalar kernel
C_BWMON = PALETTE[4]   # purple-- bwmon

OUT_DIR = os.path.join(os.path.dirname(__file__), "figures")
os.makedirs(OUT_DIR, exist_ok=True)

# ── Load bwprobe ────────────────────────────────────────────────────────────
def load_bwprobe(path="/tmp/bwprobe_clean.csv"):
    rows = []
    with open(path) as f:
        for row in csv.DictReader(f):
            rows.append({k: float(v) for k, v in row.items()})
    by_ws = collections.defaultdict(list)
    for r in rows:
        by_ws[int(r["ws_MB"])].append(r)
    ws_list = sorted(by_ws.keys())
    def mean(key):
        return np.array([np.mean([r[key] for r in by_ws[ws]]) for ws in ws_list])
    def sd(key):
        return np.array([np.std([r[key] for r in by_ws[ws]], ddof=1) for ws in ws_list])
    return {
        "ws":           np.array(ws_list),
        "bwmon":        mean("bw_hwmon_prime_MBs"),
        "bwmon_sd":     sd("bw_hwmon_prime_MBs"),
        "lat":          mean("lat_ns"),
        "lat_sd":       sd("lat_ns"),
        "l2d":          mean("bw_l2d_cache_MBs"),
        "l2d_sd":       sd("bw_l2d_cache_MBs"),
        "bus":          mean("bw_bus_access_MBs"),
        "bus_sd":       sd("bw_bus_access_MBs"),
        "chase":        mean("chase_GBs"),
        "chase_sd":     sd("chase_GBs"),
    }

# ── Load matvec ─────────────────────────────────────────────────────────────
def load_matvec(path="/tmp/matvec.csv"):
    rows = []
    with open(path) as f:
        for row in csv.DictReader(f):
            rows.append({k: float(v) for k, v in row.items()})
    by_ws = collections.defaultdict(list)
    for r in rows:
        by_ws[int(r["ws_MB"])].append(r)
    ws_list = sorted(by_ws.keys())
    def mean(key):
        return np.array([np.mean([r[key] for r in by_ws[ws]]) / 1000.0 for ws in ws_list])
    def sd(key):
        return np.array([np.std([r[key] for r in by_ws[ws]], ddof=1) / 1000.0 for ws in ws_list])
    return {
        "ws":           np.array(ws_list),
        "sc":           mean("bw_matvec_MBs"),
        "sc_sd":        sd("bw_matvec_MBs"),
        "ne":           mean("bw_matvec_neon_MBs"),
        "ne_sd":        sd("bw_matvec_neon_MBs"),
        "bwmon_sc":     mean("bw_hwmon_MBs"),
        "bwmon_sc_sd":  sd("bw_hwmon_MBs"),
        "bwmon_ne":     mean("bw_hwmon_neon_MBs"),
        "bwmon_ne_sd":  sd("bw_hwmon_neon_MBs"),
    }

bp = load_bwprobe()
mv = load_matvec()

# Classify bwprobe WS points
DRAM_WS   = {128, 160, 256}
ANOM_WS   = {192}

def classify(ws):
    if ws in DRAM_WS:   return C_DRAM
    if ws in ANOM_WS:   return C_ANOM
    return C_CACHE

# ── Figure 1: bwprobe bandwidth + latency ───────────────────────────────────
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))
fig.suptitle("bwprobe: Random Pointer-Chase Sweep\n"
             "Snapdragon 8 Elite (Oryon V2) · cpu6 · freq locked · 7 reps/WS",
             fontsize=11)

# -- Left: bwmon-prime bandwidth
colors = [classify(ws) for ws in bp["ws"]]
ax1.errorbar(bp["ws"], bp["bwmon"] / 1000,
             yerr=bp["bwmon_sd"] / 1000,
             fmt="none", ecolor="grey", elinewidth=1, capsize=3, zorder=2)
ax1.scatter(bp["ws"], bp["bwmon"] / 1000, c=colors, s=60, zorder=3)
# shade regime regions
ax1.axvspan(1, 115, alpha=0.06, color=C_CACHE, label="Cache-resident (≤112 MB)")
ax1.axvspan(115, 270, alpha=0.06, color=C_DRAM, label="DRAM-active (≥128 MB)")

# Annotate anomaly
anom_idx = list(bp["ws"]).index(192)
ax1.annotate("192 MB\nanomal", xy=(192, bp["bwmon"][anom_idx]/1000),
             xytext=(150, 2200), fontsize=8, color=C_ANOM,
             arrowprops=dict(arrowstyle="->", color=C_ANOM, lw=1))
ax1.set_xscale("log", base=2)
ax1.set_xticks([2, 4, 8, 16, 32, 64, 128, 256])
ax1.get_xaxis().set_major_formatter(ticker.ScalarFormatter())
ax1.set_xlabel("Working-set size (MB)")
ax1.set_ylabel("bwmon-prime DRAM BW (GB/s)")
ax1.set_title("DRAM Bandwidth vs Working-Set Size")
ax1.legend(fontsize=8, loc="upper left")

# -- Right: CNTVCT latency
ax2.errorbar(bp["ws"], bp["lat"],
             yerr=bp["lat_sd"],
             fmt="none", ecolor="grey", elinewidth=1, capsize=3, zorder=2)
sc2 = ax2.scatter(bp["ws"], bp["lat"], c=colors, s=60, zorder=3)

# Latency tier annotations
ax2.axhspan(0.0,  1.6, alpha=0.10, color="royalblue")
ax2.axhspan(1.6,  3.5, alpha=0.10, color="orange")
ax2.axhspan(3.5, 10.0, alpha=0.10, color="red")
ax2.text(1.2, 0.85, "L1D / L2\n~1 ns", fontsize=8, color="royalblue", va="top")
ax2.text(1.2, 2.9, "LLCC\n~2–3 ns", fontsize=8, color="darkorange")
ax2.text(1.2, 7.5, "DRAM\n~5–6 ns", fontsize=8, color="firebrick")

ax2.set_xscale("log", base=2)
ax2.set_xticks([2, 4, 8, 16, 32, 64, 128, 256])
ax2.get_xaxis().set_major_formatter(ticker.ScalarFormatter())
ax2.set_xlabel("Working-set size (MB)")
ax2.set_ylabel("Latency per hop (ns, via CNTVCT_EL0)")
ax2.set_title("Access Latency vs Working-Set Size")
ax2.set_ylim(0, 9)

plt.subplots_adjust(left=0.09, right=0.97, top=0.88, bottom=0.13, wspace=0.32)
out1 = os.path.join(OUT_DIR, "fig1_bwprobe_bw_lat.png")
plt.savefig(out1, dpi=100)
plt.close()
print(f"Saved {out1}")

# ── Figure 2: bwprobe PMU detail (l2d_cache vs bus_access) ──────────────────
fig, ax = plt.subplots(figsize=(8, 5))
fig.suptitle("bwprobe: PMU Bandwidth vs Working-Set Size", fontsize=11)

ax.plot(bp["ws"], bp["l2d"] / 1000, "o-", color=PALETTE[0],
        label="l2d_cache (L1D-miss BW)", linewidth=1.5, markersize=5)
ax.plot(bp["ws"], bp["bus"] / 1000, "s--", color=C_DRAM,
        label="bus_access (DRAM BW, process)", linewidth=1.5, markersize=5)
ax.plot(bp["ws"], bp["bwmon"] / 1000, "^:", color=C_BWMON,
        label="bwmon-prime (DRAM BW, cluster)", linewidth=1.5, markersize=5)

# Annotate regime boundary
ax.axvline(128, color="grey", linestyle=":", linewidth=1)
ax.text(128, ax.get_ylim()[1]*0.95 if ax.get_ylim()[1] > 0 else 10,
        " ←DRAM\n  onset", fontsize=8, color="grey", va="top")

ax.set_xscale("log", base=2)
ax.set_xticks([2, 4, 8, 16, 32, 64, 128, 256])
ax.get_xaxis().set_major_formatter(ticker.ScalarFormatter())
ax.set_xlabel("Working-set size (MB)")
ax.set_ylabel("Bandwidth (GB/s)")
ax.legend(fontsize=9)
ax.set_title("l2d_cache vs bus_access vs bwmon\n"
             "l2d_cache fires for LLCC hits too; bus_access only fires at DRAM")

plt.tight_layout()
out2 = os.path.join(OUT_DIR, "fig2_bwprobe_pmu.png")
plt.savefig(out2, dpi=100, bbox_inches="tight")
plt.close()
print(f"Saved {out2}")

# ── Figure 3: matvec scalar vs NEON ─────────────────────────────────────────
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))
fig.suptitle("matvec  y = Ax  (float32)  ·  Scalar vs Explicit-NEON Kernel\n"
             "Snapdragon 8 Elite (Oryon V2) · cpu6 · freq locked · 7 reps/WS",
             fontsize=11)

ws = mv["ws"]

# -- Left: useful read bandwidth (algorithm throughput)
ax1.errorbar(ws, mv["sc"], yerr=mv["sc_sd"],
             fmt="o-", color=C_SCAL, label="Scalar (A-reads)", capsize=3, linewidth=1.5)
ax1.errorbar(ws, mv["ne"], yerr=mv["ne_sd"],
             fmt="s-", color=C_NEON, label="NEON 8-acc (A-reads)", capsize=3, linewidth=1.5)
# bwmon lines (total DRAM)
ax1.errorbar(ws, mv["bwmon_sc"], yerr=mv["bwmon_sc_sd"],
             fmt="o--", color=C_SCAL, alpha=0.5, label="Scalar bwmon (total)", capsize=3)
ax1.errorbar(ws, mv["bwmon_ne"], yerr=mv["bwmon_ne_sd"],
             fmt="s--", color=C_NEON, alpha=0.5, label="NEON bwmon (total)", capsize=3)

# Regime annotation
ax1.axvspan(1, 10, alpha=0.06, color="royalblue")
ax1.axvspan(10, 270, alpha=0.06, color="tomato")
ax1.text(1.2, 5, "cache\nresident", fontsize=8, color="royalblue", va="center")
ax1.text(20, 105, "DRAM-active", fontsize=8, color="firebrick")

# Compute-bound arrow for scalar
ax1.annotate("compute-bound\n(FMA latency)\n~5.6 GB/s",
             xy=(64, 5.88), xytext=(6, 12),
             fontsize=8, color=C_SCAL,
             arrowprops=dict(arrowstyle="->", color=C_SCAL, lw=1))
# Memory-bound annotation for NEON
ax1.annotate("DRAM-bound\n~54 GB/s",
             xy=(128, 53.78), xytext=(20, 60),
             fontsize=8, color=C_NEON,
             arrowprops=dict(arrowstyle="->", color=C_NEON, lw=1))

ax1.set_xscale("log", base=2)
ax1.set_xticks([2, 4, 8, 16, 32, 64, 128, 256])
ax1.get_xaxis().set_major_formatter(ticker.ScalarFormatter())
ax1.set_xlabel("Working-set size (MB)")
ax1.set_ylabel("Bandwidth (GB/s)")
ax1.set_title("Algorithm Throughput & DRAM Traffic")
ax1.legend(fontsize=8, loc="upper right")

# -- Right: speedup and bwmon/bw_matvec amplification factor
speedup = mv["ne"] / mv["sc"]
amp_sc  = mv["bwmon_sc"] / mv["sc"]
amp_ne  = mv["bwmon_ne"] / mv["ne"]

ax2r = ax2.twinx()

ax2.plot(ws, speedup, "D-", color=PALETTE[5], label="NEON / scalar speedup", linewidth=2, markersize=6)
ax2r.plot(ws, amp_sc, "o--", color=C_SCAL, alpha=0.8, label="bwmon/bw_matvec (scalar)", linewidth=1.5, markersize=5)
ax2r.plot(ws, amp_ne, "s--", color=C_NEON, alpha=0.8, label="bwmon/bw_matvec (NEON)", linewidth=1.5, markersize=5)

ax2r.axhline(1.85, color="grey", linestyle=":", linewidth=1)
ax2r.text(2.2, 1.87, "~1.85× amplification", fontsize=8, color="grey")

ax2.set_xscale("log", base=2)
ax2.set_xticks([2, 4, 8, 16, 32, 64, 128, 256])
ax2.get_xaxis().set_major_formatter(ticker.ScalarFormatter())
ax2.set_xlabel("Working-set size (MB)")
ax2.set_ylabel("NEON / scalar speedup (×)", color=PALETTE[5])
ax2r.set_ylabel("bwmon / bw_matvec amplification (×)")
ax2.set_title("Speedup (left) & Dirty-Write Amplification (right)")

lines1, labels1 = ax2.get_legend_handles_labels()
lines2, labels2 = ax2r.get_legend_handles_labels()
ax2.legend(lines1 + lines2, labels1 + labels2, fontsize=8, loc="lower right")

plt.tight_layout()
out3 = os.path.join(OUT_DIR, "fig3_matvec.png")
plt.savefig(out3, dpi=100, bbox_inches="tight")
plt.close()
print(f"Saved {out3}")

print("Done.")
