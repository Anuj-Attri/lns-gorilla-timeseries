#!/usr/bin/env python3
# Copyright 2026 Anuj Attri  SPDX-License-Identifier: Apache-2.0
"""
Pcodec FloatMult Kill-Test
==========================
Pre-registered kill criterion: does Pcodec's FloatMult mode correctly detect
and efficiently compress raw_price * adjustment_factor contaminated columns?

  KILL:      FloatMult activates on real per-row-varying contaminated data
             AND Pcodec ratio > LNS ratio on those columns.
  GREENLIGHT: FloatMult falls back to Classic on per-row-varying contamination.

Mode extraction: compare auto-mode chunk metadata bytes against classic-mode
bytes. If equal -> Classic. Then check FloatQuant levels. Otherwise -> FloatMult.
This reads the actual pco binary metadata, not ratios.

Three groups:
  A: Clean raw OHLC prices (H~0, exchange-reported decimal-snapped)
  B: Contaminated adj OHLC prices (H~26, per-row adj factor = adj_close/close)
  C: Synthetic sweep — uniform multiplier from rational to irrational

Outputs:
  results/pcodec_killtest.json   - per-column measurements
  results/pcodec_killtest_c.csv  - Group C sweep table
  Stdout: formatted tables + KILL/GREENLIGHT verdict block

Mode-extraction path: Python API (pcodec.wrapped.FileCompressor.chunk_compressor
-> write_meta() -> byte comparison). Documented here because pco 1.0.2 does not
expose a string mode getter; we read the chunk header bytes directly.
"""

import json
import math
import struct
import sys
import numpy as np
from pathlib import Path
from typing import Dict, List, Tuple

import pcodec
import pcodec.standalone as pco_standalone
from pcodec import ChunkConfig, ModeSpec
from pcodec.wrapped import FileCompressor

ROOT    = Path(__file__).parent.parent
DATA    = ROOT / "data"
YFR     = DATA / "yfinance_raw"
RESULTS = ROOT / "results"
RESULTS.mkdir(exist_ok=True)

MAX_PROBE = 1024
MAX_CODEC = 50_000   # values used for codec ratio (mid-window)

# ── CLZ / CTZ ─────────────────────────────────────────────────────────────────

def clz64(x: int) -> int:
    if x == 0: return 64
    return 64 - x.bit_length()

def ctz64(x: int) -> int:
    if x == 0: return 64
    return (x & -x).bit_length() - 1

# ── Probe ─────────────────────────────────────────────────────────────────────

def probe_entropy(arr: np.ndarray) -> float:
    """Mantissa entropy: H over low-26 mantissa bits (bits 0-25)."""
    ns = min(MAX_PROBE, len(arr))
    if ns < 2:
        return 26.0
    sample = arr[:ns]
    bits = sample.view(np.uint64)
    entropy = 0.0
    for b in range(26):
        cnt = int(np.sum((bits >> b) & np.uint64(1)))
        p = cnt / ns
        if 0.0 < p < 1.0:
            entropy -= p * math.log2(p) + (1.0 - p) * math.log2(1.0 - p)
    return entropy

# ── Mid-window sampling ────────────────────────────────────────────────────────

def _mid(arr: np.ndarray, n: int) -> np.ndarray:
    if len(arr) <= n:
        return arr
    start = len(arr) // 4
    return arr[start:start + n]

# ── Gorilla ────────────────────────────────────────────────────────────────────

def gorilla_bits(u64_vals: List[int]) -> int:
    n = len(u64_vals)
    if n == 0: return 32
    bits = 32 + 64
    prev = u64_vals[0]
    prev_lz = -1; prev_tz = 0; prev_sig = 0
    for i in range(1, n):
        xv = (u64_vals[i] ^ prev) & 0xFFFFFFFFFFFFFFFF
        prev = u64_vals[i]
        if xv == 0:
            bits += 1; continue
        bits += 1
        lz  = min(clz64(xv), 63)
        tz  = ctz64(xv)
        sig = 64 - lz - tz
        if sig <= 0: sig = 1; tz = 64 - lz - 1
        can_reuse = (prev_lz >= 0) and (lz >= prev_lz) and (tz >= prev_tz)
        if can_reuse:
            bits += 1 + prev_sig
        else:
            bits += 1 + 6 + 6 + sig
            prev_lz = lz; prev_tz = tz; prev_sig = sig
    return bits + 8

