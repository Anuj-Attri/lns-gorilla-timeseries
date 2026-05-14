# Dataset Provenance

Each column is classified by the arithmetic operation applied **upstream** of
storage — before compression.  The classification predicts XOR-locality:
columns produced by scalar-multiplicative operations destroy the mantissa
structure that Gorilla-family codecs depend on.

## Provenance classes

| Class | Description | XOR-locality |
|---|---|---|
| `simulation` | Direct floating-point output of a PDE/N-body solver; no post-processing | High for smooth fields; low for turbulent |
| `sensor-direct` | Exchange or instrument reports a decimal value; stored as the nearest IEEE-754 double | High (low mantissa entropy) |
| `scalar-multiplied` | Raw value × irrational ratio (adj_factor, calibration, unit conversion) | Low (near-maximum mantissa entropy) |
| `server-rounded` | Vendor pre-rounds the result before delivery (Yahoo Adj Close) | High |
| `accumulated` | Running sum / cumulative product; locality depends on increment magnitude | Moderate |

---

## Family 1 — SDR-Bench: Miranda turbulence (simulation)

Archive: `data/sdrbench/SDRBENCH-Miranda-256x384x384.tar.gz`  
Source: <https://sdrbench.github.io>, Rundergan et al. 2019  
Grid: 256 × 384 × 384, float64 little-endian  
Extracted: first 5 000 000 values per field

| File | Field | Provenance | Notes |
|---|---|---|---|
| `miranda_pressure.bin` | pressure | `simulation` | ~54 % negative values |
| `miranda_density.bin` | density | `simulation` | all positive |
| `miranda_viscocity.bin` | viscocity | `simulation` | ~50 % negative |
| `miranda_diffusivity.bin` | diffusivity | `simulation` | ~50 % negative |
| `miranda_velocityx.bin` | velocity X | `simulation` | ~62 % negative |
| `miranda_velocityy.bin` | velocity Y | `simulation` | ~43 % negative |
| `miranda_velocityz.bin` | velocity Z | `simulation` | ~52 % negative |

**Column count: 7**

---

## Family 2 — SDR-Bench: NYX cosmological simulation (simulation)

Archive: `data/sdrbench/NYX-512x512x512.tar.gz`  
Source: <https://sdrbench.github.io>, Almgren et al. 2013  
Grid: 512 × 512 × 512, float32 little-endian (stored as float64 after extraction)  
Extracted: first 5 000 000 values per field

| File | Field | Provenance | Notes |
|---|---|---|---|
| `nyx_dark_matter_density.bin` | dark matter density | `simulation` | all positive, wide dynamic range |
| `nyx_velocity_x.bin` | velocity X | `simulation` | mostly positive |
| `nyx_velocity_y.bin` | velocity Y | `simulation` | ~47 % negative |
| `nyx_velocity_z.bin` | velocity Z | `simulation` | ~51 % negative |
| `nyx_temperature.bin` | temperature | `simulation` | all positive |
| `nyx_baryon_density.bin` | baryon density | `simulation` | all positive |

**Column count: 6**

---

## Family 3 — SDR-Bench: Hurricane Isabel (simulation)

Source: NCAR/CISL Research Data Archive, SDRBENCH re-distribution  
Extracted previously from `SDRBENCH-Hurricane-100x500x500.tar.gz`

| File | Field | Provenance | Notes |
|---|---|---|---|
| `hurricane_pressure.bin` | pressure | `simulation` | |
| `hurricane_temperature.bin` | temperature | `simulation` | |
| `hurricane_wind_u.bin` | wind U component | `simulation` | |

**Column count: 3**

---

## Family 4 — Yahoo Finance: raw vs adjusted OHLCV (sensor-direct / scalar-multiplied)

**Smoking-gun motivating example for paper §1.**

Downloaded via `yfinance` for AAPL, MSFT, GOOG, NVDA, TSLA,  
2010-01-01 – 2025-12-31, daily frequency.

`auto_adjust=False` → raw exchange-reported prices.  
`auto_adjust=True`  → yfinance computes `H_adj = H_raw × (adj_close / close)`,
`O_adj = O_raw × (adj_close / close)` client-side for High/Open/Low;  
Yahoo pre-rounds and delivers `Close_adj` server-side.

**Measured mantissa entropy (Shannon H of low-26 mantissa bits, first 1024 values):**

| Column class | Entropy (bits) | Explanation |
|---|---|---|
| Raw OHLCV | 0.000 | Exchange quotes in whole cents; Elf finds β in {0,1,2} for every value |
| Adj High / Open / Low | ~25.98 | Multiplied by irrational ratio `adj_close/close`; all 26 bits randomized |
| Adj Close | 0.000 | Yahoo Finance rounds adjusted close server-side before delivery |

Directory: `data/yfinance_raw/`

| File pattern | Provenance | XOR-locality |
|---|---|---|
| `{ticker}_{field}_raw.bin` (OHLCV) | `sensor-direct` | High |
| `{ticker}_{high,open,low}_adj.bin` | `scalar-multiplied` | Low |
| `{ticker}_close_adj.bin` | `server-rounded` | High |
| `{ticker}_volume_{raw,adj}.bin` | `accumulated` | Moderate |

Tickers × fields: 5 tickers × 4 float OHLC × 2 (raw + adj) = **40 columns**

---

## Family 5 — Yahoo Finance: intraday 1-minute (sensor-direct)

Directory: `data/yfinance_1m/`  
Tickers: AAPL, MSFT, GOOG  
Fields: Open, High, Low, Close, Volume  
Provenance: `sensor-direct` for OHLC (no split/dividend adjustment applied intraday)

**Column count: 15**

---

## Summary

| Family | Dataset | Columns | Provenance classes |
|---|---|---|---|
| 1 | Miranda | 7 | simulation |
| 2 | NYX | 6 | simulation |
| 3 | Hurricane | 3 | simulation |
| 4 | Yahoo Finance daily (raw + adj) | 40 | sensor-direct, scalar-multiplied, server-rounded, accumulated |
| 5 | Yahoo Finance 1-minute | 15 | sensor-direct |
| **Total** | | **71** | **5 classes** |

**Gate M4:** ≥ 4 dataset families with ≥ 20 labeled float columns — **PASS** (5 families, 71 columns).
