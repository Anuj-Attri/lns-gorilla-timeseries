#!/usr/bin/env python3
"""
fetch_yahoo.py — fetch 20 years of daily OHLCV for a basket of tickers from Yahoo Finance.
Saves raw little-endian IEEE 754 f64 binary files to data/yfinance/<TICKER>_<FIELD>.bin

Requires: pip install yfinance numpy
This script is for data ingest only. It is not benchmarked.
"""
import struct
import sys
from pathlib import Path

try:
    import yfinance as yf
    import numpy as np
except ImportError:
    print("Missing dependencies. Install with: pip install yfinance numpy")
    sys.exit(1)

TICKERS = ["AAPL", "MSFT", "GOOG", "AMZN", "META", "NVDA", "TSLA", "JPM", "XOM", "BRK-B"]
FIELDS  = ["Open", "High", "Low", "Close", "Volume"]
PERIOD  = "20y"
OUT_DIR = Path("data/yfinance")

OUT_DIR.mkdir(parents=True, exist_ok=True)

for ticker in TICKERS:
    print(f"Fetching {ticker} …", end=" ", flush=True)
    try:
        df = yf.download(ticker, period=PERIOD, auto_adjust=True, progress=False)
        if df.empty:
            print("empty — skipped")
            continue
        for field in FIELDS:
            if field not in df.columns:
                continue
            col = df[field].dropna().values.astype(np.float64)
            out = OUT_DIR / f"{ticker}_{field}.bin"
            with open(out, "wb") as f:
                f.write(col.tobytes())
            print(f"{field}({len(col)})", end=" ", flush=True)
        print()
    except Exception as e:
        print(f"ERROR: {e}")

# Write provenance file
with open(OUT_DIR / "README.md", "w") as f:
    f.write(f"""# data/yfinance provenance

Source: Yahoo Finance via yfinance Python library (https://github.com/ranaroussi/yfinance)
Tickers: {', '.join(TICKERS)}
Fields: {', '.join(FIELDS)}
Period: {PERIOD} daily OHLCV
Format: raw little-endian IEEE 754 float64, no header
License: Yahoo Finance data is subject to Yahoo's Terms of Service.
         Use for personal/research purposes only. Do not redistribute raw data.

Files named: <TICKER>_<FIELD>.bin
Fetched: see git log for date.
""")

print(f"\nAll done. Files in {OUT_DIR}")
