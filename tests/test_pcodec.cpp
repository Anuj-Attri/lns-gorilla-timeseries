// Copyright 2026 Anuj Attri
// SPDX-License-Identifier: Apache-2.0
// M3 smoke test: Pcodec round-trip on 10K doubles.
#include <catch2/catch_test_macros.hpp>

#ifndef LNS_HAS_PCO
#  define LNS_HAS_PCO 0
#endif

#if LNS_HAS_PCO
#include <lns/pcodec_codec.hpp>
#include <cmath>
#include <random>
#include <vector>

namespace {
std::vector<double> make_walk(size_t n) {
    std::mt19937_64 rng(42);
    std::normal_distribution<double> nd(0.0, 0.01);
    std::vector<double> v(n);
    double x = 100.0;
    for (auto& d : v) { x *= std::exp(nd(rng)); d = x; }
    return v;
}
} // namespace

TEST_CASE("Pcodec: round-trip on 10K random walk") {
    auto data = make_walk(10000);
    auto enc  = pcodec::encode(data.data(), data.size());
    auto dec  = pcodec::decode(enc.data(), enc.size());

    REQUIRE(dec.size() == data.size());
    for (size_t i = 0; i < data.size(); ++i)
        CHECK(dec[i] == data[i]);
}

TEST_CASE("Pcodec: achieves > 1.1x compression on random walk") {
    // Pcodec excels on data with latent integer structure (e.g. sensor readings).
    // On a log-normal random walk (no integer structure), expect modest gains.
    auto data = make_walk(1024);
    auto enc  = pcodec::encode(data.data(), data.size());
    double ratio = static_cast<double>(data.size() * 8) / enc.size();
    CHECK(ratio > 1.1);
}

#else

TEST_CASE("Pcodec: skipped (LNS_HAS_PCO=0)") {
    WARN("Pcodec not linked. Run 'cargo build --release' in third_party/pco_wrapper, then reconfigure.");
}

#endif
