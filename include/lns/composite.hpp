#pragma once
// LNS → Gorilla composite codec.
//
// Encode pipeline:
//   double[] → LnsValue<I,F>[] → reinterpret raw int64_t as uint64_t → Gorilla XOR encode
//
// Decode pipeline:
//   Gorilla XOR decode → uint64_t[] → reinterpret as int64_t → LnsValue<I,F>[] → double[]
//
// The LNS layer transforms multiplicative data into small additive increments in
// log-space. The Gorilla layer then exploits XOR locality (many leading/trailing zeros).
// Round-trip is lossy at the LNS quantisation bound; see lns_codec.hpp for error budgets.

#include "lns_codec.hpp"
#include "gorilla_codec.hpp"
#include <cstring>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace composite {

template <int I, int F>
class LnsGorilla {
public:
    static const char* name() {
        static char buf[32];
        if (buf[0] == '\0')
            std::snprintf(buf, sizeof(buf), "LNS_Q%d_%d_Gorilla", I, F);
        return buf;
    }

    // Returns encoded bytes. Format:
    //   [4 bytes: n_values uint32_t LE]
    //   [1 byte: has_specials (0 = all normal, no flags array; 1 = flags present)]
    //   [gorilla_len 4 bytes LE]
    //   [gorilla_bytes: Gorilla-encoded stream of uint64_t LNS raws]
    //   [if has_specials: flags array, 1 byte per value]
    // Flags are stored separately so Gorilla XORs only the int64_t raws.
    // Skipping the flags array (the common case for normal data) cuts ~n bytes of overhead.
    static std::vector<uint8_t> encode(const double* data, size_t n) {
        if (n == 0) {
            std::vector<uint8_t> out(5, 0); // count(4) + has_specials(1)
            return out;
        }

        // Step 1: LNS encode each double
        std::vector<uint64_t> raws(n);
        std::vector<uint8_t>  flags(n);
        bool has_specials = false;
        for (size_t i = 0; i < n; ++i) {
            auto v = lns::encode_lns<I,F>(data[i]);
            uint64_t u;
            std::memcpy(&u, &v.raw, 8);
            raws[i]  = u;
            flags[i] = v.flags;
            if (v.flags) has_specials = true;
        }

        // Step 2: Gorilla-compress the raw uint64_t stream
        auto gorilla_bytes = gorilla::encode(raws.data(), n);

        // Step 3: assemble output
        std::vector<uint8_t> out;
        out.reserve(9 + gorilla_bytes.size() + (has_specials ? n : 0));

        uint32_t cnt = static_cast<uint32_t>(n);
        out.push_back(cnt & 0xFF);
        out.push_back((cnt >> 8) & 0xFF);
        out.push_back((cnt >> 16) & 0xFF);
        out.push_back((cnt >> 24) & 0xFF);

        out.push_back(has_specials ? 1 : 0);

        uint32_t glen = static_cast<uint32_t>(gorilla_bytes.size());
        out.push_back(glen & 0xFF);
        out.push_back((glen >> 8) & 0xFF);
        out.push_back((glen >> 16) & 0xFF);
        out.push_back((glen >> 24) & 0xFF);

        out.insert(out.end(), gorilla_bytes.begin(), gorilla_bytes.end());
        if (has_specials)
            out.insert(out.end(), flags.begin(), flags.end());
        return out;
    }

    static std::vector<double> decode(const uint8_t* buf, size_t byte_len) {
        if (byte_len < 9) return {};

        uint32_t n = static_cast<uint32_t>(buf[0])
                   | (static_cast<uint32_t>(buf[1]) << 8)
                   | (static_cast<uint32_t>(buf[2]) << 16)
                   | (static_cast<uint32_t>(buf[3]) << 24);
        bool has_specials = (buf[4] != 0);
        uint32_t glen = static_cast<uint32_t>(buf[5])
                      | (static_cast<uint32_t>(buf[6]) << 8)
                      | (static_cast<uint32_t>(buf[7]) << 16)
                      | (static_cast<uint32_t>(buf[8]) << 24);

        size_t min_len = 9 + glen + (has_specials ? n : 0);
        if (byte_len < min_len) return {};

        const uint8_t* gorilla_buf = buf + 9;
        const uint8_t* flags_buf   = has_specials ? buf + 9 + glen : nullptr;

        // Step 1: Gorilla decode
        auto raws = gorilla::decode(gorilla_buf, glen);
        if (raws.size() != n) return {};

        // Step 2: LNS decode
        std::vector<double> out(n);
        for (size_t i = 0; i < n; ++i) {
            int64_t r;
            std::memcpy(&r, &raws[i], 8);
            uint8_t fl = (flags_buf && has_specials) ? flags_buf[i] : 0;
            lns::LnsValue<I,F> v{r, fl};
            out[i] = lns::decode_lns<I,F>(v);
        }
        return out;
    }
};

// Pre-instantiated aliases
using LnsGorilla_Q8_24  = LnsGorilla<8,  24>;
using LnsGorilla_Q10_22 = LnsGorilla<10, 22>;
using LnsGorilla_Q12_20 = LnsGorilla<12, 20>;
using LnsGorilla_Q12_16 = LnsGorilla<12, 16>;

} // namespace composite
