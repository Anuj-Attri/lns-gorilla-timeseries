// Copyright 2026 Anuj Attri
// Licensed under the Apache License, Version 2.0 (the "License");
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Gorilla XOR time-series compression.
// Clean-room implementation of:
//   Pelkonen et al., "Gorilla: A Fast, Scalable, In-Memory Time Series Database"
//   VLDB 2015, §4.1.2
//
// Encodes a stream of 64-bit words (doubles or LNS int64s reinterpreted).
// Three block types per value after the first:
//   '0'   — XOR is zero (same as previous)
//   '10'  — XOR meaningful bits fit in the previous [leading, trailing] window
//   '11'  — new leading/trailing control word + meaningful bits

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gorilla {

// ── clz/ctz helpers ───────────────────────────────────────────────────────────
inline int clz64(uint64_t x) {
    if (x == 0) return 64;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll(static_cast<unsigned long long>(x));
#elif defined(_MSC_VER)
    unsigned long idx;
    return _BitScanReverse64(&idx, x) ? 63 - static_cast<int>(idx) : 64;
#else
    int n = 0; while (!(x & (1ULL<<63))) { x<<=1; ++n; } return n;
#endif
}

inline int ctz64(uint64_t x) {
    if (x == 0) return 64;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(static_cast<unsigned long long>(x));
#elif defined(_MSC_VER)
    unsigned long idx;
    return _BitScanForward64(&idx, x) ? static_cast<int>(idx) : 64;
#else
    int n = 0; while (!(x & 1)) { x>>=1; ++n; } return n;
#endif
}

// ── BitWriter ─────────────────────────────────────────────────────────────────
// Appends bits MSB-first into a byte vector.
class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& buf) : buf_(buf) {
        buf_.push_back(0); // prime the first byte
    }

    // Write the `bits` most-significant bits of `value`.
    void write(uint64_t value, int bits) {
        while (bits > 0) {
            int space = 8 - bit_pos_;
            if (space == 0) {
                buf_.push_back(0);
                bit_pos_ = 0;
                space = 8;
            }
            int take = std::min(space, bits);
            uint64_t chunk = (value >> (bits - take)) & ((1ULL << take) - 1);
            buf_.back() |= static_cast<uint8_t>(chunk << (space - take));
            bits     -= take;
            bit_pos_ += take;
        }
    }

    void write1(int bit) { write(static_cast<uint64_t>(bit & 1), 1); }

private:
    std::vector<uint8_t>& buf_;
    int bit_pos_ = 0;
};

// ── BitReader ─────────────────────────────────────────────────────────────────
class BitReader {
public:
    BitReader(const uint8_t* data, size_t byte_len)
        : data_(data), byte_len_(byte_len) {}

    uint64_t read(int bits) {
        uint64_t result = 0;
        while (bits > 0) {
            if (byte_pos_ >= byte_len_) return 0;
            int avail = 8 - bit_pos_;
            int take  = std::min(avail, bits);
            uint8_t chunk = static_cast<uint8_t>(
                (data_[byte_pos_] >> (avail - take)) & ((1 << take) - 1));
            result = (result << take) | chunk;
            bit_pos_ += take;
            bits     -= take;
            if (bit_pos_ == 8) { bit_pos_ = 0; ++byte_pos_; }
        }
        return result;
    }

    int read1() { return static_cast<int>(read(1)); }
    bool eof() const { return byte_pos_ >= byte_len_; }

private:
    const uint8_t* data_;
    size_t byte_len_;
    size_t byte_pos_ = 0;
    int    bit_pos_  = 0;
};

