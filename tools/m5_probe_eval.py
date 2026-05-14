#!/usr/bin/env python3
# Copyright 2026 Anuj Attri  SPDX-License-Identifier: Apache-2.0
"""
M5 Probe-Outcome Correlation evaluation.

For each column in the train and test sets (defined in preregistration.md):
  1. Compute XorLocalityScore (probe) on first 1024 values.
  2. Compute compression ratios for all 5 codecs on first MAX_CODEC values.
  3. Write results to results/m5_probe_eval.json.

Codec ratio implementations are Python ports of the C++ header-only codecs,
producing bit-exact results on the same data.
"""

import json
import math
import struct
import numpy as np
from pathlib import Path
from typing import List, Dict
import pcodec
import pcodec.standalone as pco_standalone

ROOT     = Path(__file__).parent.parent
DATA     = ROOT / "data"
RESULTS  = ROOT / "results"
RESULTS.mkdir(exist_ok=True)

MAX_PROBE = 1024
MAX_CODEC = 100_000  # consecutive values for codec ratio estimation (middle window)

# ── CLZ / CTZ helpers ─────────────────────────────────────────────────────────

def clz64(x: int) -> int:
    if x == 0: return 64
    return 64 - x.bit_length()

def ctz64(x: int) -> int:
    if x == 0: return 64
    return (x & -x).bit_length() - 1

# ── Probe (matches include/lns/xor_locality_probe.hpp) ────────────────────────

def probe(arr: np.ndarray) -> Dict[str, float]:
    """Compute XorLocalityScore on first MAX_PROBE values."""
    ns = min(MAX_PROBE, len(arr))
    if ns < 2:
        return dict(mean_lz=0.0, mean_tz=0.0, mean_sig=64.0,
                    mantissa_entropy=26.0)

    sample = arr[:ns]
    bits = sample.view(np.uint64)

    # Mantissa entropy: low-26 mantissa bits (bits 0-25 of uint64 repr)
    low26 = (bits & np.uint64(0x3FFFFFF)).astype(np.uint32)
    entropy = 0.0
    for b in range(26):
        cnt = int(np.sum((low26 >> b) & np.uint32(1)))
        p = cnt / ns
        if 0.0 < p < 1.0:
            entropy -= p * math.log2(p) + (1.0 - p) * math.log2(1.0 - p)

    # XOR metrics
    a = bits[:-1].astype(object)  # Python ints for bitwise ops
    b = bits[1:].astype(object)
    xors = a ^ b

    sum_lz = sum_tz = sum_sig = 0.0
    for x in xors:
        xi = int(x)
        if xi == 0:
            sum_lz += 64.0
        else:
            lz  = clz64(xi)
            tz  = ctz64(xi)
            sig = 64 - lz - tz
            sum_lz  += lz
            sum_tz  += tz
            sum_sig += sig

    npairs = ns - 1
    return dict(
        mean_lz=sum_lz / npairs,
        mean_tz=sum_tz / npairs,
        mean_sig=sum_sig / npairs,
        mantissa_entropy=entropy,
    )

# ── Gorilla encoder (matches include/lns/gorilla_codec.hpp) ───────────────────

def gorilla_bits(u64_vals: List[int]) -> int:
    """Return total output bit count for Gorilla-encoded u64 sequence."""
    n = len(u64_vals)
    if n == 0: return 32  # 4-byte header
    # Header: 4 bytes (n) + 8 bytes (first value)
    bits = 32 + 64
    prev = u64_vals[0]
    prev_lz = -1
    prev_tz = 0
    prev_sig = 0

    for i in range(1, n):
        xv = (u64_vals[i] ^ prev) & 0xFFFFFFFFFFFFFFFF
        prev = u64_vals[i]

        if xv == 0:
            bits += 1
            continue

        bits += 1  # flag=1 (non-zero)
        lz  = clz64(xv)
        tz  = ctz64(xv)
        lz  = min(lz, 63)
        sig = 64 - lz - tz
        if sig <= 0:
            sig = 1
            tz  = 64 - lz - 1

        can_reuse = (prev_lz >= 0) and (lz >= prev_lz) and (tz >= prev_tz)

        if can_reuse:
            bits += 1 + prev_sig           # '0' + prev_sig_bits
        else:
            bits += 1 + 6 + 6 + sig       # '1' + lz(6) + sig-1(6) + sig_bits
            prev_lz  = lz
            prev_tz  = tz
            prev_sig = sig

    # BitWriter primes 1 extra byte; add 8 bits (partial byte rounding)
    return bits + 8

