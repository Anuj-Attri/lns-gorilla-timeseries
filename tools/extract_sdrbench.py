#!/usr/bin/env python3
# Copyright 2026 Anuj Attri  SPDX-License-Identifier: Apache-2.0
"""
Extract float fields from SDR-Bench Miranda and NYX archives.
Reads the first MAX_VALUES values per field, converts to float64,
saves as little-endian float64 .bin files in data/sdrbench/.
"""
import struct, tarfile, numpy as np, sys
from pathlib import Path

ROOT     = Path(__file__).parent.parent
SDR_DIR  = ROOT / "data" / "sdrbench"
MAX_VALS = 5_000_000  # first 5M values per field

def extract_archive(archive_path, fields, out_prefix, dtype_in):
    """Extract fields from a .tar.gz archive to out_prefix_<field>.bin files."""
    print(f"Opening {archive_path.name} ...", flush=True)
    with tarfile.open(archive_path, "r:gz") as tf:
        for member in tf.getmembers():
            fname = Path(member.name).name
            ext   = Path(fname).suffix.lower()
            if ext not in ('.f32', '.d64', '.f64'):
                continue
            stem = Path(fname).stem
            out_path = SDR_DIR / f"{out_prefix}_{stem}.bin"
            if out_path.exists():
                print(f"  skip {stem} (already exists)", flush=True)
                continue
            print(f"  extracting {stem} ...", end=" ", flush=True)
            f = tf.extractfile(member)
            if f is None:
                print("SKIP (no data)")
                continue
            raw = f.read(MAX_VALS * dtype_in.itemsize)
            arr = np.frombuffer(raw, dtype=dtype_in)[:MAX_VALS].astype(np.float64)
            # Report stats
            n_neg = int(np.sum(arr < 0))
            n_nan = int(np.sum(~np.isfinite(arr)))
            print(f"{len(arr)} values, neg={n_neg}, nonfinite={n_nan}", flush=True)
            arr.tofile(out_path)
            print(f"    -> {out_path.name}", flush=True)

def main():
    SDR_DIR.mkdir(parents=True, exist_ok=True)

    # Miranda: float64 little-endian (.d64)
    miranda_arc = SDR_DIR / "SDRBENCH-Miranda-256x384x384.tar.gz"
    if miranda_arc.exists():
        extract_archive(miranda_arc, None, "miranda", np.dtype('<f8'))
    else:
        print("Miranda archive not found; skipping.", flush=True)

    # NYX: float32 little-endian (.f32)
    nyx_arc = SDR_DIR / "NYX-512x512x512.tar.gz"
    if nyx_arc.exists():
        extract_archive(nyx_arc, None, "nyx", np.dtype('<f4'))
    else:
        print("NYX archive not found; skipping.", flush=True)

    print("Done.", flush=True)

if __name__ == "__main__":
    main()
