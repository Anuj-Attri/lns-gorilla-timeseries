// Copyright 2026 Anuj Attri
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Chimp128: XOR-based floating-point compression with a 128-entry XOR cache.
//
// Reference: Liakos et al., "Chimp: Efficient Lossless Floating Point
//   Compression for Time Series Databases", VLDB 2022.
//
// Key differences from Gorilla:
//   1. 128-entry ring buffer: each value is XOR'd against the cached entry that
//      minimises significant bits (exhaustive scan), not just the previous value.
//   2. Improved leading-zeros encoding: 3-bit index into a lookup table
//      {0,8,12,16,18,22,26,64} vs Gorilla's 6-bit exact count.
//   3. Variable-length flag prefix code:
//        '0'   (1 bit)  — XOR = 0 with previous output (same value)
//        '10'  (2 bits) — reuse previous [leading, trailing] window; +7-bit ref
//        '110' (3 bits) — new LZ from table; +7-bit ref +3-bit LZ +6-bit sig +sig bits
//
// Wire format:
//   [4 bytes LE uint32_t: n_values]
//   [8 bytes LE uint64_t: first value verbatim]
//   [bit stream: prefix-coded blocks for values [1, n)]

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gorilla_codec.hpp"  // BitWriter, BitReader, clz64, ctz64

namespace chimp128 {

// Leading-zero lookup table: 8 representative values → 3-bit indices.
// Values chosen from common LZ counts in financial/IoT IEEE 754 doubles.
// When actual LZ falls between entries, we round DOWN (conservative) so the
// reconstructed XOR still covers the actual meaningful bits.
static constexpr int LZ_TABLE[8] = {0, 8, 12, 16, 18, 22, 26, 64};

// Map actual LZ count → table index (largest table entry ≤ lz).
inline int lz_to_idx(int lz) {
    int best = 0;
    for (int i = 1; i < 8; ++i) {
        if (LZ_TABLE[i] <= lz) best = i;
        else break;
    }
    return best;
}

static constexpr int CACHE_SIZE = 128;

// ─── encode ─────────────────────────────────────────────────────────────────
inline std::vector<uint8_t> encode(const uint64_t* vals, size_t n) {
    std::vector<uint8_t> buf;
    buf.reserve(4 + 8 + n * 5);

    // Count header (4 bytes LE).
    auto push32 = [&](uint32_t v) {
        for (int b = 0; b < 4; ++b) buf.push_back((v >> (b * 8)) & 0xFF);
    };
    push32(static_cast<uint32_t>(n));
    if (n == 0) return buf;

    // First value verbatim (8 bytes LE).
    uint64_t first = vals[0];
    for (int b = 0; b < 8; ++b) buf.push_back((first >> (b * 8)) & 0xFF);

    // 128-entry ring buffer.
    uint64_t cache[CACHE_SIZE] = {};
    int cache_fill = 0;
    int cache_head = 0;
    auto cache_push = [&](uint64_t v) {
        cache[cache_head] = v;
        cache_head        = (cache_head + 1) % CACHE_SIZE;
        if (cache_fill < CACHE_SIZE) ++cache_fill;
    };
    cache_push(first);

    // Per-block state for REUSE window.
    int prev_lz       = -1; // -1 = no prior block
    int prev_trailing =  0;
    int prev_sig_bits =  0;
    uint64_t prev     = first; // previous OUTPUT value (for '0' case)

    gorilla::BitWriter bw(buf);

    for (size_t i = 1; i < n; ++i) {
        const uint64_t val = vals[i];

        // ── Case '0': identical to previous output ──────────────────────
        if (val == prev) {
            bw.write1(0); // '0'
            cache_push(val);
            prev = val;
            continue;
        }

        // ── Find best XOR partner from cache (non-previous entries first) ──
        // We always prefer minimising sig_bits; ties go to earlier entries.
        int best_idx = 0;
        int best_sig = 65;
        // Compute XOR with previous value first as baseline.
        {
            uint64_t x   = val ^ prev;
            int lz       = gorilla::clz64(x);
            int tz       = gorilla::ctz64(x);
            int sig      = 64 - lz - tz;
            if (sig < best_sig) { best_sig = sig; best_idx = cache_fill - 1; }
        }
        // Scan entire cache for a better partner.
        for (int c = 0; c < cache_fill - 1; ++c) {
            uint64_t x = val ^ cache[c];
            if (x == 0) { best_sig = 0; best_idx = c; break; }
            int sig = 64 - gorilla::clz64(x) - gorilla::ctz64(x);
            if (sig < best_sig) { best_sig = sig; best_idx = c; }
        }

        const uint64_t xorval   = val ^ cache[best_idx];
        const int lz_raw        = (xorval == 0) ? 64 : gorilla::clz64(xorval);
        const int tz            = (xorval == 0) ?  0 : gorilla::ctz64(xorval);
        const int sig_bits_real = (xorval == 0) ?  0 : 64 - lz_raw - tz;

        // ── Can we reuse the previous LZ window? ────────────────────────
        const bool can_reuse = (prev_lz >= 0)
                            && (lz_raw  >= prev_lz)
                            && (tz      >= prev_trailing)
                            && (prev_sig_bits > 0);

        if (can_reuse) {
            // '10' + 7-bit ref + sig bits within prev window.
            bw.write1(1); bw.write1(0);
            bw.write(static_cast<uint64_t>(best_idx), 7);
            uint64_t mask = (prev_sig_bits < 64)
                            ? ((1ULL << prev_sig_bits) - 1) : ~0ULL;
            bw.write((xorval >> prev_trailing) & mask, prev_sig_bits);
            // Do not update prev window state on reuse.
        } else {
            // '110' + 7-bit ref + 3-bit LZ idx + 6-bit sig count + sig bits.
            const int lz_idx = lz_to_idx(lz_raw);
            const int lz_rep = LZ_TABLE[lz_idx]; // rounded-down LZ
            int adj_sig      = 64 - lz_rep - tz;
            if (adj_sig <= 0) adj_sig = 1;

            bw.write1(1); bw.write1(1); bw.write1(0);
            bw.write(static_cast<uint64_t>(best_idx), 7);
            bw.write(static_cast<uint64_t>(lz_idx),   3);
            bw.write(static_cast<uint64_t>(adj_sig-1), 6);
            uint64_t mask = (adj_sig < 64) ? ((1ULL << adj_sig) - 1) : ~0ULL;
            bw.write((xorval >> tz) & mask, adj_sig);

            prev_lz       = lz_rep;
            prev_trailing = tz;
            prev_sig_bits = adj_sig;
        }

        cache_push(val);
        prev = val;
    }
    return buf;
}

// ─── decode ─────────────────────────────────────────────────────────────────
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

