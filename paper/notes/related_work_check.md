# M1 — Prior Work Differentiation Check
**Date:** 2026-05-14  
**Papers read:** arXiv:2303.04478 (IEEE ICC 2023), arXiv:2308.03623 (Springer DCAI 2023)  
**Authors:** Francesco Taurone, Daniel E. Lucani, Marcell Fehér, Qi Zhang (Aarhus University)  
**Gate result: PASS — framing confirmed distinct. Milestone chain may proceed.**

---

## What Taurone et al. Did

Both papers address the same core problem: floating-point time-series values as stored in memory have low shared-bit count, which limits the compression ratio of bit-pattern-based compressors. Their solution is **constructive preprocessing** — they engineer bit-pattern regularity into data that does not naturally have it.

**ICC 2023 (arXiv:2303.04478, "Change a Bit to save Bytes"):** Proposes two lossy preprocessing methods. The *addition transform* shifts all values by a constant A chosen so that the transformed set lands in an IEEE 754 exponent region where several mantissa bits are guaranteed to be shared. The *multiplication transform* substitutes each x_i with a nearby x̂_i (within the user's error bound) whose mantissa, when multiplied by a specific factor M ∈ {3,...,61}, produces a long run of trailing zeros. Both methods increase the number of common bits across the dataset before it is passed to the compressor. Up to 80% size reduction at ≤ 1% error. Compressor used: Greedy-GD (generalized deduplication with random-access capability). Datasets: IoT sensor data, beach water quality, taxi trips, gas turbine emissions.

**DCAI 2023 (arXiv:2308.03623, "Lossless preprocessing of floating point data"):** Extends the ICC work to lossless methods. Four techniques: *compact bins* (cluster values into bins and shift bins together), *multiply and shift* (iteratively double and add to push values into high-shared-mantissa regions), *shift and separate even from odd* (use mantissa evenness to distinguish bin membership on decode), *shift and save evenness* (same idea but store evenness as a metadata bit). Goal is the same: guarantee that the D most significant mantissa bits are shared across the preprocessed dataset. Compressor: Greedy-GD. Datasets: Chicago taxi trips, UCI gas turbine emissions (first 1000 elements). Up to 40% improvement losslessly.

**What they do NOT do:**

- Neither paper uses or discusses XOR-based compression (Gorilla, Chimp, Elf, Patas, or any streaming delta-XOR codec).
- Neither paper measures or predicts XOR locality of consecutive value pairs.
- Neither paper proposes any runtime probe, classifier, or routing decision.
- Neither paper considers the case where data arrives already transformed by an upstream pipeline (sensor calibration, financial adjustment factor, unit conversion) and this *pre-existing* transformation has destroyed the data's natural XOR locality.
- Neither paper addresses TSDB column routing, codec selection at ingestion time, or adaptive encoding.
- The Gorilla codec, Chimp, Elf, ALP, or Pcodec are not cited or mentioned.
- There is no concept of "provenance" — the origin of the bit patterns is not discussed.

---

## Why Our Diagnostic Inversion Is Distinct

Taurone et al. ask: *given data with low shared-bit count, how do we engineer locality?*  
We ask: *given data arriving at a TSDB, how do we detect whether an upstream scalar operation has already injected mantissa noise that will defeat XOR-based codecs, and how do we route accordingly?*

These are formally inverse problems. Theirs is constructive synthesis; ours is diagnostic analysis. Theirs requires modifying values; ours is read-only and lossless. Theirs targets a single codec family (deduplication); ours targets the codec-selection boundary between XOR-locality codecs (Gorilla, Chimp, Elf) and structure-exploiting codecs (LNS+Gorilla).

The upstream-arithmetic-contamination phenomenon — that `adj_close = close × (adj_close/close)` injects mantissa noise that collapses Gorilla's compression ratio from ~2.4× to ~1.15× on the same ticker — is not a case Taurone et al. address, describe, or could address with their constructive approach. They would require the user to tolerate lossy recovery; our probe identifies the regime and routes to the correct lossless codec.

**Conclusion:** Neither Taurone paper covers diagnostic mantissa-provenance probing on TSDB data. The contribution sentence and the differentiation paragraph are confirmed intact. Milestone chain proceeds to M2.
