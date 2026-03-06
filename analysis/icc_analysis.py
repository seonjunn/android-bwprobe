"""
icc_analysis.py — Rigorous statistical analysis of icc_bw experiment results

Analyses:
  1. Metric accuracy  — how well bus_access, icc_dram_agg, icc_llcc_agg reflect bwmon
  2. Pairwise correlations — Pearson r, Spearman ρ, OLS regression (slope, intercept, R²)
  3. HW prefetcher amplification — bwmon / bw_alg per workload in DRAM regime
  4. Timeseries fidelity — CV within rep windows for bwmon vs bus_access traces
  5. ICC lag estimate — lagged cross-correlation between bwmon and ICC timeseries

Definitions:
  bwmon     = bw_hwmon_meas, hardware DRAM monitor (~4 ms, ground truth)
  bus_acc   = bus_access PMU (0x0019), per-process DRAM counter
  icc_dagg  = icc_set_bw agg_avg at ebi node, total DCVS vote (~40 ms cadence)
  icc_lagg  = icc_set_bw agg_avg at qns_llcc node
  bw_alg    = algorithmic bandwidth (bytes touched / elapsed)

DRAM regime: bwmon > 5000 MB/s (conservative threshold; excludes cache-resident ops).
  Exception: chase is lower-BW by nature; chase DRAM regime = WS >= 128 MB (bwmon > 2000).
"""

import pandas as pd
import numpy as np
from scipy import stats
import warnings
warnings.filterwarnings('ignore')

# ──────────────────────────────────────────────────────────────────────────────
# Load data
# ──────────────────────────────────────────────────────────────────────────────
df      = pd.read_csv("data/icc_bw_summary.csv")
bwmon_t = pd.read_csv("data/icc_bw_trace_bwmon.csv")
icc_t   = pd.read_csv("data/icc_bw_trace_icc.csv")
bus_t   = pd.read_csv("data/icc_bw_trace_bus.csv")
ctx     = pd.read_csv("data/icc_bw_context.csv")

# Convert icc kBps → MB/s in summary already done; rename for clarity
df = df.rename(columns={
    "bwmon_MBs":         "bwmon",
    "icc_llcc_avg_MBs":  "icc_lagg_avg",
    "icc_llcc_agg_MBs":  "icc_lagg",
    "icc_dram_avg_MBs":  "icc_dagg_avg",
    "icc_dram_agg_MBs":  "icc_dagg",
    "bw_alg_MBs":        "bw_alg",
    "bus_access_MBs":    "bus_acc",
})

SEP = "─" * 70

# ──────────────────────────────────────────────────────────────────────────────
# Helper: OLS regression stats
# ──────────────────────────────────────────────────────────────────────────────
def ols(x, y):
    """Return slope, intercept, R², RMSE, 95% CI on slope."""
    slope, intercept, r, p, se = stats.linregress(x, y)
    n = len(x)
    t_crit = stats.t.ppf(0.975, df=n - 2)
    ci = t_crit * se
    rmse = np.sqrt(np.mean((y - (slope * x + intercept))**2))
    return dict(slope=slope, intercept=intercept, r2=r**2, rmse=rmse,
                slope_ci=ci, p=p, n=n)

def pearson_ci(r, n, conf=0.95):
    """Fisher-transform 95% CI on Pearson r."""
    z = np.arctanh(r)
    se = 1 / np.sqrt(n - 3)
    zc = stats.norm.ppf((1 + conf) / 2)
    return np.tanh(z - zc * se), np.tanh(z + zc * se)

def ratio_stats(ratios, h0=1.0):
    """Mean, SD, 95% CI, one-sample t-test vs h0."""
    n = len(ratios)
    m, s = np.mean(ratios), np.std(ratios, ddof=1)
    se = s / np.sqrt(n)
    t_crit = stats.t.ppf(0.975, df=n - 1)
    ci = t_crit * se
    t_stat, p_val = stats.ttest_1samp(ratios, h0)
    return dict(mean=m, sd=s, ci=ci, t=t_stat, p=p_val, n=n)

