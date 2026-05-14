// Copyright 2026 Anuj Attri
// Licensed under the Apache License, Version 2.0 (the "License");
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
// SPDX-License-Identifier: Apache-2.0
// stat_sig.cpp — Statistical significance testing for LNS+Gorilla vs Gorilla baseline.
//
// Reads results/ratio_sweep.csv and computes, per multiplicative dataset:
//   1. Wilcoxon signed-rank test on per-window (1024 value) compression ratios
//   2. Cohen's d effect size
//   3. 95% CI on ratio improvement via bootstrap (10k resamples)
//
// Rejection criteria (printed as FAIL):
//   p > 0.01  OR  effect size < 0.5  OR  median improvement < 1.5×
//
// Usage: ./stat_sig [ratio_sweep.csv]

#include "lns/lns_codec.hpp"
#include "lns/gorilla_codec.hpp"
#include "lns/composite.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include <map>

// ── Per-window ratio computation ──────────────────────────────────────────────

static constexpr size_t WINDOW = 1024;

static std::vector<uint64_t> as_u64(const std::vector<double>& v) {
    std::vector<uint64_t> u(v.size());
    for (size_t i = 0; i < v.size(); ++i) std::memcpy(&u[i], &v[i], 8);
    return u;
}

static std::vector<double> per_window_ratios_gorilla(const std::vector<double>& data) {
    std::vector<double> ratios;
    ratios.reserve(data.size() / WINDOW);
    for (size_t off = 0; off + WINDOW <= data.size(); off += WINDOW) {
        auto u = as_u64(std::vector<double>(data.begin()+off, data.begin()+off+WINDOW));
        auto enc = gorilla::encode(u.data(), u.size());
        ratios.push_back(static_cast<double>(WINDOW * 8) / enc.size());
    }
    return ratios;
}

template <int I, int F>
static std::vector<double> per_window_ratios_lns_gorilla(const std::vector<double>& data) {
    std::vector<double> ratios;
    ratios.reserve(data.size() / WINDOW);
    for (size_t off = 0; off + WINDOW <= data.size(); off += WINDOW) {
        std::vector<double> win(data.begin()+off, data.begin()+off+WINDOW);
        auto enc = composite::LnsGorilla<I,F>::encode(win.data(), win.size());
        ratios.push_back(static_cast<double>(WINDOW * 8) / enc.size());
    }
    return ratios;
}

// ── Wilcoxon signed-rank test ─────────────────────────────────────────────────
// Two-sided test. Returns approximate p-value using normal approximation.

struct WilcoxonResult {
    double W;   // test statistic
    double z;   // z-score
    double p;   // two-sided p-value
};

static WilcoxonResult wilcoxon_signed_rank(const std::vector<double>& a,
                                            const std::vector<double>& b) {
    assert(a.size() == b.size());
    size_t n = a.size();

    // Compute differences, discard zeros
    std::vector<double> diffs;
    for (size_t i = 0; i < n; ++i) {
        double d = a[i] - b[i];
        if (d != 0.0) diffs.push_back(d);
    }

    size_t m = diffs.size();
    if (m == 0) return {0, 0, 1.0};

    // Absolute differences with original signs
    std::vector<std::pair<double, double>> absdiff(m);
    for (size_t i = 0; i < m; ++i)
        absdiff[i] = {std::abs(diffs[i]), (diffs[i] > 0 ? 1.0 : -1.0)};

    // Rank by absolute difference (average ties)
    std::vector<size_t> idx(m);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(),
              [&](size_t a, size_t b){ return absdiff[a].first < absdiff[b].first; });

    std::vector<double> ranks(m);
    for (size_t i = 0; i < m; ) {
        size_t j = i;
        while (j < m && absdiff[idx[j]].first == absdiff[idx[i]].first) ++j;
        double avg_rank = (i + j + 1) / 2.0; // 1-indexed midpoint
        for (size_t k = i; k < j; ++k) ranks[idx[k]] = avg_rank;
        i = j;
    }

    double W_plus = 0, W_minus = 0;
    for (size_t i = 0; i < m; ++i) {
        if (absdiff[i].second > 0) W_plus  += ranks[i];
        else                       W_minus += ranks[i];
    }
    double W = std::min(W_plus, W_minus);

    // Normal approximation: E[W] = m(m+1)/4, Var[W] = m(m+1)(2m+1)/24
    double mu  = static_cast<double>(m) * (m + 1) / 4.0;
    double var = static_cast<double>(m) * (m + 1) * (2*m + 1) / 24.0;
    double z   = (W - mu) / std::sqrt(var);
    // Two-sided p-value via error function
    double p   = 2.0 * (1.0 - 0.5 * std::erfc(-std::abs(z) / std::sqrt(2.0)));
    // erfc approach: p = 2 * Phi(-|z|)
    // Phi(x) = 0.5 * erfc(-x/sqrt(2))
    // Phi(-|z|) = 0.5 * erfc(|z|/sqrt(2))
    p = std::erfc(std::abs(z) / std::sqrt(2.0));

    return {W, z, p};
}

