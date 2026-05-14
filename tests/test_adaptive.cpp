// Copyright 2026 Anuj Attri
// SPDX-License-Identifier: Apache-2.0
// M6 smoke tests: adaptive codec round-trip and routing.
#include <catch2/catch_test_macros.hpp>
#include <lns/adaptive_codec.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace {

// XOR-friendly: integer-valued prices (exact IEEE 754, zero mantissa entropy).
// Random walk in integer space; no irrational fractions.
std::vector<double> make_clean_prices(size_t n) {
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> tick(-1, 1);
    std::vector<double> v(n);
    int price = 10000; // integer cents: $100.00 without division
    for (auto& d : v) {
        price = std::max(100, price + tick(rng));
        d = static_cast<double>(price); // exact integer, zero mantissa fraction bits
    }
    return v;
}

// XOR-hostile: multiply integer prices by an irrational factor.
// The irrational multiply fills all 52 mantissa bits with pseudo-random noise.
std::vector<double> make_contaminated(size_t n) {
    auto clean = make_clean_prices(n);
    const double adj_factor = 1.0 / 7.0; // exactly irrational in base 2
    for (auto& v : clean) v *= adj_factor;
    return clean;
}

} // namespace

TEST_CASE("Adaptive: round-trip on XOR-friendly data") {
    auto data = make_clean_prices(10000);
    auto enc  = adaptive::encode(data.data(), data.size());
    auto dec  = adaptive::decode(enc.data(), enc.size());

    REQUIRE(dec.size() == data.size());
    // Chimp128 is bit-exact lossless; clean integer prices should round-trip exactly.
    for (size_t i = 0; i < data.size(); ++i)
        CHECK(dec[i] == data[i]);
}

TEST_CASE("Adaptive: routes XOR-friendly to Chimp128") {
    auto data = make_clean_prices(2048);
    auto enc  = adaptive::encode(data.data(), data.size());
    REQUIRE(!enc.empty());
    CHECK(enc[0] == adaptive::ID_CHIMP128);
}

TEST_CASE("Adaptive: routes XOR-hostile to LNS") {
    auto data = make_contaminated(2048);
    auto enc  = adaptive::encode(data.data(), data.size());
    REQUIRE(!enc.empty());
    CHECK(enc[0] == adaptive::ID_LNS_Q10_22);
}

TEST_CASE("Adaptive: compression ratio on XOR-hostile > 1.3x") {
    // Sanity: even lossy LNS should compress contaminated data.
    auto data = make_contaminated(10000);
    auto enc  = adaptive::encode(data.data(), data.size());
    double ratio = static_cast<double>(data.size() * 8) / enc.size();
    CHECK(ratio > 1.3);
}

TEST_CASE("Adaptive: empty input") {
    std::vector<double> empty;
    auto enc = adaptive::encode(empty.data(), 0);
    auto dec = adaptive::decode(enc.data(), enc.size());
    CHECK(dec.empty());
}

TEST_CASE("Adaptive: single value") {
    std::vector<double> one = {42.0};
    auto enc = adaptive::encode(one.data(), 1);
    auto dec = adaptive::decode(enc.data(), enc.size());
    REQUIRE(dec.size() == 1);
    // Could be either codec; just check it decodes to reasonable value
    CHECK(std::isfinite(dec[0]));
}

TEST_CASE("Adaptive: is_xor_hostile probe") {
    auto clean = make_clean_prices(2048);
    auto dirty = make_contaminated(2048);
    CHECK_FALSE(adaptive::is_xor_hostile(clean.data(), clean.size()));
    CHECK(adaptive::is_xor_hostile(dirty.data(), dirty.size()));
}