# ──────────────────────────────────────────────────────────────────────────────
# Define DRAM regime
# ──────────────────────────────────────────────────────────────────────────────
dram = df[
    ((df.workload == "chase")  & (df.ws_mb >= 128) & (df.bwmon > 2000)) |
    ((df.workload != "chase")  & (df.bwmon > 5000))
].copy()

print(SEP)
print("icc_bw Experiment — Statistical Analysis")
print(f"Total reps: {len(df)}  |  DRAM-regime reps: {len(dram)}")
print(f"Workloads: {df.workload.unique().tolist()}  |  WS: {sorted(df.ws_mb.unique().tolist())} MB")
print(SEP)

# ──────────────────────────────────────────────────────────────────────────────
# §1  Metric Accuracy vs bwmon (DRAM regime)
# ──────────────────────────────────────────────────────────────────────────────
print("\n§1  Metric Accuracy vs bwmon (DRAM regime)")
print("    Each metric expressed as ratio to bwmon (ground truth = 1.000)")
print("    95% CI via one-sample t-test; H₀: mean ratio = 1.000\n")
print(f"  {'Metric':<18} {'n':>4}  {'mean':>7}  {'SD':>6}  {'95% CI':>18}  {'t':>6}  {'p':>8}  {'H₀ rejected?'}")
print(f"  {'-'*18} {'-'*4}  {'-'*7}  {'-'*6}  {'-'*18}  {'-'*6}  {'-'*8}  {'-'*12}")

metric_labels = [
    ("bus_acc",   "bus_access"),
    ("icc_dagg",  "icc_dram_agg"),
    ("icc_lagg",  "icc_llcc_agg"),
]
accuracy_results = {}
for col, label in metric_labels:
    valid = dram[(dram[col] > 0) & (dram.bwmon > 0)]
    ratios = (valid[col] / valid.bwmon).values
    rs = ratio_stats(ratios)
    ci_lo, ci_hi = rs['mean'] - rs['ci'], rs['mean'] + rs['ci']
    reject = "YES **" if rs['p'] < 0.05 else "no"
    print(f"  {label:<18} {rs['n']:>4}  {rs['mean']:>7.4f}  {rs['sd']:>6.4f}"
          f"  [{ci_lo:.4f}, {ci_hi:.4f}]  {rs['t']:>6.2f}  {rs['p']:>8.4f}  {reject}")
    accuracy_results[col] = rs

# ──────────────────────────────────────────────────────────────────────────────
# §2  Per-workload accuracy (DRAM regime)
# ──────────────────────────────────────────────────────────────────────────────
print("\n§2  Per-workload metric/bwmon ratios (DRAM regime)\n")
print(f"  {'Workload':<8} {'Metric':<18} {'n':>3}  {'mean':>7}  {'SD':>6}  {'CV%':>6}")
print(f"  {'-'*8} {'-'*18} {'-'*3}  {'-'*7}  {'-'*6}  {'-'*6}")
for wl in ["chase", "scalar", "neon"]:
    sub = dram[dram.workload == wl]
    for col, label in metric_labels:
        valid = sub[(sub[col] > 0) & (sub.bwmon > 0)]
        if len(valid) == 0:
            continue
        ratios = (valid[col] / valid.bwmon).values
        m, s = np.mean(ratios), np.std(ratios, ddof=1)
        cv = 100 * s / m if m != 0 else float('nan')
        print(f"  {wl:<8} {label:<18} {len(ratios):>3}  {m:>7.4f}  {s:>6.4f}  {cv:>6.1f}%")
    print()

