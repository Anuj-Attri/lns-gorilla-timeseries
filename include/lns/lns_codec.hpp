// Copyright 2026 Anuj Attri
// Licensed under the Apache License, Version 2.0 (the "License");
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cmath>
#include <cstdint>
#include <cassert>
#include <limits>
#include <stdexcept>

namespace lns {

// ── LnsValue ─────────────────────────────────────────────────────────────────
// Fixed-point log₂ representation.
//   raw  = round(log2(|x|) * (1 << FracBits)),  packed in IntBits+FracBits bits
//   sign = separate bit (allows negative inputs)
//   Special states: is_zero, is_nan, is_inf
//
// Q-format nomenclature: Q<IntBits>.<FracBits>
// Supported sweep: Q8.24, Q10.22, Q12.20 (f32-class), Q12.16, Q16.48 (f64-class).
//
// Round-trip RMS relative error bounds (verified by test_lns_roundtrip):
//   Q8.24  < 1e-7
//   Q10.22 < 1e-6
//   Q12.20 < 1e-5
//   Q12.16 < 1e-4

template <int IntBits, int FracBits>
struct LnsValue {
    static_assert(IntBits >= 1 && FracBits >= 1,
                  "Both IntBits and FracBits must be positive");
    static_assert(IntBits + FracBits <= 63,
                  "IntBits + FracBits must fit in int64_t (max 63)");

    int64_t raw;    // signed fixed-point: sign(x) * round(log2(|x|) * scale)
    uint8_t flags;  // bit 0 = is_zero, bit 1 = is_nan, bit 2 = is_inf, bit 3 = is_negative

    static constexpr int64_t SCALE = (int64_t)1 << FracBits;
    static constexpr int64_t MAX_RAW = ((int64_t)1 << (IntBits + FracBits - 1)) - 1;

    static constexpr uint8_t FLAG_ZERO = 0x01;
    static constexpr uint8_t FLAG_NAN  = 0x02;
    static constexpr uint8_t FLAG_INF  = 0x04;
    static constexpr uint8_t FLAG_NEG  = 0x08;

    bool is_zero()     const { return flags & FLAG_ZERO; }
    bool is_nan()      const { return flags & FLAG_NAN;  }
    bool is_inf()      const { return flags & FLAG_INF;  }
    bool is_negative() const { return flags & FLAG_NEG;  }
    bool is_normal()   const { return (flags & (FLAG_ZERO|FLAG_NAN|FLAG_INF)) == 0; }

    // Serialized byte size for this representation.
    // We pack: raw (8 bytes always; upper unused bits zero) + flags (1 byte).
    static constexpr size_t WIRE_BYTES = 9;
};

// ── encode_lns ────────────────────────────────────────────────────────────────
template <int I, int F>
LnsValue<I,F> encode_lns(double x) {
    using V = LnsValue<I,F>;
    V v{};
    v.raw   = 0;
    v.flags = 0;

    if (std::isnan(x))  { v.flags = V::FLAG_NAN;  return v; }
    if (x == 0.0)       { v.flags = V::FLAG_ZERO; return v; }

    bool neg = (x < 0.0);
    double ax = neg ? -x : x;

    if (std::isinf(ax)) { v.flags = V::FLAG_INF | (neg ? V::FLAG_NEG : 0); return v; }

    double log2_ax = std::log2(ax);
    // round to nearest fixed-point value
    int64_t raw = static_cast<int64_t>(std::round(log2_ax * static_cast<double>(V::SCALE)));

    // Clamp to representable range (saturation, not wrap)
    if (raw > V::MAX_RAW)  raw = V::MAX_RAW;
    if (raw < -V::MAX_RAW) raw = -V::MAX_RAW;

    v.raw   = raw;
    v.flags = neg ? V::FLAG_NEG : 0;
    return v;
}

// ── decode_lns ────────────────────────────────────────────────────────────────
template <int I, int F>
double decode_lns(LnsValue<I,F> v) {
    using V = LnsValue<I,F>;
    if (v.is_nan())  return std::numeric_limits<double>::quiet_NaN();
    if (v.is_zero()) return 0.0;
    if (v.is_inf())  return v.is_negative()
                            ? -std::numeric_limits<double>::infinity()
                            :  std::numeric_limits<double>::infinity();

    double log2_val = static_cast<double>(v.raw) / static_cast<double>(V::SCALE);
    double magnitude = std::exp2(log2_val);
    return v.is_negative() ? -magnitude : magnitude;
}

// ── Convenience type aliases for the sweep ───────────────────────────────────
using LnsQ8_24  = LnsValue<8,  24>;
using LnsQ10_22 = LnsValue<10, 22>;
using LnsQ12_20 = LnsValue<12, 20>;
using LnsQ12_16 = LnsValue<12, 16>;
using LnsQ15_48 = LnsValue<15, 48>; // Q16.48 would overflow (64 bits); Q15.48 = 63 bits

// ── Bulk encode/decode helpers ────────────────────────────────────────────────
// These write/read tightly-packed (raw||flags) pairs into a byte buffer.
// Layout per value: 8 bytes little-endian int64_t raw, then 1 byte flags.

template <int I, int F>
void encode_lns_bulk(const double* in, size_t n, uint8_t* out) {
    for (size_t i = 0; i < n; ++i) {
        auto v = encode_lns<I,F>(in[i]);
        // write raw as little-endian int64
        int64_t r = v.raw;
        for (int b = 0; b < 8; ++b) {
            out[i * 9 + b] = static_cast<uint8_t>(r & 0xFF);
            r >>= 8;
        }
        out[i * 9 + 8] = v.flags;
    }
}

template <int I, int F>
void decode_lns_bulk(const uint8_t* in, size_t n, double* out) {
    for (size_t i = 0; i < n; ++i) {
        int64_t r = 0;
        for (int b = 7; b >= 0; --b) {
            r = (r << 8) | in[i * 9 + b];
        }
        uint8_t flags = in[i * 9 + 8];
        out[i] = decode_lns<I,F>(LnsValue<I,F>{r, flags});
    }
}

} // namespace lns
