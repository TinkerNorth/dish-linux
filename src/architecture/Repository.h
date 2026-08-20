// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Repository<K,V>: dumb, synchronous, thread-safe keyed storage.
// See architecture/README.md.

#pragma once

#include <optional>
#include <vector>

namespace dish::arch {

template <class K, class V> class Repository {
  public:
    virtual ~Repository() = default;
    virtual std::optional<V> get(const K& key) const = 0;
    virtual std::vector<V> all() const = 0;
    virtual void put(const K& key, const V& value) = 0;
    virtual void remove(const K& key) = 0;
    virtual void clear() = 0;
};

template <class K, class V> class KeyedRepository : public Repository<K, V> {
  public:
    using Repository<K, V>::put;
    using Repository<K, V>::remove;

    virtual K keyOf(const V& value) const = 0;

    void put(const V& value) { this->put(keyOf(value), value); }
    // Distinct name avoids an overload clash with remove(const K&) when K == V.
    void removeValue(const V& value) { this->remove(keyOf(value)); }
};

} // namespace dish::arch
