// Copyright 2026 Anuj Attri
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <lns/xor_locality_probe.hpp>
#include <cmath>
#include <random>
#include <vector>

namespace {

std::vector<double> make_random_walk(size_t n, double sigma, uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> nd(0.0, sigma);
    std::vector<double> v(n);
    double x = 100.0;
    for (auto& d : v) { x *= std::exp(nd(rng)); d = x; }
    return v;
}

std::vector<double> make_contaminated(size_t n, uint64_t seed = 42) {
    auto base = make_random_walk(n, 0.01, seed);
    std::mt19937_64 rng(seed + 1);
    std::uniform_real_distribution<double> adj(0.95, 1.05);
    for (auto& v : base) v *= adj(rng);
    return base;
}

} // namespace

TEST_CASE("probe: degenerate input") {
    SECTION("empty") {
        auto s = lns::probe(nullptr, 0);
        CHECK(s.mean_significant_bits == 64.0);
        CHECK(s.mantissa_entropy_bits == 26.0);
    }
    SECTION("single value") {
        double d = 42.0;
        auto s = lns::probe(&d, 1);
        CHECK(s.mean_significant_bits == 64.0);
        CHECK(s.mantissa_entropy_bits == 26.0);
    }
}

TEST_CASE("probe: constant series has perfect XOR locality") {
    std::vector<double> v(1024, 3.14159265358979);
    auto s = lns::probe(v.data(), v.size());
    CHECK(s.mean_leading_zeros  == Catch::Approx(64.0));
    CHECK(s.mean_trailing_zeros == Catch::Approx(0.0));
    CHECK(s.mean_significant_bits == Catch::Approx(0.0));
}

TEST_CASE("probe: clean random walk has higher LZ than contaminated") {
    // Clean: small consecutive steps -> many shared leading bits in XOR.
    // Contaminated: each value multiplied by an independent per-sample factor
    //   -> XOR locality destroyed, mean_leading_zeros drops.
    auto clean = make_random_walk(1024, 0.01);
    auto dirty = make_contaminated(1024);

    auto sc = lns::probe(clean.data(), clean.size());
    auto sd = lns::probe(dirty.data(), dirty.size());

    CHECK(sc.mean_leading_zeros > sd.mean_leading_zeros);
    CHECK(sc.mean_significant_bits < sd.mean_significant_bits);
}

TEST_CASE("probe: decimal-aligned prices have lower mantissa entropy than multiplied prices") {
    // Decimal-priced stocks (e.g. 100.01, 100.02, ...) stored as IEEE 754 doubles
    // have many zeros in the low mantissa bits -> low entropy.
    // The same values multiplied by per-sample adjustment factors fill all mantissa
    // bits with rounding noise -> high entropy.
    const size_t N = 1024;
    std::vector<double> clean(N), dirty(N);
    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> adj(0.9, 1.1);
    // Prices are multiples of 0.01 in [100, 200]
    for (size_t i = 0; i < N; ++i) {
        double price = std::round((100.0 + i * 0.1) * 100.0) / 100.0; // 2 d.p.
        clean[i] = price;
        dirty[i] = price * adj(rng); // per-sample random factor injects mantissa noise
    }
    auto sc = lns::probe(clean.data(), N);
    auto sd = lns::probe(dirty.data(), N);
    CHECK(sc.mantissa_entropy_bits < sd.mantissa_entropy_bits);
}

TEST_CASE("probe: mantissa entropy in [0, 26]") {
    auto data = make_contaminated(1024);
    auto s = lns::probe(data.data(), data.size());
    CHECK(s.mantissa_entropy_bits >= 0.0);
    CHECK(s.mantissa_entropy_bits <= 26.0);
}

TEST_CASE("probe: samples at most 1024 values from larger array") {
    // Build array where first 1024 are constant, rest are noisy.
    // Probe should see perfect locality.
    std::vector<double> v(4096, 100.0);
    auto noise = make_contaminated(3072);
    for (size_t i = 0; i < 3072; ++i) v[1024 + i] = noise[i];

    auto s = lns::probe(v.data(), v.size());
    CHECK(s.mean_leading_zeros == Catch::Approx(64.0));
}

TEST_CASE("probe: sig_bits = 64 - lz - tz invariant holds on random data") {
    auto data = make_contaminated(512);
    auto s = lns::probe(data.data(), data.size());
    double reconstructed = s.mean_leading_zeros + s.mean_trailing_zeros + s.mean_significant_bits;
    // For pairs where XOR != 0: lz + tz + sig == 64
    // For pairs where XOR == 0: lz == 64, tz == 0, sig == 0, sum == 64
    // So mean sum must equal 64
    CHECK(reconstructed == Catch::Approx(64.0).epsilon(0.01));
}