// ── Cohen's d ─────────────────────────────────────────────────────────────────

static double cohens_d(const std::vector<double>& a, const std::vector<double>& b) {
    // d = (mean_a - mean_b) / pooled_std
    size_t n = std::min(a.size(), b.size());
    if (n < 2) return 0;

    auto mean = [](const std::vector<double>& v, size_t nn) {
        double s = 0; for (size_t i = 0; i < nn; ++i) s += v[i]; return s/nn;
    };
    auto var = [&](const std::vector<double>& v, size_t nn, double m) {
        double s = 0;
        for (size_t i = 0; i < nn; ++i) s += (v[i]-m)*(v[i]-m);
        return s/(nn-1);
    };

    double ma = mean(a, n), mb = mean(b, n);
    double va = var(a, n, ma), vb = var(b, n, mb);
    double pooled_std = std::sqrt((va + vb) / 2.0);
    if (pooled_std == 0) return 0;
    return std::abs(ma - mb) / pooled_std;
}

// ── Bootstrap 95% CI ─────────────────────────────────────────────────────────

static std::pair<double,double> bootstrap_ci(const std::vector<double>& improvements,
                                               int resamples = 10000,
                                               uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    size_t n = improvements.size();
    std::uniform_int_distribution<size_t> dist(0, n - 1);

    std::vector<double> medians;
    medians.reserve(resamples);
    std::vector<double> sample(n);
    for (int r = 0; r < resamples; ++r) {
        for (size_t i = 0; i < n; ++i) sample[i] = improvements[dist(rng)];
        std::nth_element(sample.begin(), sample.begin() + n/2, sample.end());
        medians.push_back(sample[n/2]);
    }
    std::sort(medians.begin(), medians.end());
    size_t lo = static_cast<size_t>(resamples * 0.025);
    size_t hi = static_cast<size_t>(resamples * 0.975);
    return {medians[lo], medians[hi]};
}

// ── Synthetic dataset ─────────────────────────────────────────────────────────

static std::vector<double> gen_multiplicative(size_t n, double sigma, uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> d(0.0, sigma);
    std::vector<double> v(n);
    v[0] = 100.0;
    for (size_t i = 1; i < n; ++i) v[i] = v[i-1] * std::exp(d(rng));
    return v;
}

