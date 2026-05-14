#include <benchmark/benchmark.h>
#include "lns/lns_codec.hpp"
#include "lns/gorilla_codec.hpp"
#include "lns/composite.hpp"
#include "lns/ieee_baseline.hpp"
#include "lns/alp_codec.hpp"

#include <random>
#include <cmath>
#include <cstring>
#include <vector>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <numeric>

namespace fs = std::filesystem;

// ── Data generation ───────────────────────────────────────────────────────────

static std::vector<double> gen_multiplicative(size_t n, double sigma, uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> dist(0.0, sigma);
    std::vector<double> v(n);
    v[0] = 100.0;
    for (size_t i = 1; i < n; ++i)
        v[i] = v[i-1] * std::exp(dist(rng));
    return v;
}

static std::vector<double> gen_additive(size_t n, double sigma, uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> dist(0.0, sigma);
    std::vector<double> v(n);
    v[0] = 1000.0;
    for (size_t i = 1; i < n; ++i)
        v[i] = v[i-1] + dist(rng);
    return v;
}

static std::vector<double> load_bin(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    size_t n = sz / 8;
    std::vector<double> v(n);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n * 8));
    return v;
}

// ── Error metrics ─────────────────────────────────────────────────────────────

struct ErrorStats {
    double rmse;
    double max_rel;
    double max_abs;
};

static ErrorStats compute_errors(const std::vector<double>& orig,
                                  const std::vector<double>& recon) {
    if (orig.size() != recon.size()) return {1e30, 1e30, 1e30};
    double sum_sq = 0, max_rel = 0, max_abs = 0;
    for (size_t i = 0; i < orig.size(); ++i) {
        double diff = recon[i] - orig[i];
        sum_sq += diff * diff;
        max_abs = std::max(max_abs, std::abs(diff));
        if (orig[i] != 0.0)
            max_rel = std::max(max_rel, std::abs(diff / orig[i]));
    }
    return {std::sqrt(sum_sq / orig.size()), max_rel, max_abs};
}

// ── Benchmark fixtures ────────────────────────────────────────────────────────

// Multiplicative, σ = 0.01 (typical daily stock return ~1%)
static auto& mult_data() {
    static auto d = gen_multiplicative(1'000'000, 0.01);
    return d;
}
// Multiplicative, σ = 0.001 (low vol)
static auto& mult_lo() {
    static auto d = gen_multiplicative(1'000'000, 0.001);
    return d;
}
// Multiplicative, σ = 0.1 (high vol)
static auto& mult_hi() {
    static auto d = gen_multiplicative(1'000'000, 0.1);
    return d;
}
// Additive, σ = 1.0
static auto& add_data() {
    static auto d = gen_additive(1'000'000, 1.0);
    return d;
}

// ── IEEE Baseline benchmarks ──────────────────────────────────────────────────

static void BM_IEEE_Encode_Mult(benchmark::State& state) {
    const auto& data = mult_data();
    for (auto _ : state) {
        auto enc = baseline::encode(data.data(), data.size());
        benchmark::DoNotOptimize(enc);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(mult_data().size()));
    state.counters["ratio"] = 1.0;
}
BENCHMARK(BM_IEEE_Encode_Mult)->MinTime(2.0);

static void BM_IEEE_Decode_Mult(benchmark::State& state) {
    const auto& data = mult_data();
    auto enc = baseline::encode(data.data(), data.size());
    for (auto _ : state) {
        auto dec = baseline::decode(enc.data(), enc.size());
        benchmark::DoNotOptimize(dec);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(data.size()));
}
BENCHMARK(BM_IEEE_Decode_Mult)->MinTime(2.0);

// ── Gorilla benchmarks ────────────────────────────────────────────────────────

static std::vector<uint64_t> as_u64(const std::vector<double>& v) {
    std::vector<uint64_t> u(v.size());
    for (size_t i = 0; i < v.size(); ++i) std::memcpy(&u[i], &v[i], 8);
    return u;
}

static void BM_Gorilla_Encode_Mult(benchmark::State& state) {
    auto u64 = as_u64(mult_data());
    for (auto _ : state) {
        auto enc = gorilla::encode(u64.data(), u64.size());
        benchmark::DoNotOptimize(enc);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(u64.size()));
    // Report ratio in counter
    auto enc = gorilla::encode(u64.data(), u64.size());
    state.counters["ratio"] = static_cast<double>(u64.size() * 8) / enc.size();
}
BENCHMARK(BM_Gorilla_Encode_Mult)->MinTime(2.0);