# ──────────────────────────────────────────────────────────────────────────────
# §3  HW Prefetcher Amplification: bwmon / bw_alg
# ──────────────────────────────────────────────────────────────────────────────
print(f"§3  HW Prefetcher Amplification: bwmon / bw_alg (DRAM regime)\n")
print(f"  {'Workload':<8} {'WS (MB)':<10} {'n':>3}  {'mean':>6}  {'SD':>5}  {'95% CI':>18}  {'min':>6}  {'max':>6}")
print(f"  {'-'*8} {'-'*10} {'-'*3}  {'-'*6}  {'-'*5}  {'-'*18}  {'-'*6}  {'-'*6}")
amp_overall = {}
for wl in ["chase", "scalar", "neon"]:
    sub_all = dram[dram.workload == wl]
    for ws in sorted(sub_all.ws_mb.unique()):
        sub = sub_all[sub_all.ws_mb == ws]
        if len(sub) == 0:
            continue
        amps = (sub.bwmon / sub.bw_alg).values
        m, s = np.mean(amps), np.std(amps, ddof=1) if len(amps) > 1 else 0.0
        ci = stats.t.ppf(0.975, max(len(amps)-1, 1)) * s / np.sqrt(len(amps))
        print(f"  {wl:<8} {ws:<10} {len(amps):>3}  {m:>6.3f}  {s:>5.3f}  [{m-ci:.3f}, {m+ci:.3f}]"
              f"  {min(amps):>6.3f}  {max(amps):>6.3f}")
    # Overall across WS
    amps_all = (sub_all.bwmon / sub_all.bw_alg).values
    if len(amps_all) > 0:
        amp_overall[wl] = amps_all
        m, s = np.mean(amps_all), np.std(amps_all, ddof=1) if len(amps_all)>1 else 0
        rs = ratio_stats(amps_all, h0=1.0)
        ci_lo = rs['mean'] - rs['ci']; ci_hi = rs['mean'] + rs['ci']
        print(f"  {wl:<8} {'ALL WS':<10} {len(amps_all):>3}  {m:>6.3f}  {s:>5.3f}  [{ci_lo:.3f}, {ci_hi:.3f}]"
              f"  {min(amps_all):>6.3f}  {max(amps_all):>6.3f}")
    print()

# ──────────────────────────────────────────────────────────────────────────────
# §4  Pairwise Correlation Matrix (DRAM regime, all workloads pooled)
# ──────────────────────────────────────────────────────────────────────────────
print(f"§4  Pairwise Correlations (DRAM regime, all workloads pooled)\n")
metrics_all = ["bwmon", "bus_acc", "icc_dagg", "icc_lagg", "bw_alg"]
labels_all  = ["bwmon", "bus_acc", "icc_dram_agg", "icc_llcc_agg", "bw_alg"]

valid_dram = dram[(dram.bus_acc > 0) & (dram.icc_dagg > 0)].copy()
print(f"  n = {len(valid_dram)} DRAM-regime rep-points\n")

print("  Pearson r  (Fisher 95% CI):\n")
print(f"  {'':>14}", end="")
for l in labels_all:
    print(f"  {l:>14}", end="")
print()
for i, (c1, l1) in enumerate(zip(metrics_all, labels_all)):
    print(f"  {l1:>14}", end="")
    for j, (c2, l2) in enumerate(zip(metrics_all, labels_all)):
        if i == j:
            print(f"  {'1.000':>14}", end="")
        else:
            r, p = stats.pearsonr(valid_dram[c1], valid_dram[c2])
            lo, hi = pearson_ci(r, len(valid_dram))
            print(f"  {r:>6.4f}[{lo:.3f},{hi:.3f}]", end="")
    print()

print("\n  Spearman ρ (p-value):\n")
print(f"  {'':>14}", end="")
for l in labels_all:
    print(f"  {l:>16}", end="")
print()
for i, (c1, l1) in enumerate(zip(metrics_all, labels_all)):
    print(f"  {l1:>14}", end="")
    for j, (c2, l2) in enumerate(zip(metrics_all, labels_all)):
        if i == j:
            print(f"  {'1.000 (—)':>16}", end="")
        else:
            rho, p = stats.spearmanr(valid_dram[c1], valid_dram[c2])
            stars = "***" if p < 0.001 else ("**" if p < 0.01 else ("*" if p < 0.05 else ""))
            print(f"  {rho:>6.4f}(p={p:.2e}){stars:>2}", end="")
    print()

