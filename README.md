# Detecting Upstream Arithmetic Contamination in Financial Timeseries

**Research artifact for** *"Detecting Upstream Arithmetic Contamination in Floating-Point Financial Time-Series via XOR-Locality Probing"* — ICDE 2027 submission.

---

## The Problem

Financial data providers compute adjusted prices client-side:

```
adj_high[t] = raw_high[t] × (adj_close[t] / raw_close[t])
```

The adjustment factor changes every day. Multiplying a decimal-snapped price by an irrational-in-binary factor fills all 52 mantissa bits with pseudo-random noise — destroying the XOR locality that Gorilla-family codecs depend on.

**The smoking gun (test set columns):**

| Column | Mantissa entropy H | Gorilla ratio | LNS Q10.22 ratio |
|--------|-------------------|---------------|------------------|
| GOOG raw Open  | 0.0 bits | 2.356× | 2.403× |
| GOOG adj Open  | **26.0 bits** | 1.138× | **2.402×** |
| NVDA raw Close | 0.0 bits | 2.392× | 2.463× |
| NVDA adj Open  | **26.0 bits** | 1.144× | **2.453×** |

H = 0 → exchange-reported, decimal-snapped. H = 26 → upstream scalar multiply applied per-row.

---

## The Probe

The **XOR-locality probe** measures the marginal Shannon entropy of the low-26 mantissa bits over a 1024-value window:

```
H = Σ_{b=0}^{25}  H(p_b)    where  p_b = fraction of values with bit b set
```

Cost: O(min(1024, n)) per encode call; ≈2 µs on 1024 doubles.

```
  H distribution across 56 columns (train + test sets)
  ┌─────────────────────────────────────────────────────┐
  │ H ≈ 0  ████████████████████████  XOR-friendly (33) │
  │ H ≈ 26 ██████████████            XOR-hostile  (18) │
  │ H mixed (SDR-Bench simulation)    ████████     (5) │
  └─────────────────────────────────────────────────────┘
  Threshold τ* = 13.0  (pre-registered SHA 303dce0, fitted on train set)
```

---

## Adaptive Routing

```
                         ┌──────────────────┐
  encode(data, n) ──────►│  XOR-locality    │
                         │  probe           │
                         │  O(min(1024, n)) │
                         └────────┬─────────┘
                                  │
                    ┌─────────────┴─────────────┐
                H < τ*=13                   H ≥ τ*=13
            (XOR-friendly)             (XOR-hostile)
                    │                           │
                    ▼                           ▼
            ┌───────────────┐       ┌──────────────────────┐
            │   Chimp128    │       │  LNS Q10.22 + Gorilla│
            │  lossless     │       │  lossy  (ε < 10⁻⁶)  │
            │  XOR codec    │       │  log-space XOR codec │
            └───────────────┘       └──────────────────────┘

  Wire format: [1-byte codec-id] [codec payload]
```

---

## Results — Test Set (30 columns, pre-registered split)

### Codec geomean compression ratio

| Codec | Geomean | vs Gorilla |
|-------|---------|------------|
| Gorilla | 1.809× | baseline |
| Chimp128 | 1.681× | −7.1% |
| Elf | 0.943× | −47.9% |
| **LNS Q10.22** | **1.940×** | **+7.2%** |
| **Adaptive (probe → route)** | **2.082×** | **+15.1%** |
| Pcodec (reference) | 2.596× | different architecture |

### By data family

```
  Compression ratio by family (test set geomean)
  ─────────────────────────────────────────────────────────────
  SDR-Nyx simulation (H≈0)
    Gorilla  1.88×  ████████████████████
    LNS      1.68×  ██████████████████

  Yahoo adj OHL (H≈26, contaminated)
    Gorilla  1.14×  ████████████
    LNS      2.40×  ████████████████████████████

  Yahoo raw OHLC (H≈0, clean decimal)
    Gorilla  2.35×  ████████████████████████
    LNS      2.39×  ████████████████████████
  ─────────────────────────────────────────────────────────────
  Each █ ≈ 0.1×
```

XOR-hostile columns (H≈26): Gorilla loses 40% of its ratio vs XOR-friendly data. LNS holds steady.

### Statistical significance (M8, pre-registered protocol)

| Test | Statistic | p-value |
|------|-----------|---------|
| Wilcoxon signed-rank (LNS vs best-XOR, 30 pairs) | W⁺=105, z=−3.296 | **p=0.0010** |
| Bootstrap 95% CI on geomean ratio | [1.062, 1.268] | excludes 1.0 |
| Cohen's d (log-ratio scale) | 0.629 | medium-large |
| Bonferroni α (5 comparisons) | 0.05/5 = 0.01 | — |

---

## Pcodec FloatMult Kill Test

**Kill criterion:** if Pcodec FloatMult activates on real per-row-varying contaminated data *and* beats LNS, the probe adds no value → KILL.

**Mode extraction:** `write_meta()` chunk header bytes compared between `ModeSpec.auto()` and `ModeSpec.classic()`. Classic produces identical bytes. FloatMult encodes the detected multiplier in the first 8 bytes of the header. Not inferred from ratios.

### Why FloatMult does not fire on real adj data

