# lns-gorilla-timeseries

**Hypothesis:** For multiplicative time-series (stock prices, sensor exponentials), converting to log-number-system (LNS) fixed-point before applying Gorilla XOR compression achieves ≥3:1 compression, versus Gorilla's ~1:1 baseline on raw IEEE 754 data.

**Why it might work:** Multiplicative data has high-entropy IEEE 754 mantissas. In log space, consecutive multiplicative steps become small additive deltas. XOR of consecutive LNS values then has many leading/trailing zeros — exactly what Gorilla exploits.

**Baseline:** Raw IEEE 754 double (8 bytes/value = 1.0× ratio by definition). ALP included as competitive baseline.

**This is a reproducible benchmark, not a production library.** Results are honest: negative results are reported as-is.

## Reproducing

```bash
# Prerequisites: CMake 3.20+, C++17 compiler, Python 3 + yfinance
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Fetch real data (requires internet)
python tools/fetch_yahoo.py

# Download SDRBench subsets (optional, links in data/README.md)
# Generate synthetic data
./build/tools/gen_synthetic

# Run benchmark
./build/bench/bench_codecs --benchmark_out=results/bench_$(date +%Y%m%d).csv --benchmark_out_format=csv

# Statistical significance
./build/bench/stat_sig results/bench_latest.csv
```

## Results

See [RESULTS.md](RESULTS.md) for the full benchmark report.

## Structure

```
include/lns/   — header-only codecs
src/           — codec implementations
bench/         — Google Benchmark harness + stat tests
tests/         — Catch2 unit tests
tools/         — data fetch + synthetic generation
data/          — gitignored binary datasets
results/       — benchmark CSVs
```

## Dependencies (vendored via CMake FetchContent)

- [google/benchmark](https://github.com/google/benchmark)
- [catchorg/Catch2](https://github.com/catchorg/Catch2) v3
- [cwida/ALP](https://github.com/cwida/ALP)

## Citation

If you use this software or reproduce results from it, please cite:

```bibtex
@software{attri2026lnsgorilla,
  author  = {Attri, Anuj},
  title   = {{LNS-Gorilla}: Log-Number-System preprocessing for
             {Gorilla} {XOR} compression on multiplicative time-series},
  year    = {2026},
  version = {0.1.0},
  url     = {https://github.com/anujattri01/lns-gorilla-timeseries},
  license = {Apache-2.0}
}
```

See [CITATION.cff](CITATION.cff) for the full citation metadata.

## License

**Source code** (`include/`, `src/`, `bench/`, `tests/`, `tools/`, `CMakeLists.txt`):
Apache License 2.0 — see [LICENSE](LICENSE).

**Benchmark results** (`RESULTS.md`, `POST_DRAFT.md`):
Creative Commons Attribution 4.0 International (CC BY 4.0) — see [LICENSE-RESULTS.md](LICENSE-RESULTS.md).

Attribution is required for both. See [NOTICE](NOTICE) for full requirements.
