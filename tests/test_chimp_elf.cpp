// Copyright 2026 Anuj Attri
// SPDX-License-Identifier: Apache-2.0
// M3 smoke tests: Chimp128 and Elf round-trip on 10K values.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <lns/chimp128_codec.hpp>
#include <lns/elf_codec.hpp>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

namespace {

std::vector<double> make_walk(size_t n, double sigma = 0.01, uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> nd(0.0, sigma);
    std::vector<double> v(n);
    double x = 100.0;
    for (auto& d : v) { x *= std::exp(nd(rng)); d = x; }
    return v;
}

std::vector<double> make_decimal(size_t n) {
    std::vector<double> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = std::round((50.0 + i * 0.1) * 100.0) / 100.0; // 2 d.p.
    return v;
}

std::vector<uint64_t> doubles_to_u64(const std::vector<double>& d) {
    std::vector<uint64_t> u(d.size());
    for (size_t i = 0; i < d.size(); ++i) std::memcpy(&u[i], &d[i], 8);
    return u;
}

} // namespace

// ─── Chimp128 round-trip ────────────────────────────────────────────────────

TEST_CASE("Chimp128: round-trip on 10K random walk") {
    auto data = make_walk(10000);
    auto u64  = doubles_to_u64(data);
    auto enc  = chimp128::encode(u64.data(), u64.size());
    auto dec  = chimp128::decode(enc.data(), enc.size());

    REQUIRE(dec.size() == u64.size());
    for (size_t i = 0; i < u64.size(); ++i)
        CHECK(dec[i] == u64[i]);
}

TEST_CASE("Chimp128: round-trip on 10K decimal prices") {
    auto data = make_decimal(10000);
    auto u64  = doubles_to_u64(data);
    auto enc  = chimp128::encode(u64.data(), u64.size());
    auto dec  = chimp128::decode(enc.data(), enc.size());

    REQUIRE(dec.size() == u64.size());
    for (size_t i = 0; i < u64.size(); ++i)
        CHECK(dec[i] == u64[i]);
}

TEST_CASE("Chimp128: round-trip on empty input") {
    auto enc = chimp128::encode(nullptr, 0);
    auto dec = chimp128::decode(enc.data(), enc.size());
    CHECK(dec.empty());
}

TEST_CASE("Chimp128: round-trip on single value") {
    double v  = 3.14159265358979;
    uint64_t u; std::memcpy(&u, &v, 8);
    auto enc = chimp128::encode(&u, 1);
    auto dec = chimp128::decode(enc.data(), enc.size());
    REQUIRE(dec.size() == 1);
    CHECK(dec[0] == u);
}

TEST_CASE("Chimp128: round-trip on constant series") {
    std::vector<uint64_t> v(1024);
    double c = 42.0;
    uint64_t cu; std::memcpy(&cu, &c, 8);
    std::fill(v.begin(), v.end(), cu);
    auto enc = chimp128::encode(v.data(), v.size());
    auto dec = chimp128::decode(enc.data(), enc.size());
    REQUIRE(dec.size() == v.size());
    for (auto x : dec) CHECK(x == cu);
}

TEST_CASE("Chimp128: encoded size is smaller than raw on correlated data") {
    // Chimp128 adds 7-bit cache-reference overhead per block. On a smooth
    // random walk the cache never helps, so compression is modest.
    // Just verify we beat raw (ratio > 1.0) as a basic sanity check.
    auto data = make_walk(1024);
    auto u64  = doubles_to_u64(data);
    auto enc  = chimp128::encode(u64.data(), u64.size());
    CHECK(enc.size() < u64.size() * 8);
}

// ─── Elf round-trip ─────────────────────────────────────────────────────────

TEST_CASE("Elf: round-trip on 10K random walk") {
    auto data = make_walk(10000);
    auto enc  = elf::encode(data.data(), data.size());
    auto dec  = elf::decode(enc.data(), enc.size());

    REQUIRE(dec.size() == data.size());
    for (size_t i = 0; i < data.size(); ++i)
        CHECK(dec[i] == data[i]);
}

TEST_CASE("Elf: round-trip on 10K decimal prices") {
    auto data = make_decimal(10000);
    auto enc  = elf::encode(data.data(), data.size());
    auto dec  = elf::decode(enc.data(), enc.size());

    REQUIRE(dec.size() == data.size());
    for (size_t i = 0; i < data.size(); ++i)
        CHECK(dec[i] == data[i]);
}

TEST_CASE("Elf: round-trip on empty input") {
    auto enc = elf::encode(nullptr, 0);
    auto dec = elf::decode(enc.data(), enc.size());
    CHECK(dec.empty());
}

TEST_CASE("Elf: round-trip on single value") {
    double v  = 3.14;
    auto enc = elf::encode(&v, 1);
    auto dec = elf::decode(enc.data(), enc.size());
    REQUIRE(dec.size() == 1);
    CHECK(dec[0] == v);
}

TEST_CASE("Elf: round-trip on NaN and Inf") {
    std::vector<double> v = {
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        0.0, -0.0, 1.0, -1.0
    };
    auto enc = elf::encode(v.data(), v.size());
    auto dec = elf::decode(enc.data(), enc.size());
    REQUIRE(dec.size() == v.size());
    // NaN: compare bits, not value (NaN != NaN).
    for (size_t i = 0; i < v.size(); ++i) {
        uint64_t orig, recovered;
        std::memcpy(&orig,      &v[i],   8);
        std::memcpy(&recovered, &dec[i], 8);
        CHECK(orig == recovered);
    }
}

TEST_CASE("Elf: find_beta is correct") {
    CHECK(elf::find_beta(0.0)      == elf::BETA_VERBATIM); // -0.0 safety
    CHECK(elf::find_beta(1.5)      == 1);
    CHECK(elf::find_beta(3.14)     == 2);
    CHECK(elf::find_beta(100.0)    == 0);
    CHECK(elf::find_beta(142.33)   == 2);
    CHECK(elf::find_beta(0.001)    == 3);
    // 1.0/3.0 in IEEE 754 is representable exactly at beta=16; VERBATIM not expected.
    CHECK(elf::find_beta(1.0/3.0) <= 18);
    // Values that truly cannot be represented: NaN/Inf.
    CHECK(elf::find_beta(std::numeric_limits<double>::quiet_NaN()) == elf::BETA_VERBATIM);
    CHECK(elf::find_beta(std::numeric_limits<double>::infinity())  == elf::BETA_VERBATIM);
}

TEST_CASE("Elf: compresses decimal prices better than random walk") {
    auto decimal = make_decimal(1024);
    auto walk    = make_walk(1024);

    auto enc_d = elf::encode(decimal.data(), decimal.size());
    auto enc_w = elf::encode(walk.data(),    walk.size());

    double ratio_d = static_cast<double>(decimal.size() * 8) / enc_d.size();
    double ratio_w = static_cast<double>(walk.size()    * 8) / enc_w.size();

    // Decimal prices have short beta (β≤2), so Elf eliminates many trailing bits.
    CHECK(ratio_d > ratio_w);
}
