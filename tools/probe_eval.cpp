// Copyright 2026 Anuj Attri
// SPDX-License-Identifier: Apache-2.0
// probe_eval — per-column probe + codec compression ratio evaluator.
// Reads a float64 .bin file, computes XorLocalityScore, then compresses
// with all five codecs and prints one JSON object per run.
//
// Usage: probe_eval <path.bin> [max_values]
//   max_values defaults to 1,000,000 (first 1M values)
//
// Output: one-line JSON to stdout:
//   {"file":"...", "n":..., "mean_lz":..., "mean_tz":...,
//    "mean_sig":..., "mantissa_entropy":...,
//    "ratio_gorilla":..., "ratio_chimp128":..., "ratio_elf":...,
//    "ratio_lns_q10_22":..., "ratio_pcodec":...}

#include "lns/xor_locality_probe.hpp"
#include "lns/gorilla_codec.hpp"
#include "lns/chimp128_codec.hpp"
#include "lns/elf_codec.hpp"
#include "lns/composite.hpp"

#if LNS_HAS_PCO
#  include "lns/pcodec_codec.hpp"
#endif

#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <cstdio>
#include <algorithm>

namespace fs = std::filesystem;

static std::vector<double> load_bin(const fs::path& p, size_t max_vals) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto bytes = static_cast<size_t>(f.tellg());
    f.seekg(0);
    size_t n = std::min(bytes / 8, max_vals);
    std::vector<double> v(n);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n * 8));
    return v;
}

static std::vector<uint64_t> as_u64(const std::vector<double>& v) {
    std::vector<uint64_t> u(v.size());
    for (size_t i = 0; i < v.size(); ++i) std::memcpy(&u[i], &v[i], 8);
    return u;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: probe_eval <path.bin> [max_values]\n");
        return 1;
    }
    fs::path path = argv[1];
    size_t max_vals = (argc >= 3) ? static_cast<size_t>(std::stoul(argv[2])) : 1'000'000;

    auto data = load_bin(path, max_vals);
    if (data.empty()) {
        std::fprintf(stderr, "Error: could not read %s\n", argv[1]);
        return 1;
    }

    auto score = lns::probe(data.data(), data.size());
    size_t raw_bytes = data.size() * 8;

    // Gorilla
    auto u64 = as_u64(data);
    auto enc_g  = gorilla::encode(u64.data(), u64.size());
    double ratio_gorilla = static_cast<double>(raw_bytes) / enc_g.size();

    // Chimp128
    auto enc_c  = chimp128::encode(u64.data(), u64.size());
    double ratio_chimp   = static_cast<double>(raw_bytes) / enc_c.size();

    // Elf
    auto enc_e  = elf::encode(data.data(), data.size());
    double ratio_elf     = static_cast<double>(raw_bytes) / enc_e.size();

    // LNS Q10.22 + Gorilla
    auto enc_l  = composite::LnsGorilla<10,22>::encode(data.data(), data.size());
    double ratio_lns     = static_cast<double>(raw_bytes) / enc_l.size();

    // Pcodec
    double ratio_pco = 0.0;
#if LNS_HAS_PCO
    auto enc_p   = pcodec::encode(data.data(), data.size());
    ratio_pco    = static_cast<double>(raw_bytes) / enc_p.size();
#endif

    std::string stem = path.stem().string();
    std::printf(
        "{\"file\":\"%s\",\"n\":%zu,"
        "\"mean_lz\":%.4f,\"mean_tz\":%.4f,\"mean_sig\":%.4f,\"mantissa_entropy\":%.4f,"
        "\"ratio_gorilla\":%.4f,\"ratio_chimp128\":%.4f,\"ratio_elf\":%.4f,"
        "\"ratio_lns_q10_22\":%.4f,\"ratio_pcodec\":%.4f}\n",
        stem.c_str(), data.size(),
        score.mean_leading_zeros, score.mean_trailing_zeros,
        score.mean_significant_bits, score.mantissa_entropy_bits,
        ratio_gorilla, ratio_chimp, ratio_elf, ratio_lns, ratio_pco
    );
    return 0;
}
