// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Composer<Out, Ins...>: a Combiner under the architectural name, for named
// derivations. See architecture/README.md.

#pragma once

#include "architecture/Combiner.h"

namespace dish::arch {

template <class Out, class... Ins> class Composer : public Combiner<Out, Ins...> {
  public:
    using Combiner<Out, Ins...>::Combiner;
};

} // namespace dish::arch
