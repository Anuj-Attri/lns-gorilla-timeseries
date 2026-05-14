#!/usr/bin/env bash
# Copyright 2026 Anuj Attri  SPDX-License-Identifier: Apache-2.0
# Reproducibility script for "Detecting Upstream Arithmetic Contamination".
# Reproduces all [measured] claims in the paper from source.
#
# Requirements: Python 3.11+, CMake 3.20+, Ninja, Rust 1.70+, pcodec PyPI package.
# Platform: Linux/macOS (substitute cmake --build / ninja for Windows).
# Data: SDR-Bench archives and Yahoo Finance access (via yfinance) required.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== Step 1: Build Rust pco_wrapper ==="
cd "$ROOT/third_party/pco_wrapper"
cargo build --release

echo "=== Step 2: CMake configure and build ==="
cmake -S "$ROOT" -B "$ROOT/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" -j"$(nproc 2>/dev/null || echo 4)"

echo "=== Step 3: Run unit tests ==="
cd "$ROOT/build"
./tests --reporter compact

echo "=== Step 4: Probe latency benchmark ==="
./bench_probe --benchmark_out="$ROOT/results/bench_probe.json" \
              --benchmark_out_format=json

echo "=== Step 5: Extract SDR-Bench archives ==="
# Download Miranda and NYX from sdrbench.github.io and place in data/sdrbench/
# before running.
python "$ROOT/tools/extract_sdrbench.py"

echo "=== Step 6: Fetch Yahoo Finance data ==="
python - <<'PYEOF'
import yfinance as yf
import numpy as np
from pathlib import Path
OUT = Path("data/yfinance_raw")
OUT.mkdir(parents=True, exist_ok=True)
for ticker in ["AAPL","MSFT","GOOG","NVDA","TSLA"]:
    for adjust in [False, True]:
        suffix = "adj" if adjust else "raw"
        df = yf.download(ticker, start="2010-01-01", end="2025-12-31",
                         auto_adjust=adjust, progress=False)
        for field in ["Open","High","Low","Close"]:
            if field in df.columns:
                arr = df[field].dropna().values.astype(np.float64)
                arr.tofile(OUT / f"{ticker.lower()}_{field.lower()}_{suffix}.bin")
PYEOF

echo "=== Step 7: M5 probe evaluation ==="
python "$ROOT/tools/m5_probe_eval.py"

echo "=== Step 8: Statistical analysis ==="
python - <<'PYEOF'
import json, math, random, statistics
with open("results/m5_probe_eval.json") as f:
    rows = json.load(f)
test = [r for r in rows if r['split']=='test']
def ada(r): return max(r['ratio_gorilla'],r['ratio_chimp128']) if r['mantissa_entropy']<13 else r['ratio_lns_q10_22']
def gm(v): return math.exp(sum(math.log(x) for x in v if x>0)/len([x for x in v if x>0]))
adapt=[ada(r) for r in test]; goril=[r['ratio_gorilla'] for r in test]
print(f"Adaptive geomean: {gm(adapt):.4f}"); print(f"Gorilla  geomean: {gm(goril):.4f}")
print(f"Ratio: {gm(adapt)/gm(goril):.4f}x")
PYEOF

echo "=== All reproducibility steps complete ==="
echo "See results/m5_probe_eval.json for full per-column data."
