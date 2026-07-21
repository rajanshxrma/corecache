#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

#include "corecache/detail/atomic_shared_ptr.hpp"

namespace corecache {

// Aggregate hit/miss/eviction counters, shared by every shard of a Cache.
struct CacheStats {
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};
    std::atomic<uint64_t> evictions{0};
};

// A cache entry: the policy's intrusive list node (recency/frequency
// linkage), plus the actual value storage and an approximate recency tick.
//
// `value` is loaded/stored via AtomicSharedPtr (see detail/atomic_shared_ptr.hpp
// for why this isn't literally std::atomic<std::shared_ptr<Value>> on this
// toolchain) so that a cache hit can read the value out without taking any
// lock at all: a single acquire-load handing back a fully-formed
// shared_ptr, with no torn reads regardless of internal lock-free-ness.
//
// `tick` is a relaxed, approximate recency counter, bumped on every read.
// It exists as a fallback recency signal for the (rare, documented) window
// where the opportunistic list-reorder in Shard::get_shared can't acquire
// the exclusive lock -- see the comment there.
template <typename Key, typename Value, typename Policy>
struct Entry : Policy::Node {
    detail::AtomicSharedPtr<Value> value;
    std::atomic<uint64_t> tick{0};

    Entry(Key key_in, std::shared_ptr<Value> value_in)
        : Policy::Node{std::move(key_in)}, value(std::move(value_in)) {}
};

// One shard of a sharded Cache: an independent key space, independent
// capacity slice, independent Policy instance, independent shared_mutex.
//
// Concurrency model (see README for the full writeup):
//   - map_ structure, the intrusive list/ghost-list pointers, and Policy's
//     internal state are ALL guarded by mutex_. Reads take a shared lock
//     (concurrent readers allowed); every mutation takes a unique lock.
//   - The one genuinely lock-free path is reading the Value out of an
//     Entry already found under the shared lock: a single
//     AtomicSharedPtr::load(acquire) plus a relaxed tick bump. That is the
//     literal, narrowly-scoped lock-free claim this library makes -- map
//     lookup itself is still a lock (shared, but a lock).
template <typename Key, typename Value, typename Policy, typename Hash = std::hash<Key>>
class Shard {
public:
    explicit Shard(std::size_t capacity) : policy_(capacity) {}

    Shard(const Shard&) = delete;
    Shard& operator=(const Shard&) = delete;

    // The zero-copy read path -- the only one usable when Value is
    // move-only. Returns nullptr on a miss.
    std::shared_ptr<Value> get_shared(const Key& key) {
        using EntryT = Entry<Key, Value, Policy>;
        EntryT* entry = nullptr;
        std::shared_ptr<Value> value;
        {
            std::shared_lock read_lock(mutex_);
            auto it = map_.find(key);
            if (it == map_.end()) {
                stats_.misses.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            entry = it->second.get();
            stats_.hits.fetch_add(1, std::memory_order_relaxed);

            // The value read itself: a single atomic acquire-load handing
            // back a fully-formed shared_ptr, no torn reads regardless of
            // internal lock-free-ness -- that's the literal, narrowly-
            // scoped lock-free claim this library makes. It still happens
            // while the shared lock is held, though: the lock isn't
            // protecting the load's atomicity (the load is already
            // atomic), it's what keeps `entry` itself alive. Without it, a
            // concurrent put() could evict and destroy this exact Entry
            // between releasing the lock and dereferencing `entry` here --
            // a real use-after-free, not a benign race.
            value = entry->value.load(std::memory_order_acquire);
            entry->tick.fetch_add(1, std::memory_order_relaxed);
        }

        // Opportunistic promotion: std::shared_mutex has no atomic
        // shared->exclusive upgrade, so we try to acquire the exclusive
        // lock without blocking. Between releasing the shared lock above
        // and this try_lock succeeding, another thread could run put(),
        // evict `entry`, and free it -- `entry` is a dangling identity
        // token at that point, not something we dereference again except
        // for the address comparison below. If we do get the exclusive
        // lock, we MUST re-validate the entry is still present before
        // touching its prev/next pointers. If the try_lock fails, or
        // re-validation fails, we skip the reorder as a safe no-op:
        // recency then falls back to the tick counter above.
        std::unique_lock write_lock(mutex_, std::try_to_lock);
        if (write_lock.owns_lock()) {
            auto it = map_.find(key);
            if (it != map_.end() && it->second.get() == entry) {
                policy_.touch(entry);
            }
        }
        return value;
    }

    // Insert or update. Existing keys are updated in place and treated as
    // a fresh access (promoted exactly like a hit). New keys go through the
    // eviction policy's admit() and may evict an existing entry.
    void put(Key key, Value value) {
        using EntryT = Entry<Key, Value, Policy>;
        std::unique_lock lock(mutex_);

        if (auto it = map_.find(key); it != map_.end()) {
            EntryT* entry = it->second.get();
            entry->value.store(std::make_shared<Value>(std::move(value)),
                                std::memory_order_release);
            policy_.touch(entry);
            return;
        }

        auto classification = policy_.classify(key);
        auto new_entry =
            std::make_unique<EntryT>(key, std::make_shared<Value>(std::move(value)));
        EntryT* raw = new_entry.get();

        auto* victim = policy_.admit(raw, classification);
        if (victim == raw) {
            // Zero-capacity shard: admit() refused the insert outright.
            return;
        }
        if (victim != nullptr) {
            map_.erase(victim->key);
            stats_.evictions.fetch_add(1, std::memory_order_relaxed);
        }
        map_.emplace(std::move(key), std::move(new_entry));
    }

    bool erase(const Key& key) {
        std::unique_lock lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            return false;
        }
        policy_.remove(it->second.get());
        map_.erase(it);
        return true;
    }

    [[nodiscard]] std::size_t size() const {
        std::shared_lock lock(mutex_);
        return map_.size();
    }

    void clear() {
        std::unique_lock lock(mutex_);
        // Reuses only concept-required operations (remove()) so this works
        // for any conforming Policy, not just the two shipped here.
        for (auto& [key, entry] : map_) {
            policy_.remove(entry.get());
        }
        map_.clear();
    }

    // Per-shard hit/miss/eviction counters. Cache::stats() sums these across
    // all shards on demand.
    //
    // Deliberately NOT a reference to one CacheStats instance shared by
    // every shard: an earlier version did exactly that, and every shard's
    // get()/put() -- regardless of which shard, i.e. regardless of key --
    // ended up doing atomic fetch_add on the same handful of cache lines.
    // That's an accidental point of cross-shard contention that has nothing
    // to do with mutexes but defeats sharding just the same (cache-line
    // ping-pong across cores), and it measurably tanked multi-threaded
    // throughput -- see the README's benchmarking section for the before/
    // after numbers. Keeping counters per-shard means only threads
    // contending on the SAME shard ever touch the same cache line.
    [[nodiscard]] const CacheStats& local_stats() const noexcept { return stats_; }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<Key, std::unique_ptr<Entry<Key, Value, Policy>>, Hash> map_;
    Policy policy_;
    CacheStats stats_;
};

}  // namespace corecache
