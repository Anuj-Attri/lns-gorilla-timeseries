# Post Draft

## Option A — Technical (Twitter/X, ~280 chars)

Curious experiment: stock prices compress 2–3× better with log preprocessing before Gorilla XOR (vs ~1.1× for raw IEEE 754). The key insight: consecutive log-domain values XOR with 40+ leading zeros. Full results + C++ benchmark: [repo link]

---

## Option B — LinkedIn / longer

**Does log-number-system preprocessing help Gorilla compression on multiplicative time-series?**

Gorilla XOR (Pelkonen et al., VLDB 2015) is designed for smooth time-series data. On stock prices and geometric random walks, it achieves only ~1.09–1.15× — barely better than raw storage.

**The hypothesis:** Convert doubles to fixed-point log₂ (LNS) first. Consecutive prices differ by a small multiplicative factor; in log space this becomes a small additive delta. XOR of consecutive LNS values has 40+ leading zeros → exactly what Gorilla exploits.

**Measured result (1M synthetic values, geometric Brownian motion):**

- Gorilla on raw doubles: 1.09–1.15× (close to uncompressed)
- LNS Q8.24 + Gorilla: **2.1–2.2×** at 2.1e-8 max relative error
- LNS Q12.16 + Gorilla: **2.8–3.0×** at 5.3e-6 max relative error

Encode throughput: ~73–82 M values/s (vs Gorilla's 84 M) — ~15% overhead from the log transform.

**Where it breaks:** High-volatility data (σ=0.1) causes LNS saturation. Near-zero crossings in additive data cause ~10% regression vs Gorilla. These failure modes are in the results.

Reproducible C++ benchmark (CMake, Catch2, Google Benchmark): [repo link]

---

*Calibration notes:*
- "2–3×" refers to Q8.24–Q12.16 on low-to-moderate volatility multiplicative data [measured]
- This is a lossy codec; error bounds are quantified and tested
- ALP baseline not included (requires Clang; build uses GCC)
- No production deployment — this is a research benchmark