static void BM_Gorilla_Decode_Mult(benchmark::State& state) {
    auto u64 = as_u64(mult_data());
    auto enc = gorilla::encode(u64.data(), u64.size());
    for (auto _ : state) {
        auto dec = gorilla::decode(enc.data(), enc.size());
        benchmark::DoNotOptimize(dec);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(u64.size()));
}
BENCHMARK(BM_Gorilla_Decode_Mult)->MinTime(2.0);

static void BM_Gorilla_Encode_Add(benchmark::State& state) {
    auto u64 = as_u64(add_data());
    for (auto _ : state) {
        auto enc = gorilla::encode(u64.data(), u64.size());
        benchmark::DoNotOptimize(enc);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(u64.size()));
    auto enc = gorilla::encode(u64.data(), u64.size());
    state.counters["ratio"] = static_cast<double>(u64.size() * 8) / enc.size();
}
BENCHMARK(BM_Gorilla_Encode_Add)->MinTime(2.0);

// ── LNS+Gorilla benchmarks ────────────────────────────────────────────────────

#define BM_COMPOSITE_ENCODE(NAME, DATASET, I, F) \
static void BM_LnsGorilla_##NAME##_Encode(benchmark::State& state) { \
    const auto& data = DATASET(); \
    for (auto _ : state) { \
        auto enc = composite::LnsGorilla<I,F>::encode(data.data(), data.size()); \
        benchmark::DoNotOptimize(enc); \
    } \
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(data.size())); \
    auto enc = composite::LnsGorilla<I,F>::encode(data.data(), data.size()); \
    state.counters["ratio"] = static_cast<double>(data.size() * 8) / enc.size(); \
} \
BENCHMARK(BM_LnsGorilla_##NAME##_Encode)->MinTime(2.0)

#define BM_COMPOSITE_DECODE(NAME, DATASET, I, F) \
static void BM_LnsGorilla_##NAME##_Decode(benchmark::State& state) { \
    const auto& data = DATASET(); \
    auto enc = composite::LnsGorilla<I,F>::encode(data.data(), data.size()); \
    for (auto _ : state) { \
        auto dec = composite::LnsGorilla<I,F>::decode(enc.data(), enc.size()); \
        benchmark::DoNotOptimize(dec); \
    } \
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(data.size())); \
} \
BENCHMARK(BM_LnsGorilla_##NAME##_Decode)->MinTime(2.0)

BM_COMPOSITE_ENCODE(Q8_24_Mult,  mult_data, 8,  24);
BM_COMPOSITE_DECODE(Q8_24_Mult,  mult_data, 8,  24);
BM_COMPOSITE_ENCODE(Q8_24_MultLo, mult_lo,  8,  24);
BM_COMPOSITE_DECODE(Q8_24_MultLo, mult_lo,  8,  24);
BM_COMPOSITE_ENCODE(Q8_24_MultHi, mult_hi,  8,  24);
BM_COMPOSITE_DECODE(Q8_24_MultHi, mult_hi,  8,  24);
BM_COMPOSITE_ENCODE(Q8_24_Add,   add_data,  8,  24);
BM_COMPOSITE_DECODE(Q8_24_Add,   add_data,  8,  24);
BM_COMPOSITE_ENCODE(Q10_22_Mult, mult_data, 10, 22);
BM_COMPOSITE_DECODE(Q10_22_Mult, mult_data, 10, 22);
BM_COMPOSITE_ENCODE(Q12_16_Mult, mult_data, 12, 16);
BM_COMPOSITE_DECODE(Q12_16_Mult, mult_data, 12, 16);

// ── Ratio table: print to stdout ──────────────────────────────────────────────
// This is run separately via ratio_sweep, not part of the benchmark loop.

static void BM_Gorilla_Encode_MultLo(benchmark::State& state) {
    auto u64 = as_u64(mult_lo());
    for (auto _ : state) {
        auto enc = gorilla::encode(u64.data(), u64.size());
        benchmark::DoNotOptimize(enc);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(u64.size()));
    auto enc = gorilla::encode(u64.data(), u64.size());
    state.counters["ratio"] = static_cast<double>(u64.size() * 8) / enc.size();
}
BENCHMARK(BM_Gorilla_Encode_MultLo)->MinTime(2.0);

static void BM_Gorilla_Encode_MultHi(benchmark::State& state) {
    auto u64 = as_u64(mult_hi());
    for (auto _ : state) {
        auto enc = gorilla::encode(u64.data(), u64.size());
        benchmark::DoNotOptimize(enc);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(u64.size()));
    auto enc = gorilla::encode(u64.data(), u64.size());
    state.counters["ratio"] = static_cast<double>(u64.size() * 8) / enc.size();
}
BENCHMARK(BM_Gorilla_Encode_MultHi)->MinTime(2.0);
