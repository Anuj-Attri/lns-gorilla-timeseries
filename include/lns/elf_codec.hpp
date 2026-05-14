// Copyright 2026 Anuj Attri
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Elf: Erasing-based Lossless Floating-Point Compression.
//
// Reference: Jing Zhang et al., "Elf: Erasing-based Lossless Floating-Point
//   Compression for Time Series Databases", VLDB 2023.
//
// Key idea: For each double value v, find the smallest integer beta in [0,18]
// such that round(v * 10^beta) / 10^beta == v exactly in IEEE 754 arithmetic.
// This "beta" identifies how many decimal significant digits v contains.
// Then map v to an integer via floor(v * 10^beta) and apply Gorilla-style XOR
// on those integers. The integer representation has many trailing zeros, which
// reduces the significant bits in XOR and improves Gorilla compression.
//
// Wire format:
//   [4 bytes LE uint32_t: n_values]
//   For each value:
//     [5-bit beta value (0-18; 31 = NaN/Inf/special)]
//   Then:
//     [8 bytes LE uint64_t: first transformed int]
//     [Gorilla XOR bit-stream on the int64_t representations]
//
// Note: Values that are not finite or cannot be represented with any beta <= 18
// are stored verbatim (beta = 31, raw bits written in the XOR stream without
// Elf transformation). This guarantees lossless round-trip on all inputs.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gorilla_codec.hpp"  // BitWriter, BitReader, clz64, ctz64

namespace elf {

// Precomputed powers of 10 (10^0 .. 10^18).
static const double POW10[19] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18
};

static constexpr int BETA_VERBATIM = 31; // special marker for non-transformable values
static constexpr int BETA_BITS     = 5;  // 0-18 + sentinel 31, fits in 5 bits

// Find smallest beta in [0,18] such that round(v * 10^beta) / 10^beta == v.
// Returns BETA_VERBATIM if no such beta exists (NaN, Inf, or too many digits).
inline int find_beta(double v) {
    if (!std::isfinite(v)) return BETA_VERBATIM;
    // Preserve -0.0: its sign bit would be lost through integer round-trip.
    if (v == 0.0) return BETA_VERBATIM;

    // Work with absolute value; sign handled separately.
    double abs_v = std::abs(v);

    for (int beta = 0; beta <= 18; ++beta) {
        double scaled  = abs_v * POW10[beta];
        double rounded = std::round(scaled);
        // Check for integer overflow (rounded must fit in int64_t range).
        if (rounded >= 9.2e18) break;
        double recovered = rounded / POW10[beta];
        if (recovered == abs_v) return beta;
    }
    return BETA_VERBATIM;
}

// Map value → int64_t integer representation using beta.
// For BETA_VERBATIM: reinterpret as uint64_t (raw bits).
inline uint64_t encode_value(double v, int beta) {
    if (beta == BETA_VERBATIM) {
        uint64_t bits; std::memcpy(&bits, &v, 8); return bits;
    }
    // Preserve sign: store in sign-magnitude form as a signed 64-bit integer.
    // Negative values: use sign bit of the double's bit representation.
    double scaled = v * POW10[beta];
    int64_t ival  = static_cast<int64_t>(std::round(scaled));
    return static_cast<uint64_t>(ival);
}

// Recover original double from integer representation.
inline double decode_value(uint64_t bits, int beta) {
    if (beta == BETA_VERBATIM) {
        double v; std::memcpy(&v, &bits, 8); return v;
    }
    int64_t ival = static_cast<int64_t>(bits);
    return static_cast<double>(ival) / POW10[beta];
}

