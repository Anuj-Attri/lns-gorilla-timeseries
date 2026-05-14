// Copyright 2026 Anuj Attri
// Licensed under the Apache License, Version 2.0 (the "License");
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
// SPDX-License-Identifier: Apache-2.0
// csv_to_bin.cpp — convert a single-column CSV of doubles to raw f64 binary.
//
// Usage: ./csv_to_bin input.csv output.bin [column_index]
//   column_index: 0-based index of the column to extract (default: 0)
//   Skips header line if first token cannot be parsed as a double.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s input.csv output.bin [column_index]\n", argv[0]);
        return 1;
    }
    const char* in_path  = argv[1];
    const char* out_path = argv[2];
    int col = (argc >= 4) ? std::atoi(argv[3]) : 0;

    std::ifstream f(in_path);
    if (!f) { std::fprintf(stderr, "Cannot open %s\n", in_path); return 1; }

    std::vector<double> vals;
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string token;
        int cur = 0;
        bool found = false;
        while (std::getline(ss, token, ',')) {
            if (cur++ == col) {
                char* end;
                double v = std::strtod(token.c_str(), &end);
                if (end != token.c_str() && *end == '\0') {
                    vals.push_back(v);
                    found = true;
                } else if (first) {
                    // Looks like a header — skip silently
                }
                break;
            }
        }
        first = false;
        (void)found;
    }

    fs::create_directories(fs::path(out_path).parent_path());
    std::ofstream out(out_path, std::ios::binary);
    if (!out) { std::fprintf(stderr, "Cannot write %s\n", out_path); return 1; }
    out.write(reinterpret_cast<const char*>(vals.data()),
              static_cast<std::streamsize>(vals.size() * 8));

    std::printf("Wrote %zu values from column %d of %s → %s\n",
                vals.size(), col, in_path, out_path);
    return 0;
}