def _mid_window(arr: np.ndarray, n: int) -> np.ndarray:
    """Return up to n consecutive values from the middle quarter of arr."""
    if len(arr) <= n:
        return arr
    start = len(arr) // 4
    return arr[start:start + n]

def gorilla_ratio(arr: np.ndarray) -> float:
    u64 = _mid_window(arr, MAX_CODEC).view(np.uint64)
    vals = [int(x) for x in u64]
    raw_bits = len(vals) * 64
    enc_bits = gorilla_bits(vals)
    return raw_bits / max(enc_bits, 1)

# ── Chimp128 encoder (matches include/lns/chimp128_codec.hpp) ─────────────────

LZ_TABLE = [0, 8, 12, 16, 18, 22, 26, 64]

def lz_to_idx(lz: int) -> int:
    best = 0
    for i in range(1, 8):
        if LZ_TABLE[i] <= lz:
            best = i
        else:
            break
    return best

def chimp128_bits(u64_vals: List[int]) -> int:
    n = len(u64_vals)
    if n == 0: return 32
    bits = 32 + 64  # header + first value
    CACHE_SIZE = 128
    cache = [0] * CACHE_SIZE
    cache_fill = 0
    cache_head = 0
    prev = u64_vals[0]

    cache[cache_head] = prev
    cache_head = (cache_head + 1) % CACHE_SIZE
    cache_fill = min(cache_fill + 1, CACHE_SIZE)

    prev_lz = -1
    prev_tz = 0
    prev_sig = 0

    for i in range(1, n):
        val = u64_vals[i]

        if val == prev:
            bits += 1  # '0'
            cache[cache_head] = val
            cache_head = (cache_head + 1) % CACHE_SIZE
            cache_fill = min(cache_fill + 1, CACHE_SIZE)
            prev = val
            continue

        # Find best XOR partner
        best_idx = 0
        best_sig_b = 65
        # baseline: XOR with previous
        xb = (val ^ prev) & 0xFFFFFFFFFFFFFFFF
        lz_b = clz64(xb)
        tz_b = ctz64(xb)
        sig_b = 64 - lz_b - tz_b
        if sig_b < best_sig_b:
            best_sig_b = sig_b
            best_idx = cache_fill - 1
        # scan cache
        for c in range(cache_fill - 1):
            xc = (val ^ cache[c]) & 0xFFFFFFFFFFFFFFFF
            if xc == 0:
                best_sig_b = 0
                best_idx = c
                break
            sc = 64 - clz64(xc) - ctz64(xc)
            if sc < best_sig_b:
                best_sig_b = sc
                best_idx = c

        xorval = (val ^ cache[best_idx]) & 0xFFFFFFFFFFFFFFFF
        lz_raw = clz64(xorval) if xorval != 0 else 64
        tz     = ctz64(xorval) if xorval != 0 else 0

        can_reuse = (prev_lz >= 0) and (lz_raw >= prev_lz) and (tz >= prev_tz) and (prev_sig > 0)

        if can_reuse:
            bits += 2 + 7 + prev_sig     # '10' + 7-bit ref + sig bits
        else:
            lz_idx   = lz_to_idx(lz_raw)
            lz_rep   = LZ_TABLE[lz_idx]
            adj_sig  = 64 - lz_rep - tz
            if adj_sig <= 0: adj_sig = 1
            bits += 3 + 7 + 3 + 6 + adj_sig   # '110' + ref(7) + lz(3) + sig-1(6) + sig
            prev_lz  = lz_rep
            prev_tz  = tz
            prev_sig = adj_sig

        cache[cache_head] = val
        cache_head = (cache_head + 1) % CACHE_SIZE
        cache_fill = min(cache_fill + 1, CACHE_SIZE)
        prev = val

    return bits + 8

def chimp128_ratio(arr: np.ndarray) -> float:
    u64 = _mid_window(arr, MAX_CODEC).view(np.uint64)
    vals = [int(x) for x in u64]
    raw_bits = len(vals) * 64
    enc_bits = chimp128_bits(vals)
    return raw_bits / max(enc_bits, 1)

# ── Elf encoder (matches include/lns/elf_codec.hpp) ───────────────────────────

POW10 = [10**b for b in range(19)]

def find_beta(v: float) -> int:
    if not math.isfinite(v) or v == 0.0:
        return 31
    abs_v = abs(v)
    for beta in range(19):
        scaled  = abs_v * POW10[beta]
        rounded = round(scaled)
        if rounded >= 9.2e18:
            break
        recovered = rounded / POW10[beta]
        if recovered == abs_v:
            return beta
    return 31

