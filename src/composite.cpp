// Copyright 2026 Anuj Attri
// Licensed under the Apache License, Version 2.0 (the "License");
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
// SPDX-License-Identifier: Apache-2.0
// composite.cpp — explicit instantiations for composite LNS+Gorilla codecs.
#include "lns/composite.hpp"

namespace composite {
// Force instantiation of each Q-format variant.
template class LnsGorilla<8,  24>;
template class LnsGorilla<10, 22>;
template class LnsGorilla<12, 20>;
template class LnsGorilla<12, 16>;
} // namespace composite
