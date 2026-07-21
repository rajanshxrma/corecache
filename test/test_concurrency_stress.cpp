#include <gtest/gtest.h>

#include <atomic>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#include "corecache/cache.hpp"
#include "corecache/detail/atomic_shared_ptr.hpp"

#ifndef CORECACHE_STRESS_OPS
#define CORECACHE_STRESS_OPS 200000
#endif

namespace {

// (c) Report whether the atomic value-read path is actually lock-free on
// this platform, rather than assuming either way.
//
// The spec's canonical probe is std::atomic<std::shared_ptr<int>>{}.is_lock_free().
// On this toolchain's libc++ (Apple clang 21, _LIBCPP_VERSION 220106) that
// expression does not compile at all: std::atomic<std::shared_ptr<T>>'s
// C++20 partial specialization isn't implemented here (verified directly --
// see include/corecache/detail/atomic_shared_ptr.hpp for the full
// explanation and the pre-C++20-free-function fallback this library
// actually uses). So this reports the observed value for corecache's own
// AtomicSharedPtr<T> wrapper, which is what every Entry in this library
// really uses for its value slot.
TEST(ConcurrencyStress, ReportsAtomicSharedPtrLockFreeStatus) {
    corecache::detail::AtomicSharedPtr<int> probe;
    const bool lock_free = probe.is_lock_free();
    std::cerr << "[corecache] AtomicSharedPtr<int>::is_lock_free() = " << std::boolalpha
              << lock_free << " (std::atomic<std::shared_ptr<T>> does not compile on this libc++;"
              << " see detail/atomic_shared_ptr.hpp)" << std::endl;
    SUCCEED();
}

// (a) Mixed randomized get/put/erase from many threads over a shared,
// bounded keyspace. The only thing asserted is the post-join invariant:
// size() must never exceed capacity(), regardless of policy. This is
// meant to run under both plain builds and ThreadSanitizer -- TSan is what
// actually proves the absence of data races; this assertion just proves
// the eviction bookkeeping stays coherent under contention.
template <typename Policy>
void RunMixedStress(const char* label) {
    constexpr int kKeyspace = 500;
    constexpr int kThreads = 16;
    constexpr std::size_t kCapacity = 200;
    constexpr int kOpsPerThread = CORECACHE_STRESS_OPS;

    corecache::Cache<int, int, Policy> cache(kCapacity, /*shard_count=*/8);

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&cache, t]() {
            std::mt19937 rng(static_cast<unsigned>(t) * 7919u + 12345u);
            std::uniform_int_distribution<int> key_dist(0, kKeyspace - 1);
            std::uniform_int_distribution<int> op_dist(0, 99);
            for (int i = 0; i < kOpsPerThread; ++i) {
                const int key = key_dist(rng);
                const int op = op_dist(rng);
                if (op < 60) {
                    cache.get(key);
                } else if (op < 95) {
                    cache.put(key, key * 31 + i);
                } else {
                    cache.erase(key);
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_LE(cache.size(), cache.capacity())
        << label << ": size() exceeded capacity() after concurrent stress";
}

TEST(ConcurrencyStress, MixedRandomOpsLruInvariantHolds) {
    RunMixedStress<corecache::LruPolicy<int>>("LRU");
}

TEST(ConcurrencyStress, MixedRandomOpsArcInvariantHolds) {
    RunMixedStress<corecache::ArcPolicy<int>>("ARC");
}

// (b) Disjoint keyspace: each thread owns an exclusive key range, writes
// its own keys, then reads them all back and checks every value against
// what it wrote. Capacity is sized generously (4x total keys) so eviction
// cannot confound this signal -- this test is specifically about data
// correctness under concurrent access to *different* shards/keys, not
// about eviction behavior (which is covered elsewhere). Any mismatch here
// would mean a real bug: broken shard hashing causing two different keys
// to collide on one Entry, a race in Entry lifetime management, or a torn
// atomic read.
template <typename Policy>
void RunDisjointKeyspaceStress(const char* label) {
    constexpr int kThreads = 16;
    constexpr int kKeysPerThread = 200;
    constexpr int kTotalKeys = kThreads * kKeysPerThread;

    corecache::Cache<int, int, Policy> cache(static_cast<std::size_t>(kTotalKeys) * 4,
                                             /*shard_count=*/8);

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    std::atomic<int> corruption_count{0};

    auto expected_value = [](int key) { return key * 1000003 + 7; };

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&cache, &corruption_count, &expected_value, t]() {
            const int base = t * kKeysPerThread;
            for (int i = 0; i < kKeysPerThread; ++i) {
                const int key = base + i;
                cache.put(key, expected_value(key));
            }
            for (int i = 0; i < kKeysPerThread; ++i) {
                const int key = base + i;
                auto value = cache.get(key);
                if (!value.has_value() || *value != expected_value(key)) {
                    corruption_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(corruption_count.load(), 0)
        << label << ": cross-thread data corruption detected on " << kTotalKeys << " disjoint keys";
}

TEST(ConcurrencyStress, DisjointKeyspaceNoCorruptionLru) {
    RunDisjointKeyspaceStress<corecache::LruPolicy<int>>("LRU");
}

TEST(ConcurrencyStress, DisjointKeyspaceNoCorruptionArc) {
    RunDisjointKeyspaceStress<corecache::ArcPolicy<int>>("ARC");
}

}  // namespace
