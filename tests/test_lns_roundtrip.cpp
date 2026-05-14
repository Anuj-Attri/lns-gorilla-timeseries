#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "lns/lns_codec.hpp"
#include <random>
#include <cmath>
#include <vector>
#include <algorithm>

using namespace lns;

static double rms_rel_error(const std::vector<double>& orig,
                             const std::vector<double>& recon) {
    double sum = 0.0;
    size_t cnt = 0;
    for (size_t i = 0; i < orig.size(); ++i) {
        if (orig[i] == 0.0) continue;
        double rel = (recon[i] - orig[i]) / orig[i];
        sum += rel * rel;
        ++cnt;
    }
    return cnt > 0 ? std::sqrt(sum / cnt) : 0.0;
}

// Generate 10M log-uniform random values in [1e-6, 1e6]
static std::vector<double> gen_log_uniform(size_t n, uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> dist(-6.0, 6.0); // log10 range
    std::vector<double> v(n);
    for (auto& x : v) x = std::pow(10.0, dist(rng));
    return v;
}

template <int I, int F>
static double roundtrip_rms(const std::vector<double>& data) {
    std::vector<double> recon(data.size());
    for (size_t i = 0; i < data.size(); ++i)
        recon[i] = decode_lns<I,F>(encode_lns<I,F>(data[i]));
    return rms_rel_error(data, recon);
}

TEST_CASE("LNS round-trip RMS relative error within spec", "[lns][roundtrip]") {
    constexpr size_t N = 10'000'000;
    auto data = gen_log_uniform(N);

    SECTION("Q8.24 < 1e-7") {
        double rms = roundtrip_rms<8, 24>(data);
        INFO("Q8.24 RMS rel error = " << rms);
        REQUIRE(rms < 1e-7);
    }
    SECTION("Q10.22 < 1e-6") {
        double rms = roundtrip_rms<10, 22>(data);
        INFO("Q10.22 RMS rel error = " << rms);
        REQUIRE(rms < 1e-6);
    }
    SECTION("Q12.20 < 1e-5") {
        double rms = roundtrip_rms<12, 20>(data);
        INFO("Q12.20 RMS rel error = " << rms);
        REQUIRE(rms < 1e-5);
    }
    SECTION("Q12.16 < 1e-4") {
        double rms = roundtrip_rms<12, 16>(data);
        INFO("Q12.16 RMS rel error = " << rms);
        REQUIRE(rms < 1e-4);
    }
}

TEST_CASE("LNS handles special values", "[lns][special]") {
    SECTION("Zero") {
        auto v = encode_lns<8,24>(0.0);
        REQUIRE(v.is_zero());
        REQUIRE(decode_lns<8,24>(v) == 0.0);
    }
    SECTION("Negative zero") {
        auto v = encode_lns<8,24>(-0.0);
        REQUIRE(v.is_zero());
    }
    SECTION("NaN") {
        auto v = encode_lns<8,24>(std::numeric_limits<double>::quiet_NaN());
        REQUIRE(v.is_nan());
        REQUIRE(std::isnan(decode_lns<8,24>(v)));
    }
    SECTION("Positive infinity") {
        auto v = encode_lns<8,24>(std::numeric_limits<double>::infinity());
        REQUIRE(v.is_inf());
        REQUIRE(!v.is_negative());
        REQUIRE(std::isinf(decode_lns<8,24>(v)));
    }
    SECTION("Negative infinity") {
        auto v = encode_lns<8,24>(-std::numeric_limits<double>::infinity());
        REQUIRE(v.is_inf());
        REQUIRE(v.is_negative());
        REQUIRE(decode_lns<8,24>(v) < 0);
    }
    SECTION("Negative input") {
        double x = -3.14;
        auto v = encode_lns<8,24>(x);
        REQUIRE(v.is_negative());
        double r = decode_lns<8,24>(v);
        REQUIRE(r < 0.0);
        REQUIRE(std::abs((r - x) / x) < 1e-6);
    }
    SECTION("Very small value (near zero)") {
        double x = 1e-300;
        // May clamp but should not crash
        auto v = encode_lns<8,24>(x);
        REQUIRE_NOTHROW(decode_lns<8,24>(v));
    }
    SECTION("Sign flips in sequence") {
        std::vector<double> seq = {1.0, -1.0, 2.0, -2.0, 0.5, -0.5};
        for (double x : seq) {
            auto v = encode_lns<8,24>(x);
            double r = decode_lns<8,24>(v);
            if (x < 0) REQUIRE(r < 0);
            else REQUIRE(r > 0);
        }
    }
}

TEST_CASE("LNS Q15.48 round-trip on f64-class data", "[lns][q15_48]") {
    // Q15.48 (63-bit total): very high precision for f64-class inputs
    // (Q16.48 would need 64 bits, overflowing int64_t)
    std::vector<double> data = {1.0, 1.5, 2.7182818, 3.14159265, 1e10, 1e-10};
    for (double x : data) {
        auto v = encode_lns<15,48>(x);
        double r = decode_lns<15,48>(v);
        double rel = std::abs((r - x) / x);
        INFO("x=" << x << " r=" << r << " rel=" << rel);
        REQUIRE(rel < 1e-14); // much tighter than Q8.24
    }
}
