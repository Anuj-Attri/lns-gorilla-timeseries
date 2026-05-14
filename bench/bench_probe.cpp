// Copyright 2026 Anuj Attri
// SPDX-License-Identifier: Apache-2.0
// M2 gate benchmark: probe() must complete in < 50 us on 1024 doubles.
#include <benchmark/benchmark.h>
#include <lns/xor_locality_probe.hpp>
#include <cmath>
#include <random>
#include <vector>

static std::vector<double> make_clean(size_t n) {
    // Simulates clean multiplicative random walk (high XOR locality)
    std::vector<double> v(n);
    std::mt19937_64 rng(42);
    std::normal_distribution<double> nd(0.0, 0.01);
    double x = 100.0;
    for (auto& d : v) { x *= std::exp(nd(rng)); d = x; }
    return v;
}

static std::vector<double> make_contaminated(size_t n) {
    // Simulates upstream-scalar-multiplied values (low XOR locality)
    std::vector<double> clean = make_clean(n);
    std::vector<double> v(n);
    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> adj(0.95, 1.05);
    for (size_t i = 0; i < n; ++i)
        v[i] = clean[i] * adj(rng); // injection of mantissa noise
    return v;
}

static void BM_ProbeClean(benchmark::State& state) {
    auto data = make_clean(1024);
    for (auto _ : state) {
        auto s = lns::probe(data.data(), data.size());
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations() * 1024);
}

static void BM_ProbeContaminated(benchmark::State& state) {
    auto data = make_contaminated(1024);
    for (auto _ : state) {
        auto s = lns::probe(data.data(), data.size());
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations() * 1024);
}

BENCHMARK(BM_ProbeClean)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ProbeContaminated)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