// ─── encode ─────────────────────────────────────────────────────────────────
inline std::vector<uint8_t> encode(const double* data, size_t n) {
    std::vector<uint8_t> buf;
    buf.reserve(4 + (n * BETA_BITS + 7) / 8 + 8 + n * 5);

    // Count header.
    uint32_t cnt = static_cast<uint32_t>(n);
    for (int b = 0; b < 4; ++b) buf.push_back((cnt >> (b * 8)) & 0xFF);
    if (n == 0) return buf;

    // Phase 1: compute beta per value, write as BETA_BITS-bit packed header.
    std::vector<int>      betas(n);
    std::vector<uint64_t> ints(n);
    for (size_t i = 0; i < n; ++i) {
        betas[i] = find_beta(data[i]);
        ints[i]  = encode_value(data[i], betas[i]);
    }

    // Write beta stream: BETA_BITS bits per value, packed MSB-first.
    {
        gorilla::BitWriter bw(buf);
        for (size_t i = 0; i < n; ++i)
            bw.write(static_cast<uint64_t>(betas[i]), BETA_BITS);
    }

    // Phase 2: Gorilla XOR encode on the integer representations.
    // Write first value verbatim (8 bytes LE).
    uint64_t first = ints[0];
    for (int b = 0; b < 8; ++b) buf.push_back((first >> (b * 8)) & 0xFF);

    // XOR encode the rest.
    gorilla::BitWriter bw2(buf);
    int prev_leading  = -1;
    int prev_trailing =  0;
    int prev_sig_bits =  0;
    uint64_t prev     = first;

    for (size_t i = 1; i < n; ++i) {
        uint64_t xorval = ints[i] ^ prev;
        prev = ints[i];

        if (xorval == 0) {
            bw2.write1(0);
            continue;
        }
        bw2.write1(1);

        int leading_raw = gorilla::clz64(xorval);
        int trailing    = gorilla::ctz64(xorval);
        int leading     = std::min(leading_raw, 63);
        int sig_bits    = 64 - leading - trailing;
        if (sig_bits <= 0) { sig_bits = 1; trailing = 64 - leading - 1; }

        bool can_reuse = (prev_leading >= 0)
                      && (leading  >= prev_leading)
                      && (trailing >= prev_trailing);

        if (can_reuse) {
            bw2.write1(0);
            uint64_t mask = (prev_sig_bits < 64) ? ((1ULL << prev_sig_bits) - 1) : ~0ULL;
            bw2.write((xorval >> prev_trailing) & mask, prev_sig_bits);
        } else {
            bw2.write1(1);
            bw2.write(static_cast<uint64_t>(leading),    6);
            bw2.write(static_cast<uint64_t>(sig_bits-1), 6);
            uint64_t mask = (sig_bits < 64) ? ((1ULL << sig_bits) - 1) : ~0ULL;
            bw2.write((xorval >> trailing) & mask, sig_bits);
            prev_leading  = leading;
            prev_trailing = trailing;
            prev_sig_bits = sig_bits;
        }
    }
    return buf;
}

// ─── decode ─────────────────────────────────────────────────────────────────
inline std::vector<double> decode(const uint8_t* buf, size_t byte_len) {
    if (byte_len < 4) return {};

    uint32_t n = static_cast<uint32_t>(buf[0])
               | (static_cast<uint32_t>(buf[1]) <<  8)
               | (static_cast<uint32_t>(buf[2]) << 16)
               | (static_cast<uint32_t>(buf[3]) << 24);

    std::vector<double> out;
    out.reserve(n);
    if (n == 0) return out;

    // Read beta stream: n * BETA_BITS bits starting at byte 4.
    std::vector<int> betas(n);
    {
        gorilla::BitReader br(buf + 4, byte_len - 4);
        for (uint32_t i = 0; i < n; ++i)
            betas[i] = static_cast<int>(br.read(BETA_BITS));
    }

    // Beta stream occupies ceil(n * BETA_BITS / 8) bytes.
    // The BitWriter ctor primes exactly the first byte of the stream, so
    // beta_bytes == ceil(n * BETA_BITS / 8) already includes that byte.
    size_t beta_bytes = (static_cast<size_t>(n) * BETA_BITS + 7) / 8;
    size_t xor_start  = 4 + beta_bytes;

    if (xor_start + 8 > byte_len) return out;

    // Read first integer value (8 bytes LE).
    uint64_t prev = 0;
    for (int b = 0; b < 8; ++b)
        prev |= static_cast<uint64_t>(buf[xor_start + b]) << (b * 8);
    out.push_back(decode_value(prev, betas[0]));

    // XOR bit-stream: the BitWriter ctor primes one byte immediately after
    // the first value, so the stream starts at xor_start + 8.
    gorilla::BitReader br2(buf + xor_start + 8, byte_len - xor_start - 8);

    int prev_leading  = 0;
    int prev_trailing = 0;
    int prev_sig_bits = 0;

    for (uint32_t i = 1; i < n; ++i) {
        if (br2.eof()) break;

        int bit0 = br2.read1();
        if (bit0 == 0) {
            out.push_back(decode_value(prev, betas[i]));
            continue;
        }
        int bit1 = br2.read1();
        if (bit1 == 0) {
            uint64_t meaningful = br2.read(prev_sig_bits);
            uint64_t xorval     = meaningful << prev_trailing;
            prev ^= xorval;
        } else {
            int leading  = static_cast<int>(br2.read(6));
            int sig_bits = static_cast<int>(br2.read(6)) + 1;
            int trailing = 64 - leading - sig_bits;
            if (trailing < 0) { trailing = 0; sig_bits = 64 - leading; }
            uint64_t meaningful = br2.read(sig_bits);
            uint64_t xorval     = meaningful << trailing;
            prev ^= xorval;
            prev_leading  = leading;
            prev_trailing = trailing;
            prev_sig_bits = sig_bits;
        }
        out.push_back(decode_value(prev, betas[i]));
    }
    return out;
}

} // namespace elf
