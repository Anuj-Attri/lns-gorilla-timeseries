// Copyright 2026 Anuj Attri
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Pcodec wrapper: calls the Rust pco crate via a C ABI built in
// third_party/pco_wrapper (cargo build --release).
//
// Build dependency: third_party/pco_wrapper/target/release/libpco_wrapper.a
// Linked by CMakeLists.txt when LNS_HAS_PCO=1.
//
// Reference: "Pcodec: A Floating-Point Sequence Compressor That Reveals
//   the Latent Integer Structure", 2025 (pcodec/pcodec on GitHub).
//
// ABI (declared here; defined in the Rust staticlib):
//   pco_compress_f64  — compress n f64 → byte buffer
//   pco_decompress_f64 — decompress byte buffer → f64 array
//   pco_free          — free compressed byte buffer
//   pco_free_f64      — free decompressed f64 array

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

// ─── C ABI declarations ───────────────────────────────────────────────────────
extern "C" {
    int pco_compress_f64(const double* data_ptr, size_t n,
                         uint8_t** out_ptr, size_t* out_len);
    int pco_decompress_f64(const uint8_t* compressed_ptr, size_t compressed_len,
                           double** out_ptr, size_t* out_n);
    void pco_free(uint8_t* ptr, size_t len);
    void pco_free_f64(double* ptr, size_t n);
}

namespace pcodec {

// Wire format:
//   [4 bytes LE uint32_t: n_values]
//   [4 bytes LE uint32_t: compressed byte length]
//   [compressed bytes from pco_compress_f64]

inline std::vector<uint8_t> encode(const double* data, size_t n) {
    std::vector<uint8_t> out;
    if (n == 0) {
        out.resize(8, 0);
        return out;
    }

    uint8_t* comp_ptr = nullptr;
    size_t   comp_len = 0;
    int rc = pco_compress_f64(data, n, &comp_ptr, &comp_len);
    if (rc != 0 || comp_ptr == nullptr) {
        std::fprintf(stderr, "[pcodec] pco_compress_f64 failed (rc=%d)\n", rc);
        // Fallback: raw bytes + header.
        out.resize(8 + n * 8);
        uint32_t cnt = static_cast<uint32_t>(n);
        uint32_t len = static_cast<uint32_t>(n * 8);
        for (int b = 0; b < 4; ++b) out[b]   = (cnt >> (b*8)) & 0xFF;
        for (int b = 0; b < 4; ++b) out[4+b] = (len >> (b*8)) & 0xFF;
        std::memcpy(out.data() + 8, data, n * 8);
        return out;
    }

    out.resize(8 + comp_len);
    uint32_t cnt = static_cast<uint32_t>(n);
    uint32_t len = static_cast<uint32_t>(comp_len);
    for (int b = 0; b < 4; ++b) out[b]   = (cnt >> (b*8)) & 0xFF;
    for (int b = 0; b < 4; ++b) out[4+b] = (len >> (b*8)) & 0xFF;
    std::memcpy(out.data() + 8, comp_ptr, comp_len);
    pco_free(comp_ptr, comp_len);
    return out;
}

inline std::vector<double> decode(const uint8_t* buf, size_t byte_len) {
    if (byte_len < 8) return {};
    uint32_t n   = static_cast<uint32_t>(buf[0]) | (uint32_t(buf[1])<<8) | (uint32_t(buf[2])<<16) | (uint32_t(buf[3])<<24);
    uint32_t len = static_cast<uint32_t>(buf[4]) | (uint32_t(buf[5])<<8) | (uint32_t(buf[6])<<16) | (uint32_t(buf[7])<<24);
    if (byte_len < 8 + len) return {};

    double* vals_ptr = nullptr;
    size_t  vals_n   = 0;
    int rc = pco_decompress_f64(buf + 8, len, &vals_ptr, &vals_n);
    if (rc != 0 || vals_ptr == nullptr) {
        std::fprintf(stderr, "[pcodec] pco_decompress_f64 failed (rc=%d)\n", rc);
        return {};
    }
    std::vector<double> out(vals_ptr, vals_ptr + vals_n);
    pco_free_f64(vals_ptr, vals_n);
    (void)n; // vals_n from pco is authoritative
    return out;
}

} // namespace pcodec