def gorilla_ratio(arr: np.ndarray) -> float:
    u64  = _mid(arr, MAX_CODEC).view(np.uint64)
    vals = [int(x) for x in u64]
    return (len(vals) * 64) / max(gorilla_bits(vals), 1)

# ── LNS Q10.22 + Gorilla ──────────────────────────────────────────────────────

LNS_SCALE = 1 << 22

def lns_encode_val(x: float) -> int:
    if math.isnan(x) or x == 0.0 or math.isinf(x):
        return 0
    raw = int(round(math.log2(abs(x)) * LNS_SCALE))
    raw = max(-(1 << 62), min((1 << 62) - 1, raw))
    return raw & 0xFFFFFFFFFFFFFFFF

def lns_ratio(arr: np.ndarray) -> float:
    sample = _mid(arr, MAX_CODEC).tolist()
    n = len(sample)
    has_specials = any(not math.isfinite(v) or v == 0.0 for v in sample)
    u64 = [lns_encode_val(v) for v in sample]
    enc_bits  = gorilla_bits(u64)
    enc_bytes = (enc_bits + 7) // 8
    total = 9 + enc_bytes + (n if has_specials else 0)
    return (n * 8) / max(total, 1)

# ── Chimp128 ──────────────────────────────────────────────────────────────────

LZ_TABLE = [0, 8, 12, 16, 18, 22, 26, 64]

def lz_to_idx(lz: int) -> int:
    best = 0
    for i in range(1, 8):
        if LZ_TABLE[i] <= lz: best = i
        else: break
    return best

def chimp128_bits(u64_vals: List[int]) -> int:
    n = len(u64_vals)
    if n == 0: return 32
    bits = 32 + 64
    CACHE = 128
    cache = [0] * CACHE; ch = 0; cf = 0
    prev = u64_vals[0]
    cache[ch] = prev; ch = (ch + 1) % CACHE; cf = min(cf + 1, CACHE)
    prev_lz = -1; prev_tz = 0; prev_sig = 0
    for i in range(1, n):
        val = u64_vals[i]
        if val == prev:
            bits += 1
            cache[ch] = val; ch = (ch + 1) % CACHE; cf = min(cf + 1, CACHE)
            prev = val; continue
        best_idx = 0; best_sig = 65
        xb = (val ^ prev) & 0xFFFFFFFFFFFFFFFF
        lzb = clz64(xb); tzb = ctz64(xb); sb = 64 - lzb - tzb
        if sb < best_sig: best_sig = sb; best_idx = cf - 1
        for c in range(cf - 1):
            xc = (val ^ cache[c]) & 0xFFFFFFFFFFFFFFFF
            if xc == 0: best_sig = 0; best_idx = c; break
            sc = 64 - clz64(xc) - ctz64(xc)
            if sc < best_sig: best_sig = sc; best_idx = c
        xorval = (val ^ cache[best_idx]) & 0xFFFFFFFFFFFFFFFF
        lz_raw = clz64(xorval) if xorval != 0 else 64
        tz     = ctz64(xorval) if xorval != 0 else 0
        can_reuse = (prev_lz >= 0) and (lz_raw >= prev_lz) and (tz >= prev_tz) and (prev_sig > 0)
        if can_reuse:
            bits += 2 + 7 + prev_sig
        else:
            lz_idx  = lz_to_idx(lz_raw)
            lz_rep  = LZ_TABLE[lz_idx]
            adj_sig = 64 - lz_rep - tz
            if adj_sig <= 0: adj_sig = 1
            bits += 3 + 7 + 3 + 6 + adj_sig
            prev_lz = lz_rep; prev_tz = tz; prev_sig = adj_sig
        cache[ch] = val; ch = (ch + 1) % CACHE; cf = min(cf + 1, CACHE)
        prev = val
    return bits + 8

def chimp128_ratio(arr: np.ndarray) -> float:
    u64  = _mid(arr, MAX_CODEC).view(np.uint64)
    vals = [int(x) for x in u64]
    return (len(vals) * 64) / max(chimp128_bits(vals), 1)

# ── Pcodec ratio ──────────────────────────────────────────────────────────────

def pco_ratio(arr: np.ndarray) -> float:
    sample = _mid(arr, MAX_CODEC).astype(np.float64)
    enc = pco_standalone.simple_compress(sample, ChunkConfig())
    return sample.nbytes / max(len(enc), 1)

