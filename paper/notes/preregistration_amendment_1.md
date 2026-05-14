# Pre-Registration Amendment 1

**Filed:** 2026-05-14  
**Amends:** paper/notes/preregistration.md (commit 823fba9)  
**Reason:** Two empirical discoveries required corrections before threshold fitting.

---

## Discovery 1: SDR Boundary Artifact

When M5 was run with `MAX_CODEC=100_000` (first 100K values), Miranda `density.bin`'s
first 100,000 values are identically **1.0** (the simulation's boundary/ghost-cell region).
This gave Pcodec a ratio of 27586x on that slice vs 5.73x on the full 5M-value field —
a clear sampling artifact. Confirmed by inspecting `arr[:100000].std() == 0.0`.

**Amendment:** All codec ratio estimates now use a **middle window** of the data:
`arr[len(arr)//4 : len(arr)//4 + MAX_CODEC]` rather than `arr[:MAX_CODEC]`.
This avoids grid boundary effects. For short columns (len ≤ MAX_CODEC), all values are used.

---

## Discovery 2: Pcodec Dominates Both Classes

With the original label definition — `label = 1 if max(G,C,E) >= max(LNS, Pcodec)` — all
56 columns have `label = 0` because Pcodec achieves higher ratios than any XOR-family codec
on every column tested, regardless of mantissa entropy. The label is degenerate (one class
only); AUC is undefined.

**Root cause of label design flaw:** Pcodec has its own internal latent-structure detector
and subsumes both XOR codecs and LNS+Gorilla. Including it in the "non-XOR baseline"
makes the XOR-vs-non-XOR discrimination trivially impossible.

**Paper contribution clarification:** The probe's role is to route between two **lightweight**
streaming codecs — Gorilla (XOR-family, O(n) encode) vs LNS+Gorilla (log-domain, O(n) encode)
— without paying the cost of Pcodec's internal classifier. Pcodec is relegated to the
"heavyweight external baseline" comparison in §Evaluation.

**Amendment:** The label is redefined as:

    label = 1 if max(G_ratio, Chimp128_ratio, Elf_ratio) >= LNS_Q10_22_ratio
    label = 0 otherwise

Pcodec ratios are still recorded for Table 2 (full codec comparison) but are **not** used
in the primary AUC gate.

---

## Revised label distribution (all 56 columns, middle-window sampling)

Expected split after amendment:
- entropy ≈ 0 (XOR-friendly):  XOR-family generally ≥ LNS → label = 1 (or borderline)
- entropy ≈ 26 (XOR-hostile):  G ≈ 0.97x << LNS ≈ 2.2x → label = 0 (clear LNS wins)

The threshold τ* will be fitted on the **train set** (unchanged: Miranda + Hurricane +
AAPL/MSFT daily) to maximise F1 for label prediction using `mantissa_entropy_bits < τ*`.
Test set evaluation proceeds after threshold is committed.

---

*This amendment does not touch the train/test split, codec suite, AUC gate (≥ 0.85),
statistical test protocol, or any other pre-registered element.*
