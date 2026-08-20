// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Combiner<Out, Ins...>: N upstream Observables through a pure transform into
// one derived Observable<Out>. See architecture/README.md.

#pragma once

#include "architecture/Observable.h"

#include <functional>
#include <memory>
#include <tuple>
#include <utility>

namespace dish::arch {

template <class Out, class... Ins> class Combiner {
  public:
    Combiner(const Observable<Ins>&... upstreams, std::function<Out(const Ins&...)> transform)
        : output_(transform(upstreams.value()...)), transform_(std::move(transform)),
          upstreams_(std::addressof(upstreams)...),
          subscriptions_(upstreams.subscribe([this](const Ins&) { recompute(); },
                                             /*emitCurrent=*/false)...) {}

    Combiner(const Combiner&) = delete;
    Combiner& operator=(const Combiner&) = delete;
    Combiner(Combiner&&) = delete;
    Combiner& operator=(Combiner&&) = delete;
    ~Combiner() = default;

    const Observable<Out>& state() const { return output_; }

  private:
    void recompute() {
        output_.set(std::apply(
            [this](const Observable<Ins>*... ups) { return transform_(ups->value()...); },
            upstreams_));
    }

    Observable<Out> output_;
    std::function<Out(const Ins&...)> transform_;
    std::tuple<const Observable<Ins>*...> upstreams_;
    std::tuple<typename Observable<Ins>::Subscription...> subscriptions_;
};

} // namespace dish::arch
