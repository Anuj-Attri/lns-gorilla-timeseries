#!/usr/bin/env python3
"""
fetch_intraday.py — fetch 30 days of 1-minute OHLCV bars from Yahoo Finance.
Saves raw little-endian IEEE 754 f64 binary files to data/yfinance_1m/<TICKER>_<FIELD>.bin

Note: yfinance limits 1-minute data to the last 30 calendar days.
Requires: pip install yfinance numpy
"""
import sys
from pathlib import Path

try:
    import yfinance as yf
    import numpy as np
except ImportError:
    print("Missing dependencies. Install with: pip install yfinance numpy")
    sys.exit(1)

TICKERS = ["SPY", "QQQ", "AAPL"]
FIELDS  = ["Open", "High", "Low", "Close", "Volume"]
OUT_DIR = Path("data/yfinance_1m")

# Yahoo Finance limits 1m data to 8 days per request.
# Fetch in 7-day batches going back up to 30 days, combine.
import datetime, pandas as pd

OUT_DIR.mkdir(parents=True, exist_ok=True)

for ticker in TICKERS:
    print(f"Fetching {ticker} 1m (batched 7d x4) ...", flush=True)
    frames = []
    today = datetime.date.today()
    for batch in range(4):  # 4 batches of 7 days = ~28 days
        end   = today - datetime.timedelta(days=batch * 7)
        start = end   - datetime.timedelta(days=7)
        try:
            df = yf.download(ticker, start=start.isoformat(), end=end.isoformat(),
                             interval="1m", auto_adjust=True, progress=False)
            if not df.empty:
                frames.append(df)
                print(f"  batch {batch}: {len(df)} bars ({start} to {end})")
        except Exception as e:
            print(f"  batch {batch} ERROR: {e}")

    if not frames:
        print(f"  {ticker}: no data fetched — skipped")
        continue

    df = pd.concat(frames).sort_index().drop_duplicates()
    print(f"  {ticker}: total {len(df)} 1m bars")

    for field in FIELDS:
        if field not in df.columns:
            continue
        col = df[field].dropna().values.astype(np.float64)
        out = OUT_DIR / f"{ticker}_{field}.bin"
        with open(out, "wb") as f:
            f.write(col.tobytes())
        print(f"  saved {field}({len(col)})")

with open(OUT_DIR / "README.md", "w") as f:
    f.write(f"""# data/yfinance_1m provenance

Source: Yahoo Finance via yfinance Python library (https://github.com/ranaroussi/yfinance)
Tickers: {', '.join(TICKERS)}
Fields: {', '.join(FIELDS)}
Period: last 30 calendar days, 1-minute bars
Format: raw little-endian IEEE 754 float64, no header
License: Yahoo Finance data is subject to Yahoo's Terms of Service.
         Use for personal/research purposes only. Do not redistribute raw data.

Files named: <TICKER>_<FIELD>.bin
Fetched: see git log for date.
""")

print(f"\nAll done. Files in {OUT_DIR}")
