#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "corecache/cache.hpp"

namespace {

// Capacity is split evenly across shards (kKeyCount / kShardCount each),
// but real key-to-shard hashing is only approximately even, not exact --
// so a plain capacity == key count would let one over-assigned shard hit
// its own local cap and evict, even though the cache as a whole has spare
// room. Sizing capacity generously (3x key count) means every shard's
// local capacity comfortably exceeds its actual assigned key count, so no
// eviction happens and shard_sizes() reflects pure hash-distribution
// quality rather than eviction noise.
TEST(ShardDistribution, IntKeysSpreadAcrossAllShardsWithinBalanceBound) {
    constexpr std::size_t kKeyCount = 20000;
    constexpr std::size_t kShardCount = 8;

    corecache::Cache<int, int> cache(kKeyCount * 3, kShardCount);
    for (int i = 0; i < static_cast<int>(kKeyCount); ++i) {
        cache.put(i, i);
    }

    ASSERT_EQ(cache.size(), kKeyCount);

    auto sizes = cache.shard_sizes();
    ASSERT_EQ(sizes.size(), kShardCount);

    const double mean = static_cast<double>(kKeyCount) / static_cast<double>(kShardCount);
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        EXPECT_GT(sizes[i], 0u) << "shard " << i << " got no keys at all";
        EXPECT_LE(static_cast<double>(sizes[i]), 2.0 * mean)
            << "shard " << i << " has " << sizes[i] << " keys, more than 2x the mean (" << mean
            << ")";
    }
}

// Sequential/near-identity-hashing keys (std::hash<int> is identity on
// libc++/libstdc++) are exactly the pathological case mix64() exists to
// fix -- a raw `% shard_count` on consecutive integers with a
// power-of-two-ish shard count can land entirely in one or two shards.
TEST(ShardDistribution, SequentialIntKeysDoNotClusterInOneShard) {
    constexpr std::size_t kKeyCount = 16384;
    constexpr std::size_t kShardCount = 16;

    corecache::Cache<int, int> cache(kKeyCount * 3, kShardCount);
    for (int i = 0; i < static_cast<int>(kKeyCount); ++i) {
        cache.put(i, 0);
    }

    auto sizes = cache.shard_sizes();
    const double mean = static_cast<double>(kKeyCount) / static_cast<double>(kShardCount);
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        EXPECT_LE(static_cast<double>(sizes[i]), 2.0 * mean)
            << "shard " << i << " clustered: " << sizes[i] << " keys (mean " << mean << ")";
    }
}

TEST(ShardDistribution, StringKeysSpreadAcrossAllShards) {
    constexpr std::size_t kKeyCount = 10000;
    constexpr std::size_t kShardCount = 8;

    corecache::Cache<std::string, int> cache(kKeyCount * 3, kShardCount);
    for (std::size_t i = 0; i < kKeyCount; ++i) {
        cache.put("key-" + std::to_string(i), static_cast<int>(i));
    }

    auto sizes = cache.shard_sizes();
    const double mean = static_cast<double>(kKeyCount) / static_cast<double>(kShardCount);
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        EXPECT_LE(static_cast<double>(sizes[i]), 2.0 * mean)
            << "shard " << i << " has " << sizes[i] << " keys, more than 2x the mean (" << mean
            << ")";
    }
}

}  // namespace
