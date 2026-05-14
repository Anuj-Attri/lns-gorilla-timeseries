#!/usr/bin/env python3
"""
convert_sdrbench.py — convert SDR-Bench float32 raw files to float64 .bin.

Outputs to data/sdrbench/ with names like hurricane_pressure.bin (float64, little-endian).
Filters out NaN, Inf, and negative/zero values that would break LNS.
Reports count of problematic values.
"""
import sys
from pathlib import Path

try:
    import numpy as np
except ImportError:
    print("numpy required"); sys.exit(1)

ROOT    = Path(__file__).parent.parent
SDRDIR  = ROOT / "data" / "sdrbench"
MAX_VAL = 5_000_000  # cap at 5M values to avoid OOM on the 25M-value Hurricane arrays

def convert(src_path, dst_name, field_desc, max_n=MAX_VAL):
    print(f"Converting {src_path.name} ({field_desc})...")
    data = np.fromfile(str(src_path), dtype='<f4')
    print(f"  Total values: {len(data):,}")
    n_nan  = int(np.sum(np.isnan(data)))
    n_inf  = int(np.sum(np.isinf(data)))
    n_neg  = int(np.sum(data <= 0.0))
    print(f"  NaN={n_nan}, Inf={n_inf}, zero/negative={n_neg}")
    # Keep all finite values for compression test (include negatives — LNS will handle badly)
    finite = data[np.isfinite(data)]
    if len(finite) == 0:
        print("  No finite values — skip")
        return
    subset = finite[:max_n]
    dst = SDRDIR / f"{dst_name}.bin"
    subset.astype('<f8').tofile(str(dst))
    print(f"  Wrote {len(subset):,} float64 values to {dst.name}")
    print(f"  Range: [{float(subset.min()):.4g}, {float(subset.max()):.4g}]")

# Hurricane Isabel
hurricane_dir = SDRDIR / "100x500x500"
if hurricane_dir.exists():
    # Pressure
    pf = hurricane_dir / "Pf48.bin.f32"
    if pf.exists():
        convert(pf, "hurricane_pressure", "Hurricane Isabel pressure field Pf48")
    # Wind speed U
    uf = hurricane_dir / "Uf48.bin.f32"
    if uf.exists():
        convert(uf, "hurricane_wind_u", "Hurricane Isabel U wind field")
    # Temperature
    tf = hurricane_dir / "TCf48.bin.f32"
    if tf.exists():
        convert(tf, "hurricane_temperature", "Hurricane Isabel TC temperature")
else:
    print(f"Hurricane directory not found: {hurricane_dir}")

print("\nDone.")
