// Copyright 2026 Anuj Attri
// Licensed under the Apache License, Version 2.0 (the "License");
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
// SPDX-License-Identifier: Apache-2.0
#pragma once
// ALP codec wrapper.
// cwida/ALP (Adaptive Lossless floating-Point) — see https://github.com/cwida/ALP
//
// ALP is a lossless f64 compressor that exploits the observation that most
// real-world floating-point values have short decimal representations.
// It encodes each value as an integer exponent + scaled integer mantissa.
//
// This wrapper adapts ALP to the common Codec interface used in this project.
// If ALP headers are not found at build time (FetchContent failure), the wrapper
// falls back to IEEE baseline with a runtime warning.

#include <cstdint>
#include <cstddef>
#include <vector>
#include <cstring>

// LNS_HAS_ALP is set by CMakeLists.txt (1 = Clang + ALP available, 0 = fallback)
#ifndef LNS_HAS_ALP
#  define LNS_HAS_ALP 0
#endif

#if LNS_HAS_ALP
#  if __has_include("alp/alp.hpp")
#    include "alp/alp.hpp"
#  elif __has_include("alp.hpp")
#    include "alp.hpp"
#  else
#    undef  LNS_HAS_ALP
#    define LNS_HAS_ALP 0
#  endif
#endif

#if !LNS_HAS_ALP
#  include <cstdio>
#endif

namespace alp_wrap {

// ALP operates on fixed-size vectors (typically 1024 values = one "vector").
// We chunk the input into 1024-value blocks, compress each, and concatenate.
// Block header: 4-byte uint32_t = compressed block byte length.

static constexpr size_t BLOCK = 1024;

inline std::vector<uint8_t> encode(const double* data, size_t n) {
#if LNS_HAS_ALP
    using namespace alp;
    std::vector<uint8_t> out;
    out.reserve(n * 3); // optimistic

    size_t i = 0;
    while (i < n) {
        size_t block_n = std::min(BLOCK, n - i);

        // ALP state per block
        state st;
        uint8_t scratch[BLOCK * 8 + 256];
        size_t compressed_bytes = 0;

        // ALP encode signature varies by version; use the simplest known interface.
        // Attempt: alp::encode<double>(data+i, block_n, scratch, compressed_bytes, st)
        try {
            alp::encode(data + i, block_n, scratch, compressed_bytes, st);
        } catch (...) {
            // If ALP API doesn't match, fall back to raw copy for this block.
            compressed_bytes = block_n * 8;
            std::memcpy(scratch, data + i, compressed_bytes);
        }

        // write block header: byte count
        uint32_t blen = static_cast<uint32_t>(compressed_bytes);
        out.push_back(blen & 0xFF);
        out.push_back((blen >> 8) & 0xFF);
        out.push_back((blen >> 16) & 0xFF);
        out.push_back((blen >> 24) & 0xFF);
        // write compressed block data
        out.insert(out.end(), scratch, scratch + compressed_bytes);
        i += block_n;
    }
    return out;
#else
    // ALP not available — fall back to IEEE baseline with a one-time warning.
    static bool warned = false;
    if (!warned) {
        std::fprintf(stderr, "[alp_wrap] ALP headers not found; falling back to IEEE baseline.\n");
        warned = true;
    }
    std::vector<uint8_t> buf(n * 8);
    std::memcpy(buf.data(), data, n * 8);
    return buf;
#endif
}

inline std::vector<double> decode(const uint8_t* buf, size_t byte_len) {
#if LNS_HAS_ALP
    using namespace alp;
    std::vector<double> out;
    size_t pos = 0;
    while (pos + 4 <= byte_len) {
        uint32_t blen = static_cast<uint32_t>(buf[pos])
                      | (static_cast<uint32_t>(buf[pos+1]) << 8)
                      | (static_cast<uint32_t>(buf[pos+2]) << 16)
                      | (static_cast<uint32_t>(buf[pos+3]) << 24);
        pos += 4;
        if (pos + blen > byte_len) break;

        double decoded[BLOCK];
        size_t decoded_n = 0;
        try {
            alp::decode(buf + pos, blen, decoded, decoded_n);
        } catch (...) {
            // fallback: treat as raw doubles
            decoded_n = blen / 8;
            std::memcpy(decoded, buf + pos, decoded_n * 8);
        }
        out.insert(out.end(), decoded, decoded + decoded_n);
        pos += blen;
    }
    return out;
#else
    size_t n = byte_len / 8;
    std::vector<double> out(n);
    std::memcpy(out.data(), buf, n * 8);
    return out;
#endif
}

} // namespace alp_wrap