static std::vector<double> gen_additive(size_t n, double sigma, uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> d(0.0, sigma);
    std::vector<double> v(n);
    v[0] = 1000.0;
    for (size_t i = 1; i < n; ++i) v[i] = v[i-1] + d(rng);
    return v;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Statistical Significance: LNS+Gorilla vs Gorilla ===\n\n";

    // Test on synthetic datasets (always available)
    struct DSConfig {
        std::string name;
        std::vector<double> data;
        std::string kind;
    };

    std::vector<DSConfig> configs = {
        {"mult_sigma0.001_1M", gen_multiplicative(1'000'000, 0.001), "mult"},
        {"mult_sigma0.01_1M",  gen_multiplicative(1'000'000, 0.01),  "mult"},
        {"mult_sigma0.1_1M",   gen_multiplicative(1'000'000, 0.1),   "mult"},
        {"add_sigma1.0_1M",    gen_additive(1'000'000, 1.0),         "add" },
        {"add_sigma10.0_1M",   gen_additive(1'000'000, 10.0),        "add" },
    };

    bool any_fail = false;

    for (auto& cfg : configs) {
        std::cout << "── Dataset: " << cfg.name << " (" << cfg.kind << ") ──\n";

        auto gorilla_ratios  = per_window_ratios_gorilla(cfg.data);
        auto lns_ratios      = per_window_ratios_lns_gorilla<8,24>(cfg.data);

        size_t nw = std::min(gorilla_ratios.size(), lns_ratios.size());
        gorilla_ratios.resize(nw);
        lns_ratios.resize(nw);

        // Compute improvements (per window: lns/gorilla ratio improvement factor)
        std::vector<double> improvements(nw);
        for (size_t i = 0; i < nw; ++i)
            improvements[i] = (gorilla_ratios[i] > 0)
                               ? lns_ratios[i] / gorilla_ratios[i]
                               : 1.0;

        double mean_gorilla = std::accumulate(gorilla_ratios.begin(), gorilla_ratios.end(), 0.0) / nw;
        double mean_lns     = std::accumulate(lns_ratios.begin(), lns_ratios.end(), 0.0) / nw;

        // Median improvement
        std::vector<double> sorted_imp = improvements;
        std::nth_element(sorted_imp.begin(), sorted_imp.begin()+nw/2, sorted_imp.end());
        double median_imp = sorted_imp[nw/2];

        // Wilcoxon
        auto wil = wilcoxon_signed_rank(lns_ratios, gorilla_ratios);

        // Cohen's d
        double d = cohens_d(lns_ratios, gorilla_ratios);

        // Bootstrap CI on median improvement
        auto [ci_lo, ci_hi] = bootstrap_ci(improvements, 10000);

        std::cout << "  Windows:           " << nw << "\n"
                  << "  Gorilla mean ratio:" << std::fixed << std::setprecision(4) << mean_gorilla << "\n"
                  << "  LNS mean ratio:    " << mean_lns << "\n"
                  << "  Median improvement:" << median_imp << "×\n"
                  << "  95% CI:            [" << ci_lo << ", " << ci_hi << "]\n"
                  << "  Wilcoxon W:        " << std::setprecision(1) << wil.W << "\n"
                  << "  z-score:           " << std::setprecision(3) << wil.z << "\n"
                  << "  p-value:           " << std::scientific << std::setprecision(3) << wil.p << "\n"
                  << "  Cohen's d:         " << std::fixed << std::setprecision(3) << d << "\n";

        // Rejection criteria
        bool fail_p    = wil.p > 0.01;
        bool fail_d    = d < 0.5;
        bool fail_med  = (cfg.kind == "mult") && median_imp < 1.5;
        bool pass      = !fail_p && !fail_d && !fail_med;

        if (cfg.kind == "mult") {
            if (pass) {
                std::cout << "  RESULT: PASS — hypothesis supported [measured]\n";
            } else {
                std::cout << "  RESULT: FAIL\n";
                if (fail_p)   std::cout << "    FAIL: p > 0.01 (p=" << wil.p << ")\n";
                if (fail_d)   std::cout << "    FAIL: Cohen's d < 0.5 (d=" << d << ")\n";
                if (fail_med) std::cout << "    FAIL: median improvement < 1.5× (" << median_imp << "×)\n";
                any_fail = true;
            }
        } else {
            // Additive: check no regression > 10%
            bool regress = mean_lns < mean_gorilla * 0.9;
            if (regress) {
                std::cout << "  RESULT: ADDITIVE REGRESSION — LNS hurts by >"
                          << std::fixed << std::setprecision(1)
                          << (1.0 - mean_lns/mean_gorilla)*100 << "% [measured]\n";
                any_fail = true;
            } else {
                std::cout << "  RESULT: No regression on additive data [measured]\n";
            }
        }
        std::cout << "\n";
    }

    if (any_fail) {
        std::cout << "Overall: Some tests FAILED — see RESULTS.md for honest reporting.\n";
        return 1;
    }
    std::cout << "Overall: All tests PASSED.\n";
    return 0;
}
