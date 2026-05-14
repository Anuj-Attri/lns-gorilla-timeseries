// Copyright 2026 Anuj Attri
// Licensed under the Apache License, Version 2.0 (the "License");
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
// SPDX-License-Identifier: Apache-2.0
// ratio_sweep.cpp — compression ratio table generator.
// Sweeps over (codec, dataset, Q-format) and prints a CSV + ASCII table.
// Not part of the Google Benchmark loop; runs directly.
//
// Usage: ./ratio_sweep [data_dir]
//   data_dir defaults to ./data

#include "lns/lns_codec.hpp"
#include "lns/gorilla_codec.hpp"
#include "lns/composite.hpp"
#include "lns/ieee_baseline.hpp"

#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <numeric>
#include <random>

namespace fs = std::filesystem;

// ── Data helpers ──────────────────────────────────────────────────────────────

static std::vector<double> load_bin(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<double> v(sz / 8);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size() * 8));
    return v;
}

static std::vector<double> gen_multiplicative(size_t n, double sigma, uint64_t seed = 1) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> d(0.0, sigma);
    std::vector<double> v(n);
    v[0] = 100.0;
    for (size_t i = 1; i < n; ++i) v[i] = v[i-1] * std::exp(d(rng));
    return v;
}

static std::vector<double> gen_additive(size_t n, double sigma, uint64_t seed = 2) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> d(0.0, sigma);
    std::vector<double> v(n);
    v[0] = 1000.0;
    for (size_t i = 1; i < n; ++i) v[i] = v[i-1] + d(rng);
    return v;
}

static std::vector<uint64_t> as_u64(const std::vector<double>& v) {
    std::vector<uint64_t> u(v.size());
    for (size_t i = 0; i < v.size(); ++i) std::memcpy(&u[i], &v[i], 8);
    return u;
}

// ── Error metrics ─────────────────────────────────────────────────────────────

static double rmse(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return 1e30;
    double s = 0;
    for (size_t i = 0; i < a.size(); ++i) { double d = b[i]-a[i]; s += d*d; }
    return std::sqrt(s / a.size());
}

static double max_rel(const std::vector<double>& a, const std::vector<double>& b) {
    double m = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (a[i] != 0.0) m = std::max(m, std::abs((b[i]-a[i])/a[i]));
    return m;
}

// ── Row struct ────────────────────────────────────────────────────────────────

struct Row {
    std::string codec;
    std::string dataset;
    size_t n;
    double ratio;
    double rmse_val;
    double max_rel_val;
};

// ── Codec runners ─────────────────────────────────────────────────────────────

static Row run_ieee(const std::string& ds_name, const std::vector<double>& data) {
    auto enc = baseline::encode(data.data(), data.size());
    auto dec = baseline::decode(enc.data(), enc.size());
    return {"IEEE_baseline", ds_name, data.size(),
            static_cast<double>(data.size()*8) / enc.size(),
            rmse(data, dec), max_rel(data, dec)};
}

static Row run_gorilla(const std::string& ds_name, const std::vector<double>& data) {
    auto u = as_u64(data);
    auto enc = gorilla::encode(u.data(), u.size());
    auto dec_u = gorilla::decode(enc.data(), enc.size());
    std::vector<double> dec(dec_u.size());
    for (size_t i = 0; i < dec_u.size(); ++i) std::memcpy(&dec[i], &dec_u[i], 8);
    return {"Gorilla", ds_name, data.size(),
            static_cast<double>(data.size()*8) / enc.size(),
            rmse(data, dec), max_rel(data, dec)};
}

template <int I, int F>
static Row run_lns_gorilla(const std::string& codec_name,
                            const std::string& ds_name,
                            const std::vector<double>& data) {
    auto enc = composite::LnsGorilla<I,F>::encode(data.data(), data.size());
    auto dec = composite::LnsGorilla<I,F>::decode(enc.data(), enc.size());
    return {codec_name, ds_name, data.size(),
            static_cast<double>(data.size()*8) / enc.size(),
            rmse(data, dec), max_rel(data, dec)};
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    fs::path data_dir = (argc > 1) ? argv[1] : "data";

    // Collect datasets: synthetic + any .bin files found in data/
    struct DS { std::string name; std::vector<double> data; std::string kind; };
    std::vector<DS> datasets;

    // Synthetic
    for (double sigma : {0.001, 0.01, 0.1}) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "mult_sigma%.3f", sigma);
        datasets.push_back({buf, gen_multiplicative(1'000'000, sigma), "mult"});
    }
    for (double sigma : {0.1, 1.0, 10.0}) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "add_sigma%.1f", sigma);
        datasets.push_back({buf, gen_additive(1'000'000, sigma), "add"});
    }

    // Real data from data/
    if (fs::exists(data_dir)) {
        for (auto& entry : fs::recursive_directory_iterator(data_dir)) {
            if (entry.path().extension() == ".bin") {
                auto v = load_bin(entry.path());
                if (v.size() >= 1024) {
                    datasets.push_back({entry.path().stem().string(), std::move(v), "real"});
                }
            }
        }
    }

    std::vector<Row> rows;
    for (auto& ds : datasets) {
        if (ds.data.empty()) continue;
        rows.push_back(run_ieee(ds.name, ds.data));
        rows.push_back(run_gorilla(ds.name, ds.data));
        rows.push_back(run_lns_gorilla<8,  24>("LNS_Q8_24_Gorilla",  ds.name, ds.data));
        rows.push_back(run_lns_gorilla<10, 22>("LNS_Q10_22_Gorilla", ds.name, ds.data));
        rows.push_back(run_lns_gorilla<12, 20>("LNS_Q12_20_Gorilla", ds.name, ds.data));
        rows.push_back(run_lns_gorilla<12, 16>("LNS_Q12_16_Gorilla", ds.name, ds.data));
    }

    // ── CSV output ────────────────────────────────────────────────────────────
    fs::create_directories("results");
    std::ofstream csv("results/ratio_sweep.csv");
    csv << "codec,dataset,n_values,ratio,rmse,max_rel_err\n";
    for (auto& r : rows) {
        csv << r.codec << "," << r.dataset << "," << r.n << ","
            << std::fixed << std::setprecision(4) << r.ratio << ","
            << std::scientific << std::setprecision(4) << r.rmse_val << ","
            << r.max_rel_val << "\n";
    }
    std::cout << "Wrote results/ratio_sweep.csv\n";

    // ── ASCII table ───────────────────────────────────────────────────────────
    std::cout << "\n"
              << std::left << std::setw(26) << "Codec"
              << std::setw(28) << "Dataset"
              << std::right << std::setw(10) << "Ratio"
              << std::setw(16) << "RMSE"
              << std::setw(16) << "MaxRelErr"
              << "\n"
              << std::string(96, '-') << "\n";
    for (auto& r : rows) {
        std::cout << std::left  << std::setw(26) << r.codec
                  << std::setw(28) << r.dataset
                  << std::right << std::setw(10) << std::fixed << std::setprecision(3) << r.ratio
                  << std::setw(16) << std::scientific << std::setprecision(2) << r.rmse_val
                  << std::setw(16) << r.max_rel_val << "\n";
    }
    return 0;
}
