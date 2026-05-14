#!/usr/bin/env python3
"""
analyze_real.py — batch compression sweep + statistical analysis on real data.

Strategy:
  1. Split every .bin into 1024-value windows, write all windows into ONE temp dir.
  2. Run ratio_sweep.exe ONCE on that dir → single CSV with all windows.
  3. Parse CSV, group by ticker/field, compute per-ticker median ratios.
  4. Wilcoxon signed-rank (pooled windows): LNS-Q8.24 vs Gorilla.
  5. Cross-ticker sign test and meta-analysis.

Outputs:
  results/real_data_<datetime>.csv
  results/real_data_<datetime>_summary.txt
"""
import sys, os, subprocess, tempfile, datetime
from pathlib import Path
from collections import defaultdict

try:
    import numpy as np
    from scipy.stats import wilcoxon, binomtest
except ImportError:
    print("numpy and scipy required: pip install numpy scipy"); sys.exit(1)

ROOT    = Path(__file__).parent.parent
EXE     = ROOT / "build" / "ratio_sweep.exe"
RESULTS = ROOT / "results"
RESULTS.mkdir(exist_ok=True)

DAILY_DIR = ROOT / "data" / "yfinance"
INTRA_DIR = ROOT / "data" / "yfinance_1m"
SCI_DIR   = ROOT / "data" / "sdrbench"
WINDOW    = 1024

CODECS = ["IEEE_baseline", "Gorilla", "LNS_Q8_24_Gorilla",
          "LNS_Q10_22_Gorilla", "LNS_Q12_20_Gorilla", "LNS_Q12_16_Gorilla"]

def load_bin(path):
    data = path.read_bytes()
    n = len(data) // 8
    return np.frombuffer(data[:n*8], dtype='<f8').copy()

def write_bin(path, arr):
    with open(path, 'wb') as f:
        f.write(arr.astype('<f8').tobytes())

def parse_sweep_csv(csv_path):
    """Return dict[codec][dataset] -> (ratio, rmse, max_rel)."""
    result = defaultdict(dict)
    with open(csv_path) as f:
        f.readline()  # skip header
        for line in f:
            parts = line.strip().split(',')
            if len(parts) < 6: continue
            codec, ds, n, ratio, rmse, mre = parts[:6]
            result[codec][ds] = (float(ratio), float(rmse), float(mre))
    return result

def collect_windows(data_dirs):
    """
    Returns:
      windows: list of (tag, group, window_idx, arr)
      tags: dict tag -> (group, total_n)

    Tags are prefixed with a directory slug to avoid collisions
    (e.g. AAPL_Close appears in both data/yfinance/ and data/yfinance_1m/).
    """
    windows = []
    tags = {}
    for data_dir, group in data_dirs:
        if not data_dir.exists():
            continue
        dir_slug = data_dir.name.replace("-", "_")  # e.g. "yfinance", "yfinance_1m", "sdrbench"
        for f in sorted(data_dir.glob("*.bin")):
            stem = f.stem
            tag  = f"{dir_slug}__{stem}"  # unique: "yfinance__AAPL_Close" vs "yfinance_1m__AAPL_Close"
            arr  = load_bin(f)
            if len(arr) < WINDOW:
                print(f"  SKIP {tag}: only {len(arr)} values")
                continue
            tags[tag] = (group, len(arr), stem)  # keep original stem for display
            for i, start in enumerate(range(0, len(arr) - WINDOW + 1, WINDOW)):
                windows.append((tag, group, i, arr[start:start+WINDOW]))
    return windows, tags

