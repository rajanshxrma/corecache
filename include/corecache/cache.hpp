#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "corecache/detail/hash.hpp"
#include "corecache/policy.hpp"
#include "corecache/shard.hpp"

namespace corecache {

// corecache::Cache<Key, Value, Policy, Hash>
//
// A sharded, concurrent, capacity-bounded cache. Key space is partitioned
// across `shard_count` independent Shard instances (default:
// std::thread::hardware_concurrency()), each with its own shared_mutex,
// map, and Policy instance -- see shard.hpp for the concurrency model and
// policy.hpp for LruPolicy / ArcPolicy.
//
// Key must be hashable (Hash), equality-comparable, and copy-constructible
// -- copy-constructible because both the map and (for ArcPolicy) the ghost
// lists need independent copies of a key that outlive a given Entry. Value
// has no such requirement: get_shared() is the zero-copy path and works
// for move-only Value types; get() additionally requires
// std::copy_constructible<Value> since it hands back a Value by value.
//
// Policy selection is a compile-time template parameter -- no vtables, no
// runtime dispatch on a policy tag -- which is both more idiomatic C++ and
// lets the compiler fully inline policy operations into Shard's hot paths.
template <typename Key, typename Value, typename Policy = LruPolicy<Key>,
          typename Hash = std::hash<Key>>
    requires EvictionPolicy<Policy, Key>
class Cache {
public:
    using ShardT = Shard<Key, Value, Policy, Hash>;

    // Aliased (not redefined) so Shard -- which increments these counters
    // directly -- and Cache share the exact same type.
    using Stats = CacheStats;

    explicit Cache(std::size_t capacity, std::size_t shard_count = default_shard_count())
        : capacity_(capacity) {
        shard_count = std::max<std::size_t>(1, shard_count);
        shards_.reserve(shard_count);

        // Split capacity across shards as evenly as possible: base
        // capacity/shard_count each, remainder distributed one-per-shard to
        // the first (capacity % shard_count) shards. Sums to exactly
        // `capacity`, never inflates it.
        const std::size_t base = capacity / shard_count;
        const std::size_t remainder = capacity % shard_count;
        for (std::size_t i = 0; i < shard_count; ++i) {
            const std::size_t shard_capacity = base + (i < remainder ? 1 : 0);
            shards_.push_back(std::make_unique<ShardT>(shard_capacity, stats_));
        }
    }

    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
    Cache(Cache&&) noexcept = default;
    Cache& operator=(Cache&&) noexcept = default;

    // Copy-out read. Requires Value to be copy-constructible; for
    // move-only Value types, use get_shared() instead.
    std::optional<Value> get(const Key& key)
        requires std::copy_constructible<Value>
    {
        std::shared_ptr<Value> value = get_shared(key);
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::optional<Value>(*value);
    }

    // Zero-copy read: returns the entry's shared_ptr<Value> directly, or
    // nullptr on a miss. The only get() usable with a move-only Value.
    std::shared_ptr<Value> get_shared(const Key& key) {
        return shard_for(key).get_shared(key);
    }

    // Insert or overwrite. Takes Value by value and moves it into the
    // entry -- real move semantics, no unnecessary copies.
    void put(Key key, Value value) {
        ShardT& shard = shard_for(key);
        shard.put(std::move(key), std::move(value));
    }

    bool erase(const Key& key) { return shard_for(key).erase(key); }

    [[nodiscard]] std::size_t size() const {
        std::size_t total = 0;
        for (const auto& shard : shards_) {
            total += shard->size();
        }
        return total;
    }

    // Introspection: per-shard resident counts, in shard-index order.
    // Useful for observing distribution quality and for tests/metrics --
    // not part of the core get/put/erase contract.
    [[nodiscard]] std::vector<std::size_t> shard_sizes() const {
        std::vector<std::size_t> sizes;
        sizes.reserve(shards_.size());
        for (const auto& shard : shards_) {
            sizes.push_back(shard->size());
        }
        return sizes;
    }

    [[nodiscard]] std::size_t shard_count() const noexcept { return shards_.size(); }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    void clear() {
        for (auto& shard : shards_) {
            shard->clear();
        }
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    static std::size_t default_shard_count() {
        return std::max<std::size_t>(1, std::thread::hardware_concurrency());
    }

    [[nodiscard]] ShardT& shard_for(const Key& key) const {
        const std::size_t index = detail::shard_for(key, shards_.size(), hash_);
        return *shards_[index];
    }

    std::size_t capacity_;
    Hash hash_{};
    std::vector<std::unique_ptr<ShardT>> shards_;
    Stats stats_;
};

}  // namespace corecache