```
  Synthetic (same multiplier k every row):
    price[0] = 100 × k
    price[1] = 101 × k       ← common factor k → pco detects FloatMult ✓
    price[2] = 102 × k

  Real yfinance adj OHLC (factor changes daily):
    adj[0] = raw[0] × (adj_close[0] / raw_close[0])
    adj[1] = raw[1] × (adj_close[1] / raw_close[1])   ← different factor each row
    adj[2] = raw[2] × (adj_close[2] / raw_close[2])
             → no common multiplier → pco falls back to Classic ✗
```

### Kill test results (actual yfinance data, mode read from chunk metadata)

| Group | n cols | H | Pco mode | PCO geomean | LNS geomean |
|-------|--------|---|----------|-------------|-------------|
| A — clean raw OHLC | 20 | 0.0 | FloatMult 20/20 | **3.163×** | 2.175× |
| B — contam adj OHLC | 12 | 26.0 | **Classic 12/12** | 1.307× | **2.187×** |
| C — synthetic uniform k | 12 | 0–26 | FloatMult 12/12 | **38.8×** | 5.5× |

**VERDICT: GREENLIGHT** — FloatMult never activates on real contaminated data (0/12). LNS outperforms Pcodec Classic by +67% on Group B.

---

## Honest Limitations

| Limitation | Detail |
|------------|--------|
| AUC gate failed | Pre-registered gate ≥ 0.85; measured 0.625 on test set. The probe separates H≈0 from H≈26 with perfect recall but weak within-class discrimination. Disclosed in paper §5.4. |
| LNS is lossy | Relative error < 10⁻⁶ for prices in [2⁻¹⁰, 2²⁴]. Specials (0, ±∞, NaN) round-trip exactly via flag byte. |
| Pcodec dominates on clean data | 3.16× vs Chimp128 2.28× vs LNS 2.18×. The adaptive codec does not use Pcodec as a route target. |
| Platform | All measurements on Windows 11, MinGW GCC 15.2, AVX2. Linux/macOS builds use same flags. |

---

## Pre-Registration Record

| Artifact | SHA | Content |
|----------|-----|---------|
| `paper/notes/preregistration.md` | `823fba9` | Train/test split, codec suite, AUC gate, label definition |
| `paper/notes/preregistration_threshold.md` | `303dce0` | τ* = 13.0, fitted on train set only |
| `paper/notes/preregistration_amendment_1.md` | `303dce0` | Middle-window sampling fix; Pcodec excluded from label |

All committed before test-set evaluation. Release tags: `v0.2.0-preregistered`, `v0.3.0-paper-submitted`, `v0.3.1-pcodec-killtest`.

---

## Reproducing

```bash
# Prerequisites: CMake 3.20+, Ninja, C++17 compiler, Rust 1.70+, Python 3.11+
# pcodec PyPI package required: pip install pcodec yfinance numpy

# 1. Build Rust wrapper
cd third_party/pco_wrapper && cargo build --release && cd ../..

# 2. Build C++ codecs and benchmarks
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 3. Unit tests
cd build && ./tests --reporter compact && cd ..

# 4. Data (place Miranda + NYX archives in data/sdrbench/ first)
python tools/extract_sdrbench.py

# 5. Probe + codec evaluation
python tools/m5_probe_eval.py           # → results/m5_probe_eval.json

# 6. Pcodec kill test
python tools/pcodec_floatmult_killtest.py  # → results/pcodec_killtest.json

# Or run the full end-to-end pipeline:
bash paper/reproduce.sh
```

---

## Repository Structure

```
include/lns/
  xor_locality_probe.hpp   — O(1024) mantissa entropy probe
  gorilla_codec.hpp        — Gorilla XOR, 6-bit LZ field
  chimp128_codec.hpp       — Chimp128 with 128-entry ring cache
  elf_codec.hpp            — Elf: decimal snapping + Gorilla
  composite.hpp            — LNS Q10.22 + Gorilla pipeline
  adaptive_codec.hpp       — probe → route → encode/decode

src/                       — codec implementations
third_party/pco_wrapper/   — Rust C ABI wrapper (pco 1.0.2)
bench/                     — Google Benchmark harnesses
tests/                     — Catch2 unit tests (7 suites)
tools/                     — Python evaluation scripts
  m5_probe_eval.py         — probe + 5-codec evaluation
  pcodec_floatmult_killtest.py — kill-test: does FloatMult fire on real data?
paper/                     — LaTeX source + pre-registration notes
results/                   — JSON outputs (tracked)
data/                      — gitignored binary datasets
```

---

## Citation

```bibtex
@inproceedings{attri2027contamination,
  author    = {Attri, Anuj},
  title     = {Detecting Upstream Arithmetic Contamination in Floating-Point
               Financial Time-Series via {XOR}-Locality Probing},
  booktitle = {Proceedings of the 43rd IEEE International Conference on
               Data Engineering (ICDE)},
  year      = {2027},
  note      = {Under review}
}
```

See [CITATION.cff](CITATION.cff) for full metadata.

---

## License

Source code (`include/`, `src/`, `bench/`, `tests/`, `tools/`, `CMakeLists.txt`): **Apache 2.0** — see [LICENSE](LICENSE).

Results (`RESULTS.md`, `results/`): **CC BY 4.0** — see [LICENSE-RESULTS.md](LICENSE-RESULTS.md).

See [NOTICE](NOTICE) for attribution requirements.