# ──────────────────────────────────────────────────────────────────────────────
# §5  OLS Regression: each metric → bwmon (DRAM regime)
# ──────────────────────────────────────────────────────────────────────────────
print(f"\n§5  OLS Regression: metric = slope × bwmon + intercept (DRAM regime)\n")
print(f"  {'Metric':<18} {'n':>3}  {'slope':>8}  {'slope 95% CI':>20}  {'intercept':>12}  {'R²':>6}  {'RMSE (MB/s)':>12}")
print(f"  {'-'*18} {'-'*3}  {'-'*8}  {'-'*20}  {'-'*12}  {'-'*6}  {'-'*12}")
ols_results = {}
for col, label in [("bus_acc", "bus_access"), ("icc_dagg", "icc_dram_agg"), ("icc_lagg", "icc_llcc_agg")]:
    valid = valid_dram[valid_dram[col] > 0]
    res = ols(valid.bwmon.values, valid[col].values)
    ci_lo = res['slope'] - res['slope_ci']
    ci_hi = res['slope'] + res['slope_ci']
    print(f"  {label:<18} {res['n']:>3}  {res['slope']:>8.4f}  [{ci_lo:.4f}, {ci_hi:.4f}]"
          f"  {res['intercept']:>12.1f}  {res['r2']:>6.4f}  {res['rmse']:>12.1f}")
    ols_results[col] = res

# Per-workload OLS for bus_access → bwmon
print(f"\n  Per-workload OLS: bus_access = slope × bwmon + intercept\n")
print(f"  {'Workload':<8} {'n':>3}  {'slope':>8}  {'slope 95% CI':>20}  {'intercept':>12}  {'R²':>6}  {'RMSE':>10}")
print(f"  {'-'*8} {'-'*3}  {'-'*8}  {'-'*20}  {'-'*12}  {'-'*6}  {'-'*10}")
for wl in ["chase", "scalar", "neon"]:
    sub = valid_dram[(valid_dram.workload == wl) & (valid_dram.bus_acc > 0)]
    if len(sub) < 3:
        continue
    res = ols(sub.bwmon.values, sub.bus_acc.values)
    ci_lo = res['slope'] - res['slope_ci']
    ci_hi = res['slope'] + res['slope_ci']
    print(f"  {wl:<8} {res['n']:>3}  {res['slope']:>8.4f}  [{ci_lo:.4f}, {ci_hi:.4f}]"
          f"  {res['intercept']:>12.1f}  {res['r2']:>6.4f}  {res['rmse']:>10.1f}")

# ──────────────────────────────────────────────────────────────────────────────
# §6  Timeseries Fidelity — CV of bus_access trace within DRAM rep windows
# ──────────────────────────────────────────────────────────────────────────────
print(f"\n§6  Timeseries Fidelity — within-rep CV (DRAM regime reps only)\n")
print(f"  Metric    WS   Workload   rep   n_samples   mean (MB/s)   CV%   Skewness")
print(f"  {'-'*8} {'-'*5} {'-'*8} {'-'*4} {'-'*9}   {'-'*12}  {'-'*6}  {'-'*8}")

ends   = ctx[ctx.event == "end"][["t_s","workload","ws_mb","rep"]].rename(columns={"t_s":"t_end"})
starts = ctx[ctx.event == "start"][["t_s","workload","ws_mb","rep"]].rename(columns={"t_s":"t_start"})
windows = starts.merge(ends, on=["workload","ws_mb","rep"])

# Only DRAM reps
dram_keys = dram[["workload","ws_mb","rep"]].drop_duplicates()
dram_windows = windows.merge(dram_keys, on=["workload","ws_mb","rep"])

