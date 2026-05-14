// Copyright 2026 Anuj Attri
// Licensed under the Apache License, Version 2.0 (the "License");
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
// SPDX-License-Identifier: Apache-2.0
#pragma once
// XOR-locality probe: O(n_sample) diagnostic measuring how well a stream of
// IEEE 754 doubles is suited to XOR-based compression (Gorilla, Chimp, Elf).
//
// Two independent signals:
//   XOR metrics (mean_leading_zeros, mean_trailing_zeros, mean_significant_bits):
//     directly predict Gorilla encode efficiency per window.
//   mantissa_entropy_bits: marginal Shannon entropy of the low-26 mantissa bits.
//     Clean IEEE 754 values (simulation outputs, integer-valued prices) cluster
//     near 0 bits. Values produced by upstream scalar multiplication (adj_factor,
//     calibration coefficients, unit conversions) cluster near 26 bits, because
//     floating-point rounding fills the low mantissa with pseudo-random noise.
//
// Sample size: min(1024, n_values) consecutive values.
// Latency:     < 50 us on 1024 doubles (verified by bench/bench_probe.cpp).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lns {

struct XorLocalityScore {
    double mean_leading_zeros;     // mean LZ count over XOR of consecutive pairs
    double mean_trailing_zeros;    // mean TZ count
    double mean_significant_bits;  // mean sig-bits = 64 - LZ - TZ (lower is better for Gorilla)
    double mantissa_entropy_bits;  // marginal Shannon entropy of low-26 mantissa bits [0, 26]
};

namespace probe_detail {

inline int xp_clz64(uint64_t x) {
    if (x == 0) return 64;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll(static_cast<unsigned long long>(x));
#elif defined(_MSC_VER)
    unsigned long idx;
    return _BitScanReverse64(&idx, x) ? 63 - static_cast<int>(idx) : 64;
#else
    int n = 0;
    while (!(x & (1ULL << 63))) { x <<= 1; ++n; }
    return n;
#endif
}

inline int xp_ctz64(uint64_t x) {
    if (x == 0) return 64;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(static_cast<unsigned long long>(x));
#elif defined(_MSC_VER)
    unsigned long idx;
    return _BitScanForward64(&idx, x) ? static_cast<int>(idx) : 64;
#else
    int n = 0;
    while (!(x & 1)) { x >>= 1; ++n; }
    return n;
#endif
}

} // namespace probe_detail

// Probe n_sample = min(1024, n) values from data[0..n).
inline XorLocalityScore probe(const double* data, size_t n) {
    static constexpr size_t MAX_SAMPLE = 1024;
    const size_t ns = std::min(n, MAX_SAMPLE);

    if (ns < 2) {
        // Degenerate: classify as XOR-hostile (worst case for caller).
        return {0.0, 0.0, 64.0, 26.0};
    }

    // Mantissa entropy: count 1-bits per position across low-26 mantissa bits.
    // IEEE 754 double layout: [sign(1) | exponent(11) | mantissa(52)]
    // Bits 0-25 of the 64-bit word are the low 26 mantissa bits.
    uint32_t bit_cnt[26] = {};
    for (size_t i = 0; i < ns; ++i) {
        uint64_t bits;
        std::memcpy(&bits, data + i, sizeof(bits));
        const uint32_t low26 = static_cast<uint32_t>(bits & 0x3FFFFFFull);
        for (int b = 0; b < 26; ++b)
            bit_cnt[b] += (low26 >> b) & 1u;
    }

    // XOR metrics over n_pairs = ns - 1 consecutive pairs.
    double sum_lz = 0.0, sum_tz = 0.0, sum_sig = 0.0;
    const size_t n_pairs = ns - 1;
    for (size_t i = 1; i < ns; ++i) {
        uint64_t a, b;
        std::memcpy(&a, data + i - 1, sizeof(a));
        std::memcpy(&b, data + i,     sizeof(b));
        const uint64_t x = a ^ b;
        if (x == 0) {
            sum_lz += 64.0; // perfect locality; tz and sig contribute 0
        } else {
            const int lz  = probe_detail::xp_clz64(x);
            const int tz  = probe_detail::xp_ctz64(x);
            const int sig = 64 - lz - tz;
            sum_lz  += static_cast<double>(lz);
            sum_tz  += static_cast<double>(tz);
            sum_sig += static_cast<double>(sig);
        }
    }

    // Marginal Shannon entropy: sum H(bit_b) over 26 mantissa bit positions.
    // H(p) = -p*log2(p) - (1-p)*log2(1-p); 0 when p = 0 or p = 1.
    double entropy = 0.0;
    const double inv_ns = 1.0 / static_cast<double>(ns);
    for (int b = 0; b < 26; ++b) {
        const double p = static_cast<double>(bit_cnt[b]) * inv_ns;
        if (p > 0.0 && p < 1.0)
            entropy -= p * std::log2(p) + (1.0 - p) * std::log2(1.0 - p);
    }

    return {
        sum_lz  / static_cast<double>(n_pairs),
        sum_tz  / static_cast<double>(n_pairs),
        sum_sig / static_cast<double>(n_pairs),
        entropy
    };
}

} // namespace lns
