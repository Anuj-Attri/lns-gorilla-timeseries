// lns_codec.cpp — explicit instantiations so the linker can find them in tests/bench.
// The actual logic is header-only; this file just forces template instantiation.
#include "lns/lns_codec.hpp"

namespace lns {
template LnsValue<8,  24> encode_lns<8,  24>(double);
template LnsValue<10, 22> encode_lns<10, 22>(double);
template LnsValue<12, 20> encode_lns<12, 20>(double);
template LnsValue<12, 16> encode_lns<12, 16>(double);
template LnsValue<15, 48> encode_lns<15, 48>(double);

template double decode_lns<8,  24>(LnsValue<8,  24>);
template double decode_lns<10, 22>(LnsValue<10, 22>);
template double decode_lns<12, 20>(LnsValue<12, 20>);
template double decode_lns<12, 16>(LnsValue<12, 16>);
template double decode_lns<15, 48>(LnsValue<15, 48>);
} // namespace lns
