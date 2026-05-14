// composite.cpp — explicit instantiations for composite LNS+Gorilla codecs.
#include "lns/composite.hpp"

namespace composite {
// Force instantiation of each Q-format variant.
template class LnsGorilla<8,  24>;
template class LnsGorilla<10, 22>;
template class LnsGorilla<12, 20>;
template class LnsGorilla<12, 16>;
} // namespace composite
