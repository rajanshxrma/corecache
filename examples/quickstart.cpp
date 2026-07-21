// Minimal end-to-end usage of corecache: an LRU cache and an ARC cache,
// each populated, read back, and reported on.
#include "corecache/cache.hpp"

#include <cstdio>
#include <string>

int main() {
    // Default policy is LruPolicy; 4 shards here for a deterministic demo
    // (production code would typically just take the hardware_concurrency
    // default).
    corecache::Cache<std::string, int> lru_cache(/*capacity=*/100, /*shard_count=*/4);

    lru_cache.put("answer", 42);
    lru_cache.put("year", 2026);

    if (auto value = lru_cache.get("answer")) {
        std::printf("lru_cache[\"answer\"] = %d\n", *value);
    }

    // ARC is selected as a template parameter -- compile-time dispatch, no
    // runtime policy tag.
    corecache::Cache<std::string, int, corecache::ArcPolicy<std::string>> arc_cache(
        /*capacity=*/100, /*shard_count=*/4);

    arc_cache.put("answer", 42);
    if (auto value = arc_cache.get("answer")) {
        std::printf("arc_cache[\"answer\"] = %d\n", *value);
    }

    const auto& stats = lru_cache.stats();
    std::printf("lru_cache stats: hits=%llu misses=%llu evictions=%llu\n",
                static_cast<unsigned long long>(stats.hits.load()),
                static_cast<unsigned long long>(stats.misses.load()),
                static_cast<unsigned long long>(stats.evictions.load()));

    return 0;
}
