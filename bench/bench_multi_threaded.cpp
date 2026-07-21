// Multi-threaded benchmarks: corecache vs. baseline_lru_cache (the honest
// single-global-mutex implementation), across thread counts 1-16, under
// two workload mixes (90% get / 10% put, and 50/50).
//
// All threads share the SAME key range with a small hot-key subset (10% of
// ops land on 10 fixed keys) -- disjoint per-thread keyspaces would let
// even the naive single-mutex baseline scale trivially (no real
// contention) and misrepresent the result. This is the shared-contention
// workload where sharding is actually supposed to pay off.
//
// Setup/teardown synchronization: exactly one thread (thread_index() == 0)
// constructs and prefills the shared cache; the rest must not touch it
// until that's done, and none of them may still be inside the workload
// loop when it gets deleted. Google Benchmark's own iteration-count
// coordination does not guarantee either of those on its own -- an earlier
// version of this file used the bare "if (thread_index() == 0) { new...
// }; ...; if (thread_index() == 0) { delete... }" idiom without any
// synchronization and it crashed under real contention (a thread could
// still be mid-get()/put() when thread 0 deleted the cache out from under
// it -- a real use-after-free in the benchmark harness, not in corecache
// itself). RunSharedBenchmark below fixes that with an explicit
// acquire/release handoff for setup and a completion countdown for
// teardown, so the last thread out is always the one that deletes.
#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <random>
#include <thread>

#include "baseline_lru_cache.hpp"
#include "corecache/cache.hpp"