for _, row in dram_windows.iterrows():
    wl, ws, r = row.workload, row.ws_mb, row.rep
    t0, t1 = row.t_start, row.t_end
    # bus_access trace
    seg_bus = bus_t[(bus_t.t_s >= t0) & (bus_t.t_s <= t1)].bus_access_MBs
    # bwmon trace
    seg_bw  = bwmon_t[(bwmon_t.t_s >= t0) & (bwmon_t.t_s <= t1)].bwmon_MBs
    for label, seg in [("bus_acc", seg_bus), ("bwmon", seg_bw)]:
        seg = seg[seg > 100]  # exclude zero-padded boundary samples
        if len(seg) < 3:
            continue
        m = seg.mean(); s = seg.std(ddof=1)
        cv = 100 * s / m if m > 0 else float('nan')
        sk = stats.skew(seg)
        print(f"  {label:<8} {ws:>4}MB {wl:<8} {r:>3}   {len(seg):>6}      {m:>10.0f}  {cv:>6.1f}%  {sk:>8.2f}")

# ──────────────────────────────────────────────────────────────────────────────
# §7  ICC Lag Estimation via Lagged Cross-Correlation
# ──────────────────────────────────────────────────────────────────────────────
print(f"\n§7  ICC Lag Estimation — cross-correlation between bwmon and ICC timeseries\n")
print("    Resample both to 10 ms grid; compute cross-correlation for lags 0..500 ms\n")
print(f"  {'WS':>6}  {'Workload':<8}  {'rep':>3}  {'lag_ms':>7}  {'r_max':>7}  {'r_at_0ms':>9}")
print(f"  {'-'*6}  {'-'*8}  {'-'*3}  {'-'*7}  {'-'*7}  {'-'*9}")

icc_ebi = icc_t[icc_t.node == "ebi"].copy()
icc_ebi["agg_MBs"] = icc_ebi.agg_kBps / 1e3

GRID_MS = 10  # ms grid for resampling
MAX_LAG_STEPS = 50  # 50 × 10ms = 500ms

lag_results = []
for _, row in dram_windows.iterrows():
    wl, ws, r = row.workload, row.ws_mb, row.rep
    t0, t1 = row.t_start, row.t_end

    seg_bw  = bwmon_t[(bwmon_t.t_s >= t0) & (bwmon_t.t_s <= t1)].copy()
    seg_icc = icc_ebi[(icc_ebi.t_s >= t0) & (icc_ebi.t_s <= t1)].copy()

    if len(seg_bw) < 20 or len(seg_icc) < 3:
        continue

    # Resample to uniform grid
    tmin = max(seg_bw.t_s.min(), seg_icc.t_s.min())
    tmax = min(seg_bw.t_s.max(), seg_icc.t_s.max())
    if tmax - tmin < 1.0:
        continue

    grid = np.arange(tmin, tmax, GRID_MS / 1000)
    bw_re  = np.interp(grid, seg_bw.t_s, seg_bw.bwmon_MBs)
    icc_re = np.interp(grid, seg_icc.t_s, seg_icc.agg_MBs)

    # Zero-mean normalize
    bw_z  = (bw_re  - bw_re.mean())  / (bw_re.std()  + 1e-9)
    icc_z = (icc_re - icc_re.mean()) / (icc_re.std() + 1e-9)

    # Compute cross-correlation for positive lags (ICC lags bwmon)
    n = len(bw_z)
    xcorr = np.correlate(bw_z, icc_z, mode='full') / n
    # xcorr[n-1+k] is the cross-correlation at lag k (ICC lags bwmon by k steps)
    max_k = min(MAX_LAG_STEPS, n - 1)
    pos_xcorr = xcorr[n - 1 : n - 1 + max_k + 1]
    pos_lags  = np.arange(0, max_k + 1) * GRID_MS  # ms

    best_idx = np.argmax(pos_xcorr)
    best_lag_ms = pos_lags[best_idx]
    r_max = pos_xcorr[best_idx]
    r_at_0 = pos_xcorr[0]

    print(f"  {ws:>5}MB  {wl:<8}  {r:>3}  {best_lag_ms:>7.0f}  {r_max:>7.4f}  {r_at_0:>9.4f}")
    lag_results.append(dict(workload=wl, ws_mb=ws, rep=r,
                             lag_ms=best_lag_ms, r_max=r_max, r_at_0=r_at_0))

