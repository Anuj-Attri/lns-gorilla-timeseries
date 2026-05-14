// gen_synthetic.cpp — generate synthetic time-series datasets as raw f64 binary files.
//
// Outputs to data/synthetic/<name>.bin (little-endian IEEE 754 double, no header).
// Run: ./gen_synthetic [output_dir]

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void write_bin(const fs::path& path, const std::vector<double>& data) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "Cannot write %s\n", path.string().c_str()); return; }
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size() * 8));
    std::printf("Wrote %zu values → %s\n", data.size(), path.string().c_str());
}

static std::vector<double> gen_multiplicative(size_t n, double sigma, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> d(0.0, sigma);
    std::vector<double> v(n); v[0] = 100.0;
    for (size_t i = 1; i < n; ++i) v[i] = v[i-1] * std::exp(d(rng));
    return v;
}

static std::vector<double> gen_additive(size_t n, double sigma, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> d(0.0, sigma);
    std::vector<double> v(n); v[0] = 1000.0;
    for (size_t i = 1; i < n; ++i) v[i] = v[i-1] + d(rng);
    return v;
}

static std::vector<double> gen_mixed(size_t n, double sigma, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> d(0.0, sigma);
    std::vector<double> v(n); v[0] = 500.0;
    for (size_t i = 1; i < n; ++i) {
        if (i < n/2)
            v[i] = v[i-1] * std::exp(d(rng));  // multiplicative half
        else
            v[i] = v[i-1] + d(rng) * v[i-1];   // additive half (scaled)
    }
    return v;
}

static std::vector<double> gen_constant(size_t n, double val) {
    return std::vector<double>(n, val);
}

static std::vector<double> gen_ramp(size_t n) {
    std::vector<double> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<double>(i);
    return v;
}

static std::vector<double> gen_sinusoid(size_t n) {
    std::vector<double> v(n);
    constexpr double TWO_PI = 6.283185307179586;
    for (size_t i = 0; i < n; ++i) v[i] = 100.0 * std::sin(TWO_PI * i / 1000.0);
    return v;
}

static std::vector<double> gen_exp_decay(size_t n) {
    std::vector<double> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = 1e6 * std::exp(-static_cast<double>(i) / n);
    return v;
}

// Edge case: alternating sign flips (pathological for LNS sign bit encoding)
static std::vector<double> gen_sign_flip(size_t n, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> mag(0.5, 2.0);
    std::vector<double> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = (i % 2 == 0 ? 1.0 : -1.0) * mag(rng);
    return v;
}

// Edge case: near-zero values
static std::vector<double> gen_near_zero(size_t n, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> d(1e-15, 1e-10);
    std::vector<double> v(n);
    for (auto& x : v) x = d(rng);
    return v;
}

int main(int argc, char** argv) {
    fs::path out_dir = (argc > 1) ? argv[1] : "data/synthetic";
    constexpr size_t N = 1'000'000;

    // Multiplicative
    for (double sigma : {0.001, 0.01, 0.1}) {
        char name[64];
        std::snprintf(name, sizeof(name), "mult_sigma%.3f", sigma);
        write_bin(out_dir / (std::string(name) + ".bin"),
                  gen_multiplicative(N, sigma, 42));
    }

    // Additive
    for (double sigma : {0.1, 1.0, 10.0}) {
        char name[64];
        std::snprintf(name, sizeof(name), "add_sigma%.1f", sigma);
        write_bin(out_dir / (std::string(name) + ".bin"),
                  gen_additive(N, sigma, 43));
    }

    // Mixed
    for (double sigma : {0.01, 0.1}) {
        char name[64];
        std::snprintf(name, sizeof(name), "mixed_sigma%.2f", sigma);
        write_bin(out_dir / (std::string(name) + ".bin"),
                  gen_mixed(N, sigma, 44));
    }

    // Edge cases (smaller: 100k each)
    constexpr size_t NE = 100'000;
    write_bin(out_dir / "edge_constant.bin",  gen_constant(NE, 42.0));
    write_bin(out_dir / "edge_ramp.bin",      gen_ramp(NE));
    write_bin(out_dir / "edge_sinusoid.bin",  gen_sinusoid(NE));
    write_bin(out_dir / "edge_exp_decay.bin", gen_exp_decay(NE));
    write_bin(out_dir / "edge_sign_flip.bin", gen_sign_flip(NE, 45));
    write_bin(out_dir / "edge_near_zero.bin", gen_near_zero(NE, 46));

    std::printf("Done. All files in %s\n", out_dir.string().c_str());
    return 0;
}