def run_sweep(windows):
    """Write all windows to a temp dir, run ratio_sweep.exe once, return parsed CSV."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        tag_to_wfiles = defaultdict(list)
        for tag, group, idx, arr in windows:
            # Sanitize tag for use as a filename stem: replace non-alphanumeric with _
            safe_tag = tag.replace("-", "_").replace(" ", "_")
            fname = f"{safe_tag}_w{idx:04d}"
            write_bin(tmp / f"{fname}.bin", arr)
            tag_to_wfiles[tag].append(fname)

        print(f"Running ratio_sweep.exe on {len(windows)} window files...")
        result = subprocess.run([str(EXE), str(tmp)],
                                capture_output=True, text=True, cwd=str(ROOT))
        if result.returncode != 0:
            print("ratio_sweep stderr:", result.stderr[:500])
        csv_path = ROOT / "results" / "ratio_sweep.csv"
        if not csv_path.exists():
            print("ERROR: results/ratio_sweep.csv not produced")
            return {}, {}
        parsed = parse_sweep_csv(csv_path)
        return parsed, tag_to_wfiles

def per_ticker_ratios(parsed, tag_to_wfiles):
    """
    Returns dict tag -> dict codec -> list_of_per_window_ratios
    """
    ticker_ratios = defaultdict(lambda: defaultdict(list))
    for tag, wfiles in tag_to_wfiles.items():
        for codec in CODECS:
            for wf in wfiles:
                if codec in parsed and wf in parsed[codec]:
                    r, rmse, mre = parsed[codec][wf]
                    ticker_ratios[tag][codec].append(r)
    return ticker_ratios

def bootstrap_median_ci(arr, n_boot=5000, seed=42):
    rng = np.random.default_rng(seed)
    n = len(arr)
    medians = [np.median(rng.choice(arr, size=n, replace=True)) for _ in range(n_boot)]
    return float(np.percentile(medians, 2.5)), float(np.percentile(medians, 97.5))

def cohens_d_list(a, b):
    a, b = np.array(a), np.array(b)
    n = min(len(a), len(b))
    if n < 2: return 0.0
    pooled_std = np.sqrt((np.var(a[:n], ddof=1) + np.var(b[:n], ddof=1)) / 2)
    if pooled_std == 0: return 0.0
    return float(abs(np.mean(a[:n]) - np.mean(b[:n])) / pooled_std)

# ── Main ──────────────────────────────────────────────────────────────────────

print("=== Real-data compression analysis ===\n")

data_dirs = [(DAILY_DIR, "daily"), (INTRA_DIR, "intraday"), (SCI_DIR, "sdrbench")]
windows, tags = collect_windows(data_dirs)
print(f"Total windows: {len(windows)} from {len(tags)} files\n")

parsed, tag_to_wfiles = run_sweep(windows)

if not parsed:
    print("No results from ratio_sweep — aborting."); sys.exit(1)

ticker_ratios = per_ticker_ratios(parsed, tag_to_wfiles)

# ── Build results table ───────────────────────────────────────────────────────

rows = []
for tag in sorted(ticker_ratios.keys()):
    group  = tags[tag][0]
    n_vals = tags[tag][1]
    stem   = tags[tag][2]   # original filename stem for display
    cr     = ticker_ratios[tag]

    gorilla_wins = cr.get("Gorilla", [])
    lns_wins     = cr.get("LNS_Q8_24_Gorilla", [])
    if not gorilla_wins or not lns_wins:
        continue

    n = min(len(gorilla_wins), len(lns_wins))
    g_arr = np.array(gorilla_wins[:n])
    l_arr = np.array(lns_wins[:n])

    g_med = float(np.median(g_arr))
    l_med = float(np.median(l_arr))
    direction = "LNS>Gorilla" if l_med > g_med else "Gorilla>=LNS"

    # Wilcoxon (only meaningful with n >= 3)
    if n >= 3:
        try:
            stat, p_val = wilcoxon(l_arr, g_arr, alternative='greater')
        except Exception:
            stat, p_val = float('nan'), float('nan')
    else:
        stat, p_val = float('nan'), float('nan')

    d = cohens_d_list(l_arr, g_arr)
    if n >= 2:
        ci_lo, ci_hi = bootstrap_median_ci(l_arr / np.maximum(g_arr, 1e-9))
    else:
        ci_lo, ci_hi = float('nan'), float('nan')

    all_meds = {c: float(np.median(cr.get(c, [float('nan')]))) for c in CODECS}

    rows.append({
        "tag": tag, "stem": stem, "group": group, "n_values": n_vals, "n_windows": n,
        "gorilla_median": g_med, "lns_q8_median": l_med,
        "direction": direction,
        "wilcoxon_stat": stat, "p_value": p_val,
        "cohens_d": d, "ci_lo": ci_lo, "ci_hi": ci_hi,
        "all_meds": all_meds,
    })

# ── Write CSV ─────────────────────────────────────────────────────────────────

ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
csv_out = RESULTS / f"real_data_{ts}.csv"
with open(csv_out, "w") as f:
    f.write("dataset,group,n_values,n_windows,codec,ratio_median\n")
    for r in rows:
        for codec, med in r["all_meds"].items():
            f.write(f"{r['stem']},{r['group']},{r['n_values']},{r['n_windows']},{codec},{med:.4f}\n")
print(f"Wrote {csv_out}\n")

# ── Print headline table ──────────────────────────────────────────────────────

print(f"{'Dataset':<30} {'G':<10} {'N':>6} {'Win':>4} {'Gorilla':>9} {'LNS-Q8.24':>10} {'Dir':<14} {'p-val':>10} {'d':>5}")
print("-" * 110)
for r in rows:
    pv = f"{r['p_value']:.2e}" if not np.isnan(r['p_value']) else "n/a"
    cd = f"{r['cohens_d']:.2f}" if not np.isnan(r['cohens_d']) else "n/a"
    print(f"{r['stem']:<30} {r['group']:<10} {r['n_values']:>6,} {r['n_windows']:>4} "
          f"{r['gorilla_median']:>9.3f} {r['lns_q8_median']:>10.3f} {r['direction']:<14} "
          f"{pv:>10} {cd:>5}")

# ── H1: Daily close-price cross-ticker analysis ───────────────────────────────

print("\n=== H1 Cross-ticker meta-analysis (Close, daily) ===\n")
close_rows = [r for r in rows if r["group"] == "daily" and r["stem"].endswith("_Close")]
close_rows.sort(key=lambda r: r["stem"])

for r in close_rows:
    pv = f"{r['p_value']:.2e}" if not np.isnan(r['p_value']) else "(n<3)"
    print(f"  {r['stem']:<22} Gorilla={r['gorilla_median']:.4f}  LNS-Q8.24={r['lns_q8_median']:.4f}"
          f"  {r['direction']}  p={pv}")

n_favor = sum(1 for r in close_rows if r["direction"] == "LNS>Gorilla")
n_total = len(close_rows)
g_meds = [r["gorilla_median"]  for r in close_rows]
l_meds = [r["lns_q8_median"]   for r in close_rows]
overall_g = float(np.median(g_meds)) if g_meds else float('nan')
overall_l = float(np.median(l_meds)) if l_meds else float('nan')

print(f"\nCross-ticker summary:")
print(f"  Tickers analysed: {n_total}")
print(f"  Median Gorilla ratio: {overall_g:.4f}")
print(f"  Median LNS-Q8.24 ratio: {overall_l:.4f}")
print(f"  Tickers where LNS > Gorilla: {n_favor}/{n_total}")

# Pooled Wilcoxon across all close-price windows from all tickers
all_g_wins  = []
all_l_wins  = []
for r in close_rows:
    cr = ticker_ratios[r["tag"]]
    g = cr.get("Gorilla", [])
    l = cr.get("LNS_Q8_24_Gorilla", [])
    n = min(len(g), len(l))
    all_g_wins.extend(g[:n])
    all_l_wins.extend(l[:n])

all_g_arr = np.array(all_g_wins)
all_l_arr = np.array(all_l_wins)
print(f"  Pooled windows across all close tickers: {len(all_g_arr)}")

if len(all_g_arr) >= 3:
    try:
        pooled_stat, pooled_p = wilcoxon(all_l_arr, all_g_arr, alternative='greater')
        print(f"  Pooled Wilcoxon stat={pooled_stat:.1f}, p={pooled_p:.4e}")
    except Exception as e:
        pooled_p = float('nan')
        print(f"  Pooled Wilcoxon failed: {e}")
else:
    pooled_p = float('nan')
    print("  Too few windows for pooled Wilcoxon")

# Sign test
if n_total > 0:
    bt = binomtest(n_favor, n_total, 0.5, alternative='greater')
    print(f"  Sign test: p={bt.pvalue:.4e} ({n_favor}/{n_total} favor LNS)")
    sign_p = bt.pvalue
else:
    sign_p = float('nan')

# H1 verdict
h1_ratio_ok  = overall_l >= 1.8
h1_dir_ok    = (n_favor == n_total)
h1_p_ok      = (not np.isnan(pooled_p)) and pooled_p < 0.001
print(f"\n  H1 checks:")
print(f"    Median LNS >= 1.8x: {overall_l:.4f} -> {'PASS' if h1_ratio_ok else 'FAIL'}")
print(f"    All {n_total} tickers favor LNS: {'PASS' if h1_dir_ok else f'FAIL ({n_favor}/{n_total})'}")
print(f"    Pooled Wilcoxon p < 0.001: p={pooled_p:.2e} -> {'PASS' if h1_p_ok else 'FAIL/NA'}")
h1_pass = h1_ratio_ok and h1_dir_ok
print(f"  H1: {'CONFIRMED' if h1_pass else 'NOT CONFIRMED'}")

# ── H2: Intraday close ────────────────────────────────────────────────────────

print("\n=== H2 Intraday regime (Close, 1-min) ===\n")
intra_close = [r for r in rows if r["group"] == "intraday" and r["stem"].endswith("_Close")]
if intra_close:
    for r in intra_close:
        print(f"  {r['stem']:<22} Gorilla={r['gorilla_median']:.4f}  LNS-Q8.24={r['lns_q8_median']:.4f}  {r['direction']}")
    intra_l_med = float(np.median([r["lns_q8_median"] for r in intra_close]))
    intra_dir   = sum(1 for r in intra_close if r["direction"] == "LNS>Gorilla")
    print(f"\n  Median LNS-Q8.24 intraday: {intra_l_med:.4f}")
    print(f"  Tickers with LNS > Gorilla: {intra_dir}/{len(intra_close)}")
    h2_pass = intra_l_med >= 1.5
    print(f"  H2 (>= 1.5x): {'CONFIRMED' if h2_pass else 'NOT CONFIRMED'}")
else:
    print("  No intraday close data available — H2 not testable")
    h2_pass = False

# ── H3: SDR-Bench ─────────────────────────────────────────────────────────────

print("\n=== H3 SDR-Bench scientific data ===\n")
sci_rows = [r for r in rows if r["group"] == "sdrbench"]
if sci_rows:
    for r in sci_rows:
        print(f"  {r['stem']:<22} Gorilla={r['gorilla_median']:.4f}  LNS-Q8.24={r['lns_q8_median']:.4f}  {r['direction']}")
    n_sci_pass = sum(1 for r in sci_rows if r["lns_q8_median"] >= 1.5)
    print(f"\n  Arrays with LNS >= 1.5x: {n_sci_pass}/{len(sci_rows)}")
    h3_pass = n_sci_pass >= 2
    print(f"  H3 (>= 2 of 3): {'CONFIRMED' if h3_pass else 'NOT CONFIRMED'}")
else:
    print("  SDR-Bench data unavailable (download failed) — H3 not testable")
    h3_pass = None

# ── Verdict ───────────────────────────────────────────────────────────────────

print("\n" + "=" * 60)
print("FINAL VERDICT")
print("=" * 60)
if h1_pass and h2_pass:
    verdict = "H1 CONFIRMED, H2 CONFIRMED -> PAPER-WORTHY"
    next_step = "Next: ALP comparison, SIMD LNS, ClickHouse codec PR."
elif h1_pass:
    verdict = "H1 CONFIRMED, H2 NOT CONFIRMED -> STRONG TECH NOTE"
    next_step = "Intraday regime requires different approach or analysis."
else:
    verdict = "H1 NOT CONFIRMED -> HONEST NEGATIVE RESULT"
    next_step = "Real markets violate synthetic assumptions -- document the mechanism."
print(f"\n**{verdict}**")
print(f"{next_step}\n")
h3_str = "CONFIRMED" if h3_pass else ("NOT CONFIRMED" if h3_pass is False else "NOT TESTABLE (download failed)")
print(f"H3 (SDR-Bench): {h3_str}")

# ── Failure modes ─────────────────────────────────────────────────────────────

print("\n=== Regression / failure modes ===")
fail_rows = [r for r in rows if r["direction"] != "LNS>Gorilla"]
if fail_rows:
    print(f"\n{'Dataset':<32} {'Group':<10} {'Gorilla':>9} {'LNS':>9}  Diagnosis")
    print("-" * 80)
    for r in fail_rows:
        print(f"{r['stem']:<32} {r['group']:<10} {r['gorilla_median']:>9.3f} {r['lns_q8_median']:>9.3f}  LNS underperforms")
else:
    print("\nNo regression cases — LNS >= Gorilla on all datasets.")

# ── Save summary ──────────────────────────────────────────────────────────────

summary_path = RESULTS / f"real_data_{ts}_summary.txt"
with open(summary_path, "w") as f:
    f.write(f"VERDICT: {verdict}\n")
    f.write(f"H1: {'CONFIRMED' if h1_pass else 'NOT CONFIRMED'}\n")
    f.write(f"H2: {'CONFIRMED' if h2_pass else 'NOT CONFIRMED'}\n")
    f.write(f"H3: {h3_str}\n\n")
    f.write("Daily close-price results:\n")
    for r in close_rows:
        f.write(f"  {r['stem']}: Gorilla={r['gorilla_median']:.4f} LNS-Q8.24={r['lns_q8_median']:.4f} {r['direction']}\n")
    f.write("\nIntraday close results:\n")
    for r in (intra_close if intra_close else []):
        f.write(f"  {r['stem']}: Gorilla={r['gorilla_median']:.4f} LNS-Q8.24={r['lns_q8_median']:.4f} {r['direction']}\n")
    f.write("\nAll codec medians:\n")
    for r in rows:
        f.write(f"\n{r['stem']} ({r['group']}):\n")
        for c, v in r["all_meds"].items():
            f.write(f"  {c}: {v:.4f}\n")

print(f"\nSummary -> {summary_path}")
print(f"CSV     -> {csv_out}")