if lag_results:
    ldf = pd.DataFrame(lag_results)
    ldf_dram = ldf[ldf.r_max > 0.3]  # only meaningful correlations
    if len(ldf_dram) > 0:
        print(f"\n  ICC lag summary (reps with r_max > 0.30):")
        print(f"    mean lag = {ldf_dram.lag_ms.mean():.0f} ms  ±  {ldf_dram.lag_ms.std():.0f} ms")
        print(f"    median lag = {ldf_dram.lag_ms.median():.0f} ms")
        print(f"    lag range = [{ldf_dram.lag_ms.min():.0f}, {ldf_dram.lag_ms.max():.0f}] ms")

# ──────────────────────────────────────────────────────────────────────────────
# §8  WS Effect on metric ratios (Kruskal-Wallis + pairwise Mann-Whitney)
# ──────────────────────────────────────────────────────────────────────────────
print(f"\n§8  WS Effect on bus_acc/bwmon ratio (Kruskal-Wallis test)\n")
print("    H₀: ratio distribution is identical across WS levels\n")
ws_groups = {}
for ws in sorted(dram.ws_mb.unique()):
    sub = dram[(dram.ws_mb == ws) & (dram.bus_acc > 0) & (dram.bwmon > 0)]
    if len(sub) > 0:
        ws_groups[ws] = (sub.bus_acc / sub.bwmon).values
        m = np.mean(ws_groups[ws])
        print(f"  WS={ws:>3}MB  n={len(ws_groups[ws])}  mean={m:.4f}  "
              f"SD={np.std(ws_groups[ws], ddof=1):.4f}  "
              f"vals={[f'{v:.3f}' for v in ws_groups[ws]]}")

if len(ws_groups) >= 2:
    stat, p = stats.kruskal(*ws_groups.values())
    print(f"\n  Kruskal-Wallis H = {stat:.3f},  p = {p:.4f}")
    print(f"  {'→ WS has a SIGNIFICANT effect' if p < 0.05 else '→ No significant WS effect'} on bus_acc/bwmon ratio (α = 0.05)")

# ──────────────────────────────────────────────────────────────────────────────
# §9  Summary table for README
# ──────────────────────────────────────────────────────────────────────────────
print(f"\n{SEP}")
print("§9  Summary Statistics for README\n")
print("  Per-rep aggregate results (all workloads, all WS):\n")
print(f"  {'Workload':<8} {'WS':>4}  {'bwmon':>8}  {'icc_dagg':>9}  {'bus_acc':>8}  {'bw_alg':>8}  {'amp':>6}  {'bus/bw':>7}  {'icc/bw':>7}")
print(f"  {'-'*8} {'-'*4}  {'-'*8}  {'-'*9}  {'-'*8}  {'-'*8}  {'-'*6}  {'-'*7}  {'-'*7}")
for wl in ["chase", "scalar", "neon"]:
    for ws in sorted(df.ws_mb.unique()):
        sub = df[(df.workload == wl) & (df.ws_mb == ws)]
        bm = sub.bwmon.mean()
        da = sub.icc_dagg.mean()
        ba = sub.bus_acc.mean()
        al = sub.bw_alg.mean()
        amp = bm / al if al > 0 else float('nan')
        brat = ba / bm if bm > 0 else float('nan')
        irat = da / bm if bm > 0 else float('nan')
        print(f"  {wl:<8} {ws:>4}  {bm:>8.0f}  {da:>9.0f}  {ba:>8.0f}  {al:>8.0f}  {amp:>6.3f}  {brat:>7.4f}  {irat:>7.4f}")
    print()

print(SEP)
print("Analysis complete.")