namespace {

constexpr int kKeyRange = 100000;
constexpr int kHotKeyCount = 10;
constexpr std::size_t kCapacity = 50000;

// corecache's shard count here is deliberately tuned well above
// std::thread::hardware_concurrency() (8 on the machine these numbers were
// measured on), not left at the default. At 16 contending threads sharing
// one hot-key subset, 8 shards means ~2 threads per shard on average --
// not enough contention reduction to overcome corecache's higher per-op
// cost (opportunistic promotion's second lock+lookup, atomic shared_ptr
// bookkeeping). Measured directly: with shard_count left at the default
// (8), corecache was SLOWER than baseline_lru_cache at every thread count
// in this file's workload -- raising it to 64 flipped that to a real win
// at 4+ threads. This mirrors standard practice in real sharded caches
// (e.g. Guava/Caffeine-style stripe counts are tuned independent of core
// count, not derived from it) -- shard count is a capacity-planning knob,
// and hardware_concurrency() is only a reasonable default, not a promise
// that it's the right number for every workload.
constexpr std::size_t kCorecacheShardCount = 64;

template <typename CacheT>
void PrefillShared(CacheT& cache) {
    for (int i = 0; i < kKeyRange; ++i) {
        cache.put(i, i);
    }
}

template <typename CacheT>
void RunMixedWorkload(benchmark::State& state, CacheT& cache, int put_percent) {
    std::mt19937 rng(static_cast<unsigned>(state.thread_index()) * 2654435761u + 1u);
    std::uniform_int_distribution<int> hot_gate(0, 99);
    std::uniform_int_distribution<int> hot_key_dist(0, kHotKeyCount - 1);
    std::uniform_int_distribution<int> full_key_dist(0, kKeyRange - 1);
    std::uniform_int_distribution<int> op_gate(0, 99);

    for (auto _ : state) {
        const int key = (hot_gate(rng) < 10) ? hot_key_dist(rng) : full_key_dist(rng);
        if (op_gate(rng) < put_percent) {
            cache.put(key, key);
        } else {
            benchmark::DoNotOptimize(cache.get(key));
        }
    }
}

// One instantiation of these statics per CacheT -- safe because each is a
// template function-local static, initialized independently per type.
// g_cache: the acquire/release handoff for setup (nullptr until thread 0
//   has fully constructed and prefilled the cache).
// g_finished: teardown countdown -- the thread that observes the count
//   reach state.threads() is guaranteed every other thread has already
//   returned from RunMixedWorkload, so it alone deletes.
template <typename CacheT, typename FactoryFn>
void RunSharedBenchmark(benchmark::State& state, FactoryFn make_cache, int put_percent) {
    static std::atomic<CacheT*> g_cache{nullptr};
    static std::atomic<int> g_finished{0};

    CacheT* cache;
    if (state.thread_index() == 0) {
        g_finished.store(0, std::memory_order_relaxed);
        cache = make_cache();
        PrefillShared(*cache);
        g_cache.store(cache, std::memory_order_release);
    } else {
        while ((cache = g_cache.load(std::memory_order_acquire)) == nullptr) {
            std::this_thread::yield();
        }
    }

    RunMixedWorkload(state, *cache, put_percent);

    if (g_finished.fetch_add(1, std::memory_order_acq_rel) + 1 == state.threads()) {
        g_cache.store(nullptr, std::memory_order_release);
        delete cache;
    }
}

// -----------------------------------------------------------------------
// baseline_lru_cache (single global std::mutex)
// -----------------------------------------------------------------------

using Baseline = corecache::bench::BaselineLruCache<int, int>;

void BM_MT_Baseline_90Get10Put(benchmark::State& state) {
    RunSharedBenchmark<Baseline>(
        state, [] { return new Baseline(kCapacity); }, /*put_percent=*/10);
}
BENCHMARK(BM_MT_Baseline_90Get10Put)->ThreadRange(1, 16)->UseRealTime();

void BM_MT_Baseline_50Get50Put(benchmark::State& state) {
    RunSharedBenchmark<Baseline>(
        state, [] { return new Baseline(kCapacity); }, /*put_percent=*/50);
}
BENCHMARK(BM_MT_Baseline_50Get50Put)->ThreadRange(1, 16)->UseRealTime();

// -----------------------------------------------------------------------
// corecache, LruPolicy
// -----------------------------------------------------------------------

using CorecacheLru = corecache::Cache<int, int>;

void BM_MT_CorecacheLru_90Get10Put(benchmark::State& state) {
    RunSharedBenchmark<CorecacheLru>(
        state, [] { return new CorecacheLru(kCapacity, kCorecacheShardCount); }, /*put_percent=*/10);
}
BENCHMARK(BM_MT_CorecacheLru_90Get10Put)->ThreadRange(1, 16)->UseRealTime();

void BM_MT_CorecacheLru_50Get50Put(benchmark::State& state) {
    RunSharedBenchmark<CorecacheLru>(
        state, [] { return new CorecacheLru(kCapacity, kCorecacheShardCount); }, /*put_percent=*/50);
}
BENCHMARK(BM_MT_CorecacheLru_50Get50Put)->ThreadRange(1, 16)->UseRealTime();

// -----------------------------------------------------------------------
// corecache, ArcPolicy
// -----------------------------------------------------------------------

using CorecacheArc = corecache::Cache<int, int, corecache::ArcPolicy<int>>;

void BM_MT_CorecacheArc_90Get10Put(benchmark::State& state) {
    RunSharedBenchmark<CorecacheArc>(
        state, [] { return new CorecacheArc(kCapacity, kCorecacheShardCount); }, /*put_percent=*/10);
}
BENCHMARK(BM_MT_CorecacheArc_90Get10Put)->ThreadRange(1, 16)->UseRealTime();

void BM_MT_CorecacheArc_50Get50Put(benchmark::State& state) {
    RunSharedBenchmark<CorecacheArc>(
        state, [] { return new CorecacheArc(kCapacity, kCorecacheShardCount); }, /*put_percent=*/50);
}
BENCHMARK(BM_MT_CorecacheArc_50Get50Put)->ThreadRange(1, 16)->UseRealTime();

}  // namespace

BENCHMARK_MAIN();
