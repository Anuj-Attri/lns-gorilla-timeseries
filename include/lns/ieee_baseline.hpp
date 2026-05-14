#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace baseline {

// Raw IEEE 754 passthrough — 8 bytes per double, no transformation.
// Compression ratio = 1.0 by construction.
// Used as the reference denominator for all ratio calculations.

inline std::vector<uint8_t> encode(const double* data, size_t n) {
    std::vector<uint8_t> buf(n * 8);
    std::memcpy(buf.data(), data, n * 8);
    return buf;
}

inline std::vector<double> decode(const uint8_t* buf, size_t byte_len) {
    size_t n = byte_len / 8;
    std::vector<double> out(n);
    std::memcpy(out.data(), buf, n * 8);
    return out;
}

} // namespace baseline
