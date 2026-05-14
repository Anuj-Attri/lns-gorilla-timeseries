// Copyright 2026 Anuj Attri
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Adaptive codec: runtime probe -> threshold -> route.
//
// Strategy (tau* = 13.0, pre-registered in paper/notes/preregistration_threshold.md,
// SHA 303dce0, fitted on Miranda+Hurricane+AAPL/MSFT train set):
//
//   mantissa_entropy_bits < tau*  ->  Chimp128 (best XOR codec on train)
//   mantissa_entropy_bits >= tau* ->  LNS Q10.22 + Gorilla
//
// Probe cost: O(min(1024, n)) per encode call; ~2 us on 1024 doubles.
// Route overhead: zero (same encode/decode path as the chosen codec).
//
// Wire format: 1-byte codec-id prefix + codec payload.
//   codec_id = 0: Chimp128
//   codec_id = 1: LNS Q10.22 + Gorilla
//
// Measured results (test set, 30 columns, SHA e17656d):
//   Geomean ratio: 2.082x (vs Gorilla 1.809x = +15.1%, vs LNS 1.940x = +7.3%)
//   XOR-hostile (6 cols): 1.954x vs Gorilla 1.144x = +70.8%

#include "xor_locality_probe.hpp"
#include "chimp128_codec.hpp"
#include "composite.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace adaptive {

static constexpr double TAU_STAR       = 13.0; // pre-registered threshold
static constexpr uint8_t ID_CHIMP128   = 0;
static constexpr uint8_t ID_LNS_Q10_22 = 1;

// ── encode ────────────────────────────────────────────────────────────────────
inline std::vector<uint8_t> encode(const double* data, size_t n) {
    // Probe: O(min(1024,n))
    lns::XorLocalityScore sc = lns::probe(data, n);

    std::vector<uint8_t> payload;
    uint8_t codec_id;

    if (sc.mantissa_entropy_bits < TAU_STAR) {
        // XOR-friendly: use Chimp128
        codec_id = ID_CHIMP128;
        std::vector<uint64_t> u64(n);
        for (size_t i = 0; i < n; ++i) std::memcpy(&u64[i], &data[i], 8);
        payload = chimp128::encode(u64.data(), n);
    } else {
        // XOR-hostile: use LNS Q10.22 + Gorilla
        codec_id = ID_LNS_Q10_22;
        payload = composite::LnsGorilla<10, 22>::encode(data, n);
    }

    // Prepend 1-byte codec id
    std::vector<uint8_t> out;
    out.reserve(1 + payload.size());
    out.push_back(codec_id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// ── decode ────────────────────────────────────────────────────────────────────
inline std::vector<double> decode(const uint8_t* buf, size_t byte_len) {
    if (byte_len < 2) return {};
    const uint8_t codec_id = buf[0];
    const uint8_t* payload = buf + 1;
    const size_t   pay_len = byte_len - 1;

    if (codec_id == ID_CHIMP128) {
        auto u64 = chimp128::decode(payload, pay_len);
        std::vector<double> out(u64.size());
        for (size_t i = 0; i < u64.size(); ++i) std::memcpy(&out[i], &u64[i], 8);
        return out;
    } else if (codec_id == ID_LNS_Q10_22) {
        return composite::LnsGorilla<10, 22>::decode(payload, pay_len);
    }
    return {};
}

// ── score (for testing/introspection) ────────────────────────────────────────
inline lns::XorLocalityScore score(const double* data, size_t n) {
    return lns::probe(data, n);
}

inline bool is_xor_hostile(const double* data, size_t n) {
    return lns::probe(data, n).mantissa_entropy_bits >= TAU_STAR;
}

} // namespace adaptive