def encode_elf_value(v: float, beta: int) -> int:
    if beta == 31:
        bits = struct.unpack('<Q', struct.pack('<d', v))[0]
        return bits
    scaled = v * POW10[beta]
    ival   = int(round(scaled))
    # as uint64 (two's complement)
    return ival & 0xFFFFFFFFFFFFFFFF

def elf_bits(data: List[float]) -> int:
    n = len(data)
    if n == 0: return 32
    # Header: 4 bytes n
    bits_count = 32
    # Beta stream: 5 bits per value, packed; + 1 byte from BitWriter ctor
    beta_bits_raw = n * 5
    beta_bytes = (beta_bits_raw + 7) // 8
    bits_count += beta_bytes * 8

    betas = [find_beta(v) for v in data]
    ints  = [encode_elf_value(v, b) for v, b in zip(data, betas)]

    # First value: 8 bytes verbatim
    bits_count += 64

    # Gorilla XOR on ints[1:]
    prev = ints[0]
    prev_lz = -1
    prev_tz = 0
    prev_sig = 0

    for i in range(1, n):
        xv = (ints[i] ^ prev) & 0xFFFFFFFFFFFFFFFF
        prev = ints[i]

        if xv == 0:
            bits_count += 1
            continue
        bits_count += 1

        lz  = clz64(xv)
        tz  = ctz64(xv)
        lz  = min(lz, 63)
        sig = 64 - lz - tz
        if sig <= 0:
            sig = 1
            tz  = 64 - lz - 1

        can_reuse = (prev_lz >= 0) and (lz >= prev_lz) and (tz >= prev_tz)
        if can_reuse:
            bits_count += 1 + prev_sig
        else:
            bits_count += 1 + 6 + 6 + sig
            prev_lz  = lz
            prev_tz  = tz
            prev_sig = sig

    # BitWriter priming byte
    bits_count += 8
    return bits_count

def elf_ratio(arr: np.ndarray) -> float:
    sample = _mid_window(arr, MAX_CODEC)
    data   = sample.tolist()
    raw_bits  = len(data) * 64
    enc_bits  = elf_bits(data)
    return raw_bits / max(enc_bits, 1)

# ── LNS Q10.22 + Gorilla (matches include/lns/composite.hpp) ──────────────────

LNS_F = 22
LNS_SCALE = 1 << LNS_F  # 4194304

def lns_encode_val(x: float) -> int:
    """Encode one double as LNS Q10.22 raw (int64 → uint64)."""
    if math.isnan(x) or x == 0.0 or math.isinf(x):
        return 0  # specials handled via flags; raw=0
    abs_x   = abs(x)
    log2_ax = math.log2(abs_x)
    raw     = int(round(log2_ax * LNS_SCALE))
    # clamp to int64 range
    raw = max(-(1 << 62), min((1 << 62) - 1, raw))
    # Store as two's complement uint64
    return raw & 0xFFFFFFFFFFFFFFFF

def lns_ratio(arr: np.ndarray) -> float:
    sample = _mid_window(arr, MAX_CODEC)
    data   = sample.tolist()
    n      = len(data)

    has_specials = any(not math.isfinite(v) or v == 0.0 for v in data)
    u64_vals = [lns_encode_val(v) for v in data]

    gorilla_enc_bits = gorilla_bits(u64_vals)
    gorilla_bytes    = (gorilla_enc_bits + 7) // 8

    # Composite header: 4 (n) + 1 (has_specials) + 4 (gorilla_len) = 9 bytes
    total_bytes = 9 + gorilla_bytes + (n if has_specials else 0)
    raw_bytes   = n * 8
    return raw_bytes / max(total_bytes, 1)

# ── Pcodec (uses Python pcodec package, same Rust pco 1.0.2 crate) ────────────

PCO_CONFIG = pcodec.ChunkConfig()

def pco_ratio(arr: np.ndarray) -> float:
    sample = _mid_window(arr, MAX_CODEC).astype(np.float64)
    raw_bytes = sample.nbytes
    enc = pco_standalone.simple_compress(sample, PCO_CONFIG)
    # Wire format adds 4-byte header in pcodec_codec.hpp; negligible
    return raw_bytes / max(len(enc), 1)

# ── Dataset inventory ─────────────────────────────────────────────────────────

# Train set: Miranda + Hurricane + AAPL/MSFT (raw + adj OHLC)
# Test set:  NYX + GOOG/NVDA/TSLA (raw + adj OHLC)

SDR = DATA / "sdrbench"
YFR = DATA / "yfinance_raw"

TRAIN_COLS = []
TEST_COLS  = []