    uint64_t first = 0;
    for (int b = 0; b < 8; ++b)
        first |= static_cast<uint64_t>(buf[4 + b]) << (b * 8);
    out.push_back(first);

    uint64_t cache[CACHE_SIZE] = {};
    int cache_fill = 0;
    int cache_head = 0;
    auto cache_push = [&](uint64_t v) {
        cache[cache_head] = v;
        cache_head        = (cache_head + 1) % CACHE_SIZE;
        if (cache_fill < CACHE_SIZE) ++cache_fill;
    };
    cache_push(first);

    int prev_lz       = -1;
    int prev_trailing =  0;
    int prev_sig_bits =  0;
    uint64_t prev     = first;

    // Bit stream starts at byte 12 (4 count + 8 first value).
    gorilla::BitReader br(buf + 12, byte_len - 12);

    for (uint32_t i = 1; i < n; ++i) {
        if (br.eof()) break;

        uint64_t val;
        int bit0 = br.read1();

        if (bit0 == 0) {
            // '0': same as previous output.
            val = prev;
        } else {
            int bit1 = br.read1();
            if (bit1 == 0) {
                // '10': reuse previous window.
                int ref_idx = static_cast<int>(br.read(7));
                if (ref_idx >= cache_fill) ref_idx = 0;
                uint64_t meaningful = br.read(prev_sig_bits);
                uint64_t xorval     = meaningful << prev_trailing;
                val = cache[ref_idx] ^ xorval;
            } else {
                // '110': new control word.
                int bit2 = br.read1();
                (void)bit2; // always 0 in current scheme
                int ref_idx = static_cast<int>(br.read(7));
                if (ref_idx >= cache_fill) ref_idx = 0;
                int lz_idx   = static_cast<int>(br.read(3));
                int lz_rep   = LZ_TABLE[lz_idx];
                int adj_sig  = static_cast<int>(br.read(6)) + 1;
                int trailing = 64 - lz_rep - adj_sig;
                if (trailing < 0) trailing = 0;
                uint64_t meaningful = br.read(adj_sig);
                uint64_t xorval     = meaningful << trailing;
                val = cache[ref_idx] ^ xorval;

                prev_lz       = lz_rep;
                prev_trailing = trailing;
                prev_sig_bits = adj_sig;
            }
        }

        out.push_back(val);
        cache_push(val);
        prev = val;
    }
    return out;
}

} // namespace chimp128