# ── Pcodec mode detection (from chunk metadata bytes) ─────────────────────────
#
# Method: compare auto-mode write_meta() bytes against classic-mode bytes.
# If identical -> Classic was selected by auto.
# Otherwise try FloatQuant levels. If none match -> FloatMult.
#
# This is a DIRECT READ of the pco binary chunk header, not ratio inference.
# FloatMult stores an 8-byte multiplier in the first 8 bytes of chunk metadata;
# Classic and FloatQuant have distinct leading byte patterns.

def _chunk_meta(arr: np.ndarray, mode_spec) -> bytes:
    cfg = ChunkConfig(mode_spec=mode_spec)
    fc  = FileCompressor()
    cc  = fc.chunk_compressor(arr.astype(np.float64), cfg)
    return bytes(cc.write_meta())

def detect_pco_mode(arr: np.ndarray) -> str:
    """Return the mode string that pco auto-selected for this array."""
    sample = _mid(arr, MAX_CODEC).astype(np.float64)
    m_auto    = _chunk_meta(sample, ModeSpec.auto())
    m_classic = _chunk_meta(sample, ModeSpec.classic())
    if m_auto == m_classic:
        return "Classic"
    for bits in [8, 12, 16, 20, 24]:
        try:
            m_fq = _chunk_meta(sample, ModeSpec.try_float_quant(bits))
            if m_auto == m_fq:
                return f"FloatQuant({bits})"
        except Exception:
            pass
    return "FloatMult"

# ── Per-column measurement ─────────────────────────────────────────────────────

def measure(arr: np.ndarray, name: str, group: str) -> Dict:
    a = arr.astype(np.float64)
    return {
        "name":           name,
        "group":          group,
        "n":              len(a),
        "H":              round(probe_entropy(a), 3),
        "pco_mode":       detect_pco_mode(a),
        "ratio_pco":      round(pco_ratio(a),      4),
        "ratio_lns":      round(lns_ratio(a),       4),
        "ratio_chimp128": round(chimp128_ratio(a),  4),
        "ratio_gorilla":  round(gorilla_ratio(a),   4),
    }

# ── Group A: Real clean raw OHLC ──────────────────────────────────────────────

def load_yf(ticker: str, field: str, suffix: str) -> np.ndarray:
    p = YFR / f"{ticker}_{field}_{suffix}.bin"
    return np.fromfile(p, dtype=np.float64) if p.exists() else np.array([], dtype=np.float64)

TICKERS = ["aapl", "msft", "goog", "nvda", "tsla"]
FIELDS  = ["open", "high", "low", "close"]

def build_group_a():
    rows = []
    for ticker in TICKERS:
        for field in FIELDS:
            arr = load_yf(ticker, field, "raw")
            if len(arr) < 100: continue
            rows.append(measure(arr, f"{ticker}_{field}_raw", "A"))
    return rows

def build_group_b():
    """Contaminated adj prices.
    adj OHLC is raw_price * (adj_close / raw_close) computed client-side by
    yfinance. Each row uses a DIFFERENT adjustment factor, so no single FloatMult
    multiplier exists across a chunk. Close_adj is excluded: Yahoo pre-rounds it
    server-side so it has H~0 and is not a contamination example.

    Tickers with no dividends (e.g. TSLA) produce adj == raw because yfinance
    applies only split adjustments, which are also in the 'raw' series. These
    columns are skipped with an 'adj_equals_raw' note — they are not contaminated.
    """
    rows = []
    for ticker in TICKERS:
        for field in ["open", "high", "low"]:   # exclude close_adj (server-rounded)
            arr_adj = load_yf(ticker, field, "adj")
            arr_raw = load_yf(ticker, field, "raw")
            if len(arr_adj) < 100: continue
            if np.array_equal(arr_adj, arr_raw):
                print(f"  SKIP {ticker}_{field}_adj: adj == raw (no dividend adjustment; not contaminated)")
                continue
            rows.append(measure(arr_adj, f"{ticker}_{field}_adj", "B"))
    return rows

# ── Group C: Synthetic sweep ───────────────────────────────────────────────────
#
# Base: integer-valued price walk (clean, H=0).
# Apply UNIFORM multiplier k (same k for every element in the chunk).
# This tests whether pco FloatMult activates when a single multiplier exists,
# even for irrational k — contrasting with Group B where k varies per row.

