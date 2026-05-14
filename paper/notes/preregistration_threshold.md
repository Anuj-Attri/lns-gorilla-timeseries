# Pre-Registered Threshold τ*

**Filed:** 2026-05-14 (after train-set evaluation, before test-set evaluation)  
**Threshold:** τ* = **13.0** (mantissa_entropy_bits)

## Fitting procedure (train set, 26 columns)

Since the probe score is effectively bimodal — columns cluster at entropy ≈ 0.0 (XOR-friendly)
or entropy ≈ 24–26 (XOR-hostile) with no intermediate values — any threshold τ ∈ (0, 24)
produces the same confusion matrix. The midpoint τ* = 13.0 was chosen.

**Train confusion matrix at τ* = 13.0 (primary label: max(XOR) ≥ LNS):**

| | Pred=1 (XOR-friendly) | Pred=0 (XOR-hostile) |
|---|---|---|
| Actual=1 | TP = 11 | FN = 0 |
| Actual=0 | FP = 3  | TN = 12 |

- Precision = 0.786, Recall = 1.000, F1 = 0.880
- Train AUC = **0.90** [above 0.85 gate]

## False positives on train (entropy=0 but LNS wins):

| Column | G ratio | LNS ratio | Margin |
|---|---|---|---|
| miranda_density | 1.166 | 63.888 | LNS dominates (boundary artifact: near-constant density≈1.0 compresses better in log-space) |
| hurricane_temperature | 1.790 | 1.837 | LNS margin < 3% |
| msft_close_adj | 2.368 | 2.485 | LNS margin < 5%; adj_close is server-rounded (entropy=0) |

*SHA of this commit is cited in §Evaluation of the paper as the pre-test threshold commitment.*
