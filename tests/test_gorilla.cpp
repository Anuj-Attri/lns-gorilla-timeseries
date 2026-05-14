// Copyright 2026 Anuj Attri
// Licensed under the Apache License, Version 2.0 (the "License");
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include "lns/gorilla_codec.hpp"
#include <random>
#include <cstring>
#include <cmath>
#include <vector>
#include <limits>

using namespace gorilla;

static std::vector<uint64_t> doubles_as_u64(const std::vector<double>& v) {
    std::vector<uint64_t> out(v.size());
    for (size_t i = 0; i < v.size(); ++i) std::memcpy(&out[i], &v[i], 8);
    return out;
}

static std::vector<double> u64_as_doubles(const std::vector<uint64_t>& v) {
    std::vector<double> out(v.size());
    for (size_t i = 0; i < v.size(); ++i) std::memcpy(&out[i], &v[i], 8);
    return out;
}

TEST_CASE("Gorilla lossless on small constant sequence", "[gorilla][lossless]") {
    std::vector<uint64_t> data = {0x3FF0000000000000ULL, // 1.0
                                   0x3FF0000000000000ULL,
                                   0x3FF0000000000000ULL};
    auto enc = encode(data.data(), data.size());
    auto dec = decode(enc.data(), enc.size());
    REQUIRE(dec == data);
}

TEST_CASE("Gorilla lossless on synthetic additive data", "[gorilla][lossless]") {
    std::mt19937_64 rng(123);
    std::normal_distribution<double> dist(100.0, 1.0);
    std::vector<double> vals(1000);
    vals[0] = 100.0;
    for (size_t i = 1; i < vals.size(); ++i)
        vals[i] = vals[i-1] + dist(rng);

    auto u64 = doubles_as_u64(vals);
    auto enc = encode(u64.data(), u64.size());
    auto dec = decode(enc.data(), enc.size());
    REQUIRE(dec.size() == u64.size());
    REQUIRE(dec == u64); // must be bitwise identical (lossless codec)
}

TEST_CASE("Gorilla lossless on random u64", "[gorilla][lossless]") {
    std::mt19937_64 rng(999);
    std::vector<uint64_t> data(512);
    for (auto& x : data) x = rng();
    auto enc = encode(data.data(), data.size());
    auto dec = decode(enc.data(), enc.size());
    REQUIRE(dec == data);
}

TEST_CASE("Gorilla empty input", "[gorilla][edge]") {
    auto enc = encode(nullptr, 0);
    auto dec = decode(enc.data(), enc.size());
    REQUIRE(dec.empty());
}

TEST_CASE("Gorilla single value", "[gorilla][edge]") {
    uint64_t val = 0xDEADBEEFCAFEBABEULL;
    auto enc = encode(&val, 1);
    auto dec = decode(enc.data(), enc.size());
    REQUIRE(dec.size() == 1);
    REQUIRE(dec[0] == val);
}

TEST_CASE("Gorilla compression ratio > 1 on constant stream", "[gorilla][ratio]") {
    std::vector<uint64_t> data(1024, 0x4059000000000000ULL); // all 100.0
    auto enc = encode(data.data(), data.size());
    double ratio = static_cast<double>(data.size() * 8) / enc.size();
    INFO("Constant stream ratio = " << ratio);
    REQUIRE(ratio > 4.0); // constant stream should compress very well
}

TEST_CASE("Gorilla handles IEEE specials", "[gorilla][special]") {
    std::vector<double> specials = {
        0.0, -0.0, 1.0, -1.0,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()
    };
    auto u64 = doubles_as_u64(specials);
    auto enc = encode(u64.data(), u64.size());
    auto dec = decode(enc.data(), enc.size());
    REQUIRE(dec == u64); // bitwise — NaN identity OK since we compare uint64
}

TEST_CASE("Gorilla large dataset round-trip", "[gorilla][large]") {
    // 100k stock-like multiplicative values
    std::mt19937_64 rng(42);
    std::normal_distribution<double> step(0.0, 0.01);
    std::vector<double> vals(100'000);
    vals[0] = 150.0;
    for (size_t i = 1; i < vals.size(); ++i)
        vals[i] = vals[i-1] * std::exp(step(rng));

    auto u64 = doubles_as_u64(vals);
    auto enc = encode(u64.data(), u64.size());
    auto dec = decode(enc.data(), enc.size());
    REQUIRE(dec == u64);
}
