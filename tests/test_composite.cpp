// Copyright 2026 Anuj Attri
// Licensed under the Apache License, Version 2.0 (the "License");
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include "lns/composite.hpp"
#include <random>
#include <cmath>
#include <vector>
#include <algorithm>

using namespace composite;

// Max relative error allowed for each Q-format (matches lns_codec.hpp spec)
static constexpr double TOL_Q8_24  = 1e-6; // slightly looser than RMS spec for per-value max
static constexpr double TOL_Q10_22 = 1e-5;
static constexpr double TOL_Q12_20 = 1e-4;
static constexpr double TOL_Q12_16 = 1e-3;

template <int I, int F>
static void check_roundtrip(const std::vector<double>& data, double max_rel_tol) {
    auto enc = LnsGorilla<I,F>::encode(data.data(), data.size());
    auto dec = LnsGorilla<I,F>::decode(enc.data(), enc.size());
    REQUIRE(dec.size() == data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == 0.0) {
            REQUIRE(dec[i] == 0.0);
            continue;
        }
        if (std::isnan(data[i])) {
            REQUIRE(std::isnan(dec[i]));
            continue;
        }
        if (std::isinf(data[i])) {
            REQUIRE(std::isinf(dec[i]));
            continue;
        }
        double rel = std::abs((dec[i] - data[i]) / data[i]);
        INFO("i=" << i << " orig=" << data[i] << " recon=" << dec[i] << " rel=" << rel);
        REQUIRE(rel < max_rel_tol);
    }
}

TEST_CASE("Composite Q8.24 round-trip on multiplicative data", "[composite][q8_24]") {
    std::mt19937_64 rng(1);
    std::normal_distribution<double> step(0.0, 0.01);
    std::vector<double> data(10'000);
    data[0] = 100.0;
    for (size_t i = 1; i < data.size(); ++i)
        data[i] = data[i-1] * std::exp(step(rng));
    check_roundtrip<8,24>(data, TOL_Q8_24);
}

TEST_CASE("Composite Q10.22 round-trip on multiplicative data", "[composite][q10_22]") {
    std::mt19937_64 rng(2);
    std::normal_distribution<double> step(0.0, 0.01);
    std::vector<double> data(10'000);
    data[0] = 50.0;
    for (size_t i = 1; i < data.size(); ++i)
        data[i] = data[i-1] * std::exp(step(rng));
    check_roundtrip<10,22>(data, TOL_Q10_22);
}

TEST_CASE("Composite Q12.20 round-trip on multiplicative data", "[composite][q12_20]") {
    std::mt19937_64 rng(3);
    std::normal_distribution<double> step(0.0, 0.01);
    std::vector<double> data(10'000);
    data[0] = 200.0;
    for (size_t i = 1; i < data.size(); ++i)
        data[i] = data[i-1] * std::exp(step(rng));
    check_roundtrip<12,20>(data, TOL_Q12_20);
}

TEST_CASE("Composite Q12.16 round-trip on multiplicative data", "[composite][q12_16]") {
    std::mt19937_64 rng(4);
    std::normal_distribution<double> step(0.0, 0.01);
    std::vector<double> data(10'000);
    data[0] = 75.0;
    for (size_t i = 1; i < data.size(); ++i)
        data[i] = data[i-1] * std::exp(step(rng));
    check_roundtrip<12,16>(data, TOL_Q12_16);
}

TEST_CASE("Composite on additive data", "[composite][additive]") {
    std::mt19937_64 rng(5);
    std::normal_distribution<double> step(0.0, 1.0);
    std::vector<double> data(5000);
    data[0] = 1000.0;
    for (size_t i = 1; i < data.size(); ++i)
        data[i] = data[i-1] + step(rng);
    // Additive data: values stay positive, should still compress
    // Just verify round-trip correctness
    check_roundtrip<8,24>(data, TOL_Q8_24);
}

TEST_CASE("Composite on constant sequence", "[composite][edge]") {
    std::vector<double> data(1024, 42.0);
    check_roundtrip<8,24>(data, 1e-8);
}

TEST_CASE("Composite on empty input", "[composite][edge]") {
    auto enc = LnsGorilla_Q8_24::encode(nullptr, 0);
    auto dec = LnsGorilla_Q8_24::decode(enc.data(), enc.size());
    REQUIRE(dec.empty());
}

TEST_CASE("Composite on single value", "[composite][edge]") {
    double val = 3.14159;
    auto enc = LnsGorilla_Q8_24::encode(&val, 1);
    auto dec = LnsGorilla_Q8_24::decode(enc.data(), enc.size());
    REQUIRE(dec.size() == 1);
    REQUIRE(std::abs((dec[0] - val) / val) < TOL_Q8_24);
}

TEST_CASE("Composite handles special values gracefully", "[composite][special]") {
    std::vector<double> data = {
        0.0, 1.0, -1.0, 100.0,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
        1e-300, 1e300
    };
    // Should not crash; decode should produce same special values
    auto enc = LnsGorilla_Q8_24::encode(data.data(), data.size());
    auto dec = LnsGorilla_Q8_24::decode(enc.data(), enc.size());
    REQUIRE(dec.size() == data.size());
    REQUIRE(dec[0] == 0.0);
    REQUIRE(std::isinf(dec[4]));
    REQUIRE(dec[4] > 0);
    REQUIRE(std::isinf(dec[5]));
    REQUIRE(dec[5] < 0);
    REQUIRE(std::isnan(dec[6]));
}

TEST_CASE("Composite compression ratio > 2 on low-volatility stock-like data", "[composite][ratio]") {
    // σ=0.001 multiplicative — should compress extremely well in LNS space
    std::mt19937_64 rng(77);
    std::normal_distribution<double> step(0.0, 0.001);
    std::vector<double> data(1024);
    data[0] = 150.0;
    for (size_t i = 1; i < data.size(); ++i)
        data[i] = data[i-1] * std::exp(step(rng));

    auto enc = LnsGorilla_Q8_24::encode(data.data(), data.size());
    double ratio = static_cast<double>(data.size() * 8) / enc.size();
    INFO("Composite ratio on low-vol multiplicative = " << ratio);
    // We expect > 2:1 even at this test scale
    REQUIRE(ratio > 2.0);
}
