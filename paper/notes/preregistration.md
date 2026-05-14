# Pre-Registration: Probe-Outcome Correlation (M5) and Adaptive Selector (M6)

**Filed:** 2026-05-14  
**Status:** LOCKED — do not edit after first commit; all results computed after this SHA.

---

## 1. Hypothesis

The XOR-locality probe's `mantissa_entropy_bits` score (Shannon H of the
low-26 mantissa bits, measured over `min(1024, n)` values) is a reliable
binary classifier of whether a codec router should prefer a Gorilla-family
codec (Gorilla, Chimp128, Elf) or a non-XOR codec (LNS, Pcodec) for maximum
compression ratio on a given float column.

**Directional prediction:** columns with `mantissa_entropy_bits < 13` (halfway
between 0 and 26) are XOR-friendly; columns with `mantissa_entropy_bits ≥ 13`
are XOR-hostile. The probe will achieve AUC ≥ 0.85 as a binary classifier
(best codec is XOR vs. best codec is non-XOR) on the **held-out test set**.

---

## 2. Datasets and Train/Test Split

**FIXED BEFORE ANY EVALUATION.** No column appears in both sets.

### Train set (threshold fitting only)

| Source | Columns |
|---|---|
| SDR Miranda (7 fields) | simulation; XOR-locality unknown a priori |
| SDR Hurricane (3 fields) | simulation |
| Yahoo AAPL daily (raw + adj, OHLC) | 8 columns |
| Yahoo MSFT daily (raw + adj, OHLC) | 8 columns |

Train total: 26 columns.

### Test set (evaluation — do not inspect before threshold is committed)

| Source | Columns |
|---|---|
| SDR NYX (6 fields) | simulation |
| Yahoo GOOG daily (raw + adj, OHLC) | 8 columns |
| Yahoo NVDA daily (raw + adj, OHLC) | 8 columns |
| Yahoo TSLA daily (raw + adj, OHLC) | 8 columns |

Test total: 30 columns.

---

## 3. Codec Suite

Five codecs evaluated on every column; all compiled from the **same build
directory** with the **same flags** (`-O3 -march=native -DNDEBUG -ffast-math`
on Linux; `/O2 /arch:AVX2 /DNDEBUG /GL` on MSVC):

1. **LNS** (Q10.22 fixed-point log₂)
2. **Gorilla** (Pelkonen et al. 2015, 6-bit leading-zero field)
3. **Chimp128** (ring-buffer XOR, 128-entry, variable-length prefix)
4. **Elf** (find_beta → int64 → Gorilla XOR)
5. **Pcodec** (Rust `pco 1.0.2`, latent-integer detector)

**Compression ratio** = `(raw bytes) / (compressed bytes)` = `n×8 / |encoded|`.

---

## 4. Probe Threshold Fitting (Train Set Only)

1. Run probe on every train column → `mantissa_entropy_bits` score.
2. Run all 5 codecs on every train column → `best_codec` (highest ratio), `best_xor` (highest ratio among Gorilla/Chimp128/Elf), `best_nonxor` (highest ratio among LNS/Pcodec).
3. Label each column: `label = 1` if `best_xor_ratio ≥ best_nonxor_ratio`, else `label = 0`.
4. Fit optimal binary threshold `τ*` on train set: `τ* = argmax_τ F1(label, predict(entropy < τ))`.
5. Commit `τ*` to `paper/notes/preregistration_threshold.md` **before** opening the test set.

---

## 5. Test-Set Evaluation (M5 — after threshold commit)

1. Run probe on every test column.
2. Predict: `pred = 1` if `mantissa_entropy_bits < τ*`, else `pred = 0`.
3. Compare to ground-truth `label` from codec runs.
4. Report: AUC (ROC), precision, recall, F1 at `τ*`.
5. **Gate:** AUC ≥ 0.85.  If AUC < 0.85, report the actual value; do not adjust `τ*`.

---

## 6. Adaptive Selector Evaluation (M6 — after M5)

The adaptive selector applies `τ*` at runtime:
- `if mantissa_entropy_bits < τ*` → route to best-XOR codec from train-set winner
- else → route to best-nonXOR codec from train-set winner

**Headline metric:** geometric mean compression ratio of adaptive selector vs.
single-best-fixed codec (fixed = whichever single codec wins most columns on
the full 56-column set).

**Gate:** adaptive selector geometric mean ≥ 1.10 × single-best-fixed geometric mean.

---

## 7. Statistical Tests (M8)

- Wilcoxon signed-rank test on per-column ratio pairs (adaptive vs. fixed).
- Bonferroni correction for 5 codec comparisons; α = 0.05 / 5 = 0.01.
- 95 % bootstrap CI (10 000 resamples) on geometric mean ratio.
- Cohen's d for effect size.
- All tests two-sided.

---

*This document is the pre-registration record. The SHA of the commit that
introduces it is cited in §5 (Evaluation) of the paper.*
