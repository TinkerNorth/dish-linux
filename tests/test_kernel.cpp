// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "architecture/Combiner.h"
#include "architecture/Composer.h"
#include "architecture/Controller.h"
#include "architecture/Observable.h"
#include "architecture/Repository.h"
#include "architecture/StateSource.h"

#include "ComposerProbe.h"
#include "ControllerProbe.h"
#include "RepositoryContract.h"
#include "StateSourceProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace dish::arch;
using dish::test::ComposerProbe;
using dish::test::ControllerProbe;
using dish::test::StateSourceProbe;

TEST_CASE("Observable replays the latest and emits only on change", "[kernel][observable]") {
    Observable<int> o{1};
    std::vector<int> seen;
    auto sub = o.subscribe([&](const int& v) { seen.push_back(v); }, true);
    REQUIRE(seen == std::vector<int>{1}); // the trailing true is emitCurrent

    o.set(1); // distinct-until-changed: no emit
    REQUIRE(seen.size() == 1);

    o.set(2);
    o.update([](const int& v) { return v + 10; });
    REQUIRE(seen == std::vector<int>{1, 2, 12});
    REQUIRE(o.value() == 12);

    sub.reset(); // unsubscribe
    o.set(99);
    REQUIRE(seen.size() == 3);
}

TEST_CASE("Observable subscribe(emitCurrent=false) skips the initial value",
          "[kernel][observable]") {
    Observable<int> o{5};
    std::vector<int> seen;
    auto sub = o.subscribe([&](const int& v) { seen.push_back(v); }, false);
    REQUIRE(seen.empty());
    o.set(6);
    REQUIRE(seen == std::vector<int>{6});
}

TEST_CASE("Composer derives eagerly and recomputes on any upstream change", "[kernel][composer]") {
    Observable<int> a{1};
    Observable<int> b{2};
    Composer<int, int, int> sum(a, b, [](const int& x, const int& y) { return x + y; });
    ComposerProbe<int> probe(sum.state());

    REQUIRE(sum.state().value() == 3); // computed once, eagerly
    REQUIRE(probe.states() == std::vector<int>{3});

    a.set(10);
    REQUIRE(sum.state().value() == 12);
    a.set(10); // upstream unchanged -> no recompute emit
    REQUIRE(probe.states() == std::vector<int>{3, 12});
}

namespace {
class CounterSource : public StateSource<int> {
  public:
    CounterSource() : StateSource<int>(0) {}
    void bump() {
        setState([](const int& v) { return v + 1; });
    }
    void setTo(int v) { setState(v); }
};
} // namespace

TEST_CASE("StateSource emits its mutation sequence", "[kernel][statesource]") {
    CounterSource src;
    StateSourceProbe<int> probe(src.state());
    src.bump();
    src.setTo(5);
    src.setTo(5); // distinct: no emit
    REQUIRE(probe.states() == std::vector<int>{0, 1, 5});
    REQUIRE(probe.latest() == 5);
    REQUIRE(probe.count() == 3);
}

namespace {
class RecordingController : public Controller<int> {
  public:
    explicit RecordingController(const Observable<int>& up) : Controller<int>(up) {}
    std::vector<int> applied;

  protected:
    void apply(const int& value) override { applied.push_back(value); }
};
} // namespace

TEST_CASE("Controller applies current on start, is idempotent, and stops cleanly",
          "[kernel][controller]") {
    Observable<int> up{7};
    RecordingController ctrl(up);
    ControllerProbe<RecordingController> probe(ctrl);

    probe.start();
    REQUIRE(ctrl.applied == std::vector<int>{7});

    probe.start();
    up.set(8);
    REQUIRE(ctrl.applied == std::vector<int>{7, 8});

    probe.stop();
    up.set(9); // no longer subscribed
    REQUIRE(ctrl.applied == std::vector<int>{7, 8});
}

namespace {
template <class K, class V> class InMemoryRepository : public KeyedRepository<K, V> {
  public:
    std::optional<V> get(const K& key) const override {
        auto it = data_.find(key);
        return it == data_.end() ? std::nullopt : std::optional<V>(it->second);
    }
    std::vector<V> all() const override {
        std::vector<V> out;
        out.reserve(data_.size());
        for (const auto& entry : data_) { out.push_back(entry.second); }
        return out;
    }
    void put(const K& key, const V& value) override { data_[key] = value; }
    void remove(const K& key) override { data_.erase(key); }
    void clear() override { data_.clear(); }
    K keyOf(const V&) const override { return K{}; } // unused by the base contract

  private:
    std::map<K, V> data_;
};
} // namespace

TEST_CASE("A concrete Repository satisfies the standard contract", "[kernel][repository]") {
    dish::test::runRepositoryContract<std::string, std::string>(
        [] { return std::make_unique<InMemoryRepository<std::string, std::string>>(); },
        [](int i) { return "key" + std::to_string(i); },
        [](const std::string& k) { return "val:" + k; });
}
