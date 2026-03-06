"""
split_traces.py — Post-process combined trace files into per-(workload, metric) CSVs.

Input (data/ directory):
  icc_bw_context.csv    — rep start/end markers
  icc_bw_trace_bwmon.csv — t_s, bwmon_MBs
  icc_bw_trace_bus.csv  — t_s, bus_access_MBs
  icc_bw_trace_icc.csv  — t_s, node, avg_kBps, agg_kBps

Output (data/traces/):
  {workload}_bwmon.csv      columns: t_s, ws_mb, rep, value_MBs
  {workload}_bus_access.csv
  {workload}_icc_dram.csv   (ebi node, agg_kBps → MB/s)
  {workload}_icc_llcc.csv   (qns_llcc node, agg_kBps → MB/s)
  {workload}_bw_alg.csv     (constant within each rep window, from context end event)

Usage: python3 analysis/split_traces.py [--data data/] [--prefix icc_bw]
"""

import argparse
import os
import pandas as pd

def split(data_dir="data/", prefix="icc_bw"):
    ctx    = pd.read_csv(os.path.join(data_dir, f"{prefix}_context.csv"))
    bwmon  = pd.read_csv(os.path.join(data_dir, f"{prefix}_trace_bwmon.csv"))
    bus    = pd.read_csv(os.path.join(data_dir, f"{prefix}_trace_bus.csv"))
    icc    = pd.read_csv(os.path.join(data_dir, f"{prefix}_trace_icc.csv"))

    out_dir = os.path.join(data_dir, "traces")
    os.makedirs(out_dir, exist_ok=True)

    icc_dram = icc[icc.node == "ebi"].copy()
    icc_dram["value_MBs"] = icc_dram.agg_kBps / 1e3
    icc_llcc = icc[icc.node == "qns_llcc"].copy()
    icc_llcc["value_MBs"] = icc_llcc.agg_kBps / 1e3

    starts = (ctx[ctx.event == "start"]
              [["t_s", "workload", "ws_mb", "rep"]]
              .rename(columns={"t_s": "t_start"}))
    ends   = (ctx[ctx.event == "end"]
              [["t_s", "workload", "ws_mb", "rep", "bw_alg_MBs"]]
              .rename(columns={"t_s": "t_end"}))
    windows = starts.merge(ends, on=["workload", "ws_mb", "rep"])

    METRICS = [
        ("bwmon",      bwmon,    "bwmon_MBs"),
        ("bus_access", bus,      "bus_access_MBs"),
        ("icc_dram",   icc_dram, "value_MBs"),
        ("icc_llcc",   icc_llcc, "value_MBs"),
    ]

    for wl in sorted(windows.workload.unique()):
        wl_wins = windows[windows.workload == wl]

        for metric, trace, vcol in METRICS:
            rows = []
            for _, win in wl_wins.iterrows():
                seg = trace[(trace.t_s >= win.t_start) & (trace.t_s <= win.t_end)].copy()
                seg = seg.assign(ws_mb=win.ws_mb, rep=win.rep)
                rows.append(seg[["t_s", vcol, "ws_mb", "rep"]]
                             .rename(columns={vcol: "value_MBs"}))
            if rows:
                out = pd.concat(rows, ignore_index=True)
                path = os.path.join(out_dir, f"{wl}_{metric}.csv")
                out.to_csv(path, index=False)
                print(f"  {wl}_{metric}.csv  ({len(out)} rows)")

        # bw_alg: constant within each rep — two rows per rep (start + end timestamps)
        rows = []
        for _, win in wl_wins.iterrows():
            for ts in (win.t_start, win.t_end):
                rows.append({"t_s": ts, "ws_mb": win.ws_mb,
                             "rep": win.rep, "value_MBs": win.bw_alg_MBs})
        out = pd.DataFrame(rows)
        path = os.path.join(out_dir, f"{wl}_bw_alg.csv")
        out.to_csv(path, index=False)
        print(f"  {wl}_bw_alg.csv  ({len(out)} rows)")

    print(f"\nWrote {sum(1 for f in os.listdir(out_dir) if f.endswith('.csv'))} files → {out_dir}")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--data",   default="data/")
    ap.add_argument("--prefix", default="icc_bw")
    args = ap.parse_args()
    split(args.data, args.prefix)
