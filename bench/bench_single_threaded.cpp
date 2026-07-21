// Single-threaded Get/Put benchmarks across four contenders:
//   - corecache with LruPolicy
//   - corecache with ArcPolicy
//   - baseline_lru_cache (one std::mutex around every op, "the honest
//     naive-but-safe implementation")
//   - a raw std::unordered_map -- a theoretical ceiling only. It has no
//     capacity bound, no eviction, and no locking at all, so it is not a
//     real alternative; it exists purely to show how much headroom is
//     between "a hash map with no cache semantics" and corecache.
//
// Parameterized by cache size (10k / 100k entries).
#include <benchmark/benchmark.h>

#include <random>
#include <unordered_map>

#include "baseline_lru_cache.hpp"
#include "corecache/cache.hpp"

namespace {

template <typename CacheT>
void FillSequential(CacheT& cache, int n) {
    for (int i = 0; i < n; ++i) {
        cache.put(i, i);
    }
}

// ---------------------------------------------------------------------
// Get
// ---------------------------------------------------------------------

void BM_Get_CorecacheLru(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    corecache::Cache<int, int> cache(static_cast<std::size_t>(n));
    FillSequential(cache, n);
    std::mt19937 rng(1);
    std::uniform_int_distribution<int> dist(0, n - 1);
    for (auto _ : state) {
        benchmark::DoNotOptimize(cache.get(dist(rng)));
    }
}
BENCHMARK(BM_Get_CorecacheLru)->Arg(10000)->Arg(100000);

void BM_Get_CorecacheArc(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    corecache::Cache<int, int, corecache::ArcPolicy<int>> cache(static_cast<std::size_t>(n));
    FillSequential(cache, n);
    std::mt19937 rng(1);
    std::uniform_int_distribution<int> dist(0, n - 1);
    for (auto _ : state) {
        benchmark::DoNotOptimize(cache.get(dist(rng)));
    }
}
BENCHMARK(BM_Get_CorecacheArc)->Arg(10000)->Arg(100000);

void BM_Get_BaselineMutexMap(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    corecache::bench::BaselineLruCache<int, int> cache(static_cast<std::size_t>(n));
    FillSequential(cache, n);
    std::mt19937 rng(1);
    std::uniform_int_distribution<int> dist(0, n - 1);
    for (auto _ : state) {
        benchmark::DoNotOptimize(cache.get(dist(rng)));
    }
}
BENCHMARK(BM_Get_BaselineMutexMap)->Arg(10000)->Arg(100000);

void BM_Get_RawUnorderedMap_TheoreticalCeiling(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    std::unordered_map<int, int> map;
    map.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        map[i] = i;
    }
    std::mt19937 rng(1);
    std::uniform_int_distribution<int> dist(0, n - 1);
    for (auto _ : state) {
        benchmark::DoNotOptimize(map.find(dist(rng)));
    }
}
BENCHMARK(BM_Get_RawUnorderedMap_TheoreticalCeiling)->Arg(10000)->Arg(100000);

// ---------------------------------------------------------------------
// Put
// ---------------------------------------------------------------------

void BM_Put_CorecacheLru(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    corecache::Cache<int, int> cache(static_cast<std::size_t>(n));
    FillSequential(cache, n);
    int i = 0;
    for (auto _ : state) {
        cache.put(i % n, i);
        ++i;
    }
}
BENCHMARK(BM_Put_CorecacheLru)->Arg(10000)->Arg(100000);

void BM_Put_CorecacheArc(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    corecache::Cache<int, int, corecache::ArcPolicy<int>> cache(static_cast<std::size_t>(n));
    FillSequential(cache, n);
    int i = 0;
    for (auto _ : state) {
        cache.put(i % n, i);
        ++i;
    }
}
BENCHMARK(BM_Put_CorecacheArc)->Arg(10000)->Arg(100000);

void BM_Put_BaselineMutexMap(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    corecache::bench::BaselineLruCache<int, int> cache(static_cast<std::size_t>(n));
    FillSequential(cache, n);
    int i = 0;
    for (auto _ : state) {
        cache.put(i % n, i);
        ++i;
    }
}
BENCHMARK(BM_Put_BaselineMutexMap)->Arg(10000)->Arg(100000);

void BM_Put_RawUnorderedMap_TheoreticalCeiling(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    std::unordered_map<int, int> map;
    map.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        map[i] = i;
    }
    int i = 0;
    for (auto _ : state) {
        map[i % n] = i;
        ++i;
    }
}
BENCHMARK(BM_Put_RawUnorderedMap_TheoreticalCeiling)->Arg(10000)->Arg(100000);

}  // namespace

BENCHMARK_MAIN();