// ── encode ────────────────────────────────────────────────────────────────────
// Wire format:
//   [4 bytes LE uint32_t: n_values]
//   [8 bytes LE uint64_t: first value verbatim]
//   [bit stream: subsequent values as Gorilla blocks]
inline std::vector<uint8_t> encode(const uint64_t* vals, size_t n) {
    std::vector<uint8_t> buf;
    buf.reserve(4 + 8 + n * 4);

    // count header
    uint32_t cnt = static_cast<uint32_t>(n);
    buf.push_back( cnt        & 0xFF);
    buf.push_back((cnt >>  8) & 0xFF);
    buf.push_back((cnt >> 16) & 0xFF);
    buf.push_back((cnt >> 24) & 0xFF);

    if (n == 0) return buf;

    // first value verbatim (little-endian)
    uint64_t prev = vals[0];
    for (int b = 0; b < 8; ++b) buf.push_back((prev >> (b * 8)) & 0xFF);

    // bit stream for values [1, n)
    BitWriter bw(buf); // pushes the first bit-stream byte internally

    // State for "reuse previous window" optimisation.
    // Window = [prev_leading leading zeros, prev_trailing trailing zeros].
    // A new XOR can reuse the window iff:
    //   new_leading >= prev_leading  AND  new_trailing >= prev_trailing
    // which guarantees the new XOR's meaningful bits lie entirely within the window.
    int prev_leading  = -1; // -1 = no previous block
    int prev_trailing = 0;
    int prev_sig_bits = 0;

    for (size_t i = 1; i < n; ++i) {
        uint64_t xorval = vals[i] ^ prev;
        prev = vals[i];

        if (xorval == 0) {
            bw.write1(0); // block type '0': same as previous
            continue;
        }

        bw.write1(1); // first control bit = 1: XOR is non-zero

        // Compute window for this XOR value.
        int leading_raw = clz64(xorval);
        int trailing    = ctz64(xorval);
        // We use a 6-bit leading-zero field (0-63), wider than the original 5-bit
        // Gorilla spec, to handle LNS int64 values that have 36-50+ leading zeros.
        // (IEEE doubles rarely exceed 31 leading zeros; LNS fixed-point values do.)
        int leading = std::min(leading_raw, 63);
        int sig_bits = 64 - leading - trailing;
        if (sig_bits <= 0) {
            sig_bits = 1;
            trailing = 64 - leading - 1;
        }

        // Can we reuse the previous window?
        bool can_reuse = (prev_leading >= 0)
                      && (leading  >= prev_leading)
                      && (trailing >= prev_trailing);

        if (can_reuse) {
            // Block type '10': single '0' bit after the not-same '1'.
            // The decode interprets bit0=1, bit1=0 as "reuse previous window".
            // Extract bits [prev_trailing, prev_trailing + prev_sig_bits - 1].
            bw.write1(0);
            uint64_t mask = (prev_sig_bits < 64) ? ((1ULL << prev_sig_bits) - 1) : ~0ULL;
            uint64_t meaningful = (xorval >> prev_trailing) & mask;
            bw.write(meaningful, prev_sig_bits);
            // Note: prev_leading/trailing/sig_bits are NOT updated on reuse.
        } else {
            // Block type '11': single '1' bit after the not-same '1', then control word.
            bw.write1(1);
            bw.write(static_cast<uint64_t>(leading), 6);      // 6-bit: supports 0-63
            bw.write(static_cast<uint64_t>(sig_bits - 1), 6); // 0 → 1 sig bit
            uint64_t mask = (sig_bits < 64) ? ((1ULL << sig_bits) - 1) : ~0ULL;
            uint64_t meaningful = (xorval >> trailing) & mask;
            bw.write(meaningful, sig_bits);

            prev_leading  = leading;
            prev_trailing = trailing;
            prev_sig_bits = sig_bits;
        }
    }
    return buf;
}

// ── decode ────────────────────────────────────────────────────────────────────
inline std::vector<uint64_t> decode(const uint8_t* buf, size_t byte_len) {
    if (byte_len < 4) return {};

    uint32_t n = static_cast<uint32_t>(buf[0])
               | (static_cast<uint32_t>(buf[1]) <<  8)
               | (static_cast<uint32_t>(buf[2]) << 16)
               | (static_cast<uint32_t>(buf[3]) << 24);

    std::vector<uint64_t> out;
    out.reserve(n);
    if (n == 0) return out;
    if (byte_len < 12) return out;

    // First value verbatim (little-endian).
    uint64_t prev = 0;
    for (int b = 0; b < 8; ++b)
        prev |= static_cast<uint64_t>(buf[4 + b]) << (b * 8);
    out.push_back(prev);

    // Bit stream starts at byte 12 (4 count + 8 first value).
    // The encoder's BitWriter::ctor pushed one byte before writing any bits,
    // so the bit stream occupies bytes [12, byte_len).
    BitReader br(buf + 12, byte_len - 12);

    int prev_leading  = 0;
    int prev_trailing = 0;
    int prev_sig_bits = 0;

    for (uint32_t i = 1; i < n; ++i) {
        if (br.eof()) break;

        int bit0 = br.read1();
        if (bit0 == 0) {
            out.push_back(prev); // block '0': same as previous XOR (= same value)
            continue;
        }

        int bit1 = br.read1();
        if (bit1 == 0) {
            // Block '10': reuse previous window.
            uint64_t meaningful = br.read(prev_sig_bits);
            uint64_t xorval     = meaningful << prev_trailing;
            prev ^= xorval;
        } else {
            // Block '11': new control word.
            int leading  = static_cast<int>(br.read(6)); // 6-bit field, matches encode
            int sig_bits = static_cast<int>(br.read(6)) + 1;
            int trailing = 64 - leading - sig_bits;
            if (trailing < 0) { trailing = 0; sig_bits = 64 - leading; }
            uint64_t meaningful = br.read(sig_bits);
            uint64_t xorval     = meaningful << trailing;
            prev ^= xorval;

            prev_leading  = leading;
            prev_trailing = trailing;
            prev_sig_bits = sig_bits;
        }
        out.push_back(prev);
    }
    return out;
}

} // namespace gorilla