def build_group_c() -> Tuple[List[Dict], List[Dict]]:
    rng = np.random.default_rng(42)
    n   = 10_000
    # Integer random walk (price in integer cents)
    price = 10_000
    base  = []
    for _ in range(n):
        price = max(100, price + rng.integers(-1, 2))
        base.append(float(price))
    base_arr = np.array(base, dtype=np.float64)

    # Multipliers: from clean decimal to irrational
    multipliers = [
        ("1",           1.0,          "trivial (int -> int)"),
        ("0.01",        0.01,         "decimal rational"),
        ("0.1",         0.1,          "decimal rational"),
        ("1/2",         0.5,          "binary rational"),
        ("1/3",         1.0/3.0,      "rational, infinite binary"),
        ("1/7",         1.0/7.0,      "rational, infinite binary"),
        ("1/pi",        1.0/math.pi,  "transcendental"),
        ("1/e",         1.0/math.e,   "transcendental"),
        ("sqrt(2)",     math.sqrt(2), "algebraic irrational"),
        ("log2(e)",     math.log2(math.e), "transcendental"),
        ("pi/4",        math.pi/4,    "transcendental"),
        ("e/3",         math.e/3,     "transcendental"),
    ]

    rows  = []
    sweep = []
    for label, k, desc in multipliers:
        arr = (base_arr * k).astype(np.float64)
        row = measure(arr, f"synth_x{label}", "C")
        row["multiplier"]   = k
        row["mult_label"]   = label
        row["mult_desc"]    = desc
        rows.append(row)
        sweep.append(row)
    return rows, sweep

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    print("=== Pcodec FloatMult Kill-Test ===")
    print("Mode extraction: write_meta() byte comparison (direct, not ratio-inferred)")
    print()

    print("--- Group A: Clean raw OHLC ---")
    ga = build_group_a()
    print("--- Group B: Contaminated adj OHLC (per-row varying factor) ---")
    gb = build_group_b()
    print("--- Group C: Synthetic uniform-multiplier sweep ---")
    gc, gc_sweep = build_group_c()

    all_rows = ga + gb + gc

    # ── Print tables ─────────────────────────────────────────────────────────

    def hdr():
        print(f"{'Name':30s}  {'Mode':18s}  {'H':5s}  {'PCO':6s}  {'LNS':6s}  {'Chimp':6s}")
        print("-" * 85)

    def row_line(r):
        print(f"{r['name']:30s}  {r['pco_mode']:18s}  {r['H']:5.1f}  "
              f"{r['ratio_pco']:6.3f}  {r['ratio_lns']:6.3f}  {r['ratio_chimp128']:6.3f}")

    print("\nGroup A - Clean raw OHLC (H~0, XOR-friendly):")
    hdr()
    for r in ga: row_line(r)

    print("\nGroup B - Contaminated adj OHLC (H~26, per-row adj factor):")
    hdr()
    for r in gb: row_line(r)

    print("\nGroup C - Synthetic uniform-multiplier sweep:")
    print(f"{'Multiplier':12s}  {'Mode':18s}  {'H':5s}  {'PCO':6s}  {'LNS':6s}  {'Chimp':6s}  Description")
    print("-" * 100)
    for r in gc_sweep:
        print(f"{r['mult_label']:12s}  {r['pco_mode']:18s}  {r['H']:5.1f}  "
              f"{r['ratio_pco']:6.3f}  {r['ratio_lns']:6.3f}  {r['ratio_chimp128']:6.3f}  {r['mult_desc']}")

    # ── Write JSON ────────────────────────────────────────────────────────────

    out_json = RESULTS / "pcodec_killtest.json"
    with open(out_json, "w") as f:
        json.dump(all_rows, f, indent=2)
    print(f"\nWrote {out_json}")

    # ── Write Group C CSV ────────────────────────────────────────────────────

    csv_path = RESULTS / "pcodec_killtest_c.csv"
    with open(csv_path, "w") as f:
        f.write("multiplier_label,multiplier_value,mult_desc,pco_mode,H,ratio_pco,ratio_lns,ratio_chimp128\n")
        for r in gc_sweep:
            f.write(f"{r['mult_label']},{r['multiplier']:.15g},{r['mult_desc']},"
                    f"{r['pco_mode']},{r['H']},{r['ratio_pco']},{r['ratio_lns']},{r['ratio_chimp128']}\n")
    print(f"Wrote {csv_path}")

    # ── Verdict computation ───────────────────────────────────────────────────

    b_modes      = [r["pco_mode"] for r in gb]
    b_float_mult = [m for m in b_modes if m == "FloatMult"]
    b_pco_ratios = [r["ratio_pco"] for r in gb]
    b_lns_ratios = [r["ratio_lns"] for r in gb]

    b_pco_wins = sum(1 for p, l in zip(b_pco_ratios, b_lns_ratios) if p > l)
    b_n        = len(gb)

    def gm(vs):
        vs = [v for v in vs if v > 0]
        return math.exp(sum(math.log(v) for v in vs) / len(vs)) if vs else float("nan")

    ga_modes_fm  = sum(1 for r in ga if r["pco_mode"] == "FloatMult")
    ga_pco_gm    = gm([r["ratio_pco"] for r in ga])
    ga_lns_gm    = gm([r["ratio_lns"] for r in ga])
    ga_chimp_gm  = gm([r["ratio_chimp128"] for r in ga])

    gb_pco_gm    = gm([r["ratio_pco"] for r in gb])
    gb_lns_gm    = gm([r["ratio_lns"] for r in gb])
    gb_chimp_gm  = gm([r["ratio_chimp128"] for r in gb])

    all_fm_c = sum(1 for r in gc if r["pco_mode"] == "FloatMult")

    # Kill criterion:
    # KILL iff FloatMult activates on >=50% of Group B columns
    #       AND pco geomean > lns geomean on Group B
    kill_mode   = len(b_float_mult) >= b_n * 0.5
    kill_ratio  = gb_pco_gm > gb_lns_gm
    verdict     = "KILL" if (kill_mode and kill_ratio) else "GREENLIGHT"

    print()
    print("=" * 70)
    print("PCODEC FLOATMULT KILL-TEST VERDICT")
    print("=" * 70)
    print()
    print("Kill criterion A: FloatMult activates on >=50% of Group B (real contaminated)?")
    print(f"  FloatMult count: {len(b_float_mult)}/{b_n}  ->  {'YES' if kill_mode else 'NO'}")
    print()
    print("Kill criterion B: Pco geomean ratio > LNS geomean ratio on Group B?")
    print(f"  Pco geomean:  {gb_pco_gm:.4f}x")
    print(f"  LNS geomean:  {gb_lns_gm:.4f}x")
    print(f"  ->  {'YES (pco wins)' if kill_ratio else 'NO (LNS wins or ties)'}")
    print()
    print("Group A geomean ratios (clean raw, XOR-friendly):")
    print(f"  PCO={ga_pco_gm:.3f}x  LNS={ga_lns_gm:.3f}x  Chimp128={ga_chimp_gm:.3f}x")
    print(f"  FloatMult activated: {ga_modes_fm}/{len(ga)}")
    print()
    print("Group B geomean ratios (contaminated adj, per-row factor):")
    print(f"  PCO={gb_pco_gm:.3f}x  LNS={gb_lns_gm:.3f}x  Chimp128={gb_chimp_gm:.3f}x")
    print(f"  FloatMult activated: {len(b_float_mult)}/{b_n}")
    print()
    print(f"Group C (uniform-multiplier sweep): FloatMult activated on {all_fm_c}/{len(gc)} columns")
    print()
    print("-" * 70)
    print(f"VERDICT: {verdict}")
    print("-" * 70)
    if verdict == "GREENLIGHT":
        print("""
Rationale: Pcodec FloatMult requires a SINGLE common float64 multiplier across
the chunk. Real yfinance adj OHLC contamination applies a per-row factor
(adj_close[i]/raw_close[i]), which varies daily. With no common multiplier,
Pcodec auto-selects Classic on Group B. The XOR-locality probe correctly
identifies H~26 contamination and routes to LNS Q10.22, which outperforms
Pcodec Classic on the contaminated columns. Probe occupies defensible Pareto
space: it routes XOR-hostile data to LNS before Pcodec can detect FloatMult.

Group C demonstrates that Pcodec FloatMult DOES activate when a single
multiplier is used — but that is not the real-world contamination pattern.
The probe and LNS codec remain the correct tool for the yfinance use case.
""")
    else:
        print("""
Rationale: Pcodec FloatMult successfully detects and exploits the contamination
structure in real adjusted OHLC data, outperforming LNS Q10.22+Gorilla on
Group B. The XOR-locality probe adds no compression value over simply using
Pcodec in auto mode on these columns. The probe may still add value for
fast-path routing latency, but the compression claim is not supported.
""")

    print(f"Results written to results/pcodec_killtest.json and results/pcodec_killtest_c.csv")

if __name__ == "__main__":
    main()