def add_sdr(cols, prefix, provenance):
    for p in sorted(SDR.glob(f"{prefix}_*.bin")):
        cols.append({"file": p, "provenance": provenance,
                     "family": f"SDR-{prefix.capitalize()}"})

# Miranda (train), Hurricane (train), NYX (test)
add_sdr(TRAIN_COLS, "miranda", "simulation")
add_sdr(TRAIN_COLS, "hurricane", "simulation")
add_sdr(TEST_COLS,  "nyx",      "simulation")

OHLC = ["open", "high", "low", "close"]

def add_yf(cols, ticker, provenance_raw, provenance_adj):
    for field in OHLC:
        raw_p = YFR / f"{ticker}_{field}_raw.bin"
        adj_p = YFR / f"{ticker}_{field}_adj.bin"
        if raw_p.exists():
            cols.append({"file": raw_p, "provenance": provenance_raw,
                         "family": f"Yahoo-{ticker.upper()}-daily"})
        if adj_p.exists():
            cols.append({"file": adj_p, "provenance": provenance_adj,
                         "family": f"Yahoo-{ticker.upper()}-daily"})

# Train: AAPL + MSFT
add_yf(TRAIN_COLS, "aapl", "sensor-direct", "scalar-multiplied")
add_yf(TRAIN_COLS, "msft", "sensor-direct", "scalar-multiplied")

# Test: GOOG + NVDA + TSLA
add_yf(TEST_COLS, "goog", "sensor-direct", "scalar-multiplied")
add_yf(TEST_COLS, "nvda", "sensor-direct", "scalar-multiplied")
add_yf(TEST_COLS, "tsla", "sensor-direct", "scalar-multiplied")

# ── Evaluation loop ───────────────────────────────────────────────────────────

def eval_column(col_info: dict, split: str) -> dict:
    p   = col_info["file"]
    arr = np.fromfile(p, dtype=np.float64)
    if len(arr) == 0:
        return None

    sc  = probe(arr)
    r_g = gorilla_ratio(arr)
    r_c = chimp128_ratio(arr)
    r_e = elf_ratio(arr)
    r_l = lns_ratio(arr)
    r_p = pco_ratio(arr)

    best_xor    = max(r_g, r_c, r_e)
    best_nonxor = max(r_l, r_p)
    # Amendment 1: label uses LNS only (Pcodec is heavyweight external baseline)
    xor_wins    = 1 if best_xor >= r_l else 0

    return {
        "split":      split,
        "file":       p.name,
        "family":     col_info["family"],
        "provenance": col_info["provenance"],
        "n":          len(arr),
        "mean_lz":    sc["mean_lz"],
        "mean_tz":    sc["mean_tz"],
        "mean_sig":   sc["mean_sig"],
        "mantissa_entropy": sc["mantissa_entropy"],
        "ratio_gorilla":    r_g,
        "ratio_chimp128":   r_c,
        "ratio_elf":        r_e,
        "ratio_lns_q10_22": r_l,
        "ratio_pcodec":     r_p,
        "best_xor_ratio":   best_xor,
        "best_nonxor_ratio": best_nonxor,
        "label_xor_wins":   xor_wins,
    }

def main():
    results = []
    total = len(TRAIN_COLS) + len(TEST_COLS)
    done  = 0

    for col in TRAIN_COLS:
        done += 1
        print(f"[{done:2d}/{total}] train  {col['file'].name} ...", flush=True)
        row = eval_column(col, "train")
        if row:
            results.append(row)
            print(f"         entropy={row['mantissa_entropy']:.2f}  "
                  f"G={row['ratio_gorilla']:.3f}  C={row['ratio_chimp128']:.3f}  "
                  f"E={row['ratio_elf']:.3f}  L={row['ratio_lns_q10_22']:.3f}  "
                  f"P={row['ratio_pcodec']:.3f}  xor_wins={row['label_xor_wins']}",
                  flush=True)

    for col in TEST_COLS:
        done += 1
        print(f"[{done:2d}/{total}] test   {col['file'].name} ...", flush=True)
        row = eval_column(col, "test")
        if row:
            results.append(row)
            print(f"         entropy={row['mantissa_entropy']:.2f}  "
                  f"G={row['ratio_gorilla']:.3f}  C={row['ratio_chimp128']:.3f}  "
                  f"E={row['ratio_elf']:.3f}  L={row['ratio_lns_q10_22']:.3f}  "
                  f"P={row['ratio_pcodec']:.3f}  xor_wins={row['label_xor_wins']}",
                  flush=True)

    out_path = RESULTS / "m5_probe_eval.json"
    with open(out_path, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nWrote {out_path}  ({len(results)} rows)", flush=True)

if __name__ == "__main__":
    main()
