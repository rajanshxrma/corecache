#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>

#include "corecache/cache.hpp"

namespace {

// Tracks exactly how many times its destructor runs, globally, across all
// instances -- used to prove eviction/erase/destroy each destroy the Value
// exactly once (no leak, no double-free).
struct DtorCounter {
    static std::atomic<int> live_count;
    static std::atomic<int> destroy_count;

    int id;

    explicit DtorCounter(int id_in = 0) : id(id_in) { live_count.fetch_add(1); }
    DtorCounter(const DtorCounter& other) : id(other.id) { live_count.fetch_add(1); }
    DtorCounter(DtorCounter&& other) noexcept : id(other.id) { live_count.fetch_add(1); }
    DtorCounter& operator=(const DtorCounter&) = default;
    DtorCounter& operator=(DtorCounter&&) noexcept = default;
    ~DtorCounter() {
        live_count.fetch_sub(1);
        destroy_count.fetch_add(1);
    }
};
std::atomic<int> DtorCounter::live_count{0};
std::atomic<int> DtorCounter::destroy_count{0};

class CacheApiTest : public ::testing::Test {
  protected:
    void SetUp() override {
        DtorCounter::live_count = 0;
        DtorCounter::destroy_count = 0;
    }
};

TEST_F(CacheApiTest, EvictionDestroysValueExactlyOnce) {
    {
        corecache::Cache<int, DtorCounter> cache(2, /*shard_count=*/1);
        cache.put(1, DtorCounter(1));
        cache.put(2, DtorCounter(2));
        EXPECT_EQ(DtorCounter::live_count.load(), 2);

        // Third distinct key on a capacity-2, single-shard LRU cache evicts
        // key 1's value.
        cache.put(3, DtorCounter(3));
        EXPECT_EQ(DtorCounter::live_count.load(), 2);
        EXPECT_GE(DtorCounter::destroy_count.load(), 1);
        EXPECT_EQ(cache.stats().evictions.load(), 1u);
    }
    // Cache destructor must release everything still resident, with no
    // double-destroy of anything already evicted.
    EXPECT_EQ(DtorCounter::live_count.load(), 0);
}

TEST_F(CacheApiTest, EraseDestroysValueExactlyOnce) {
    corecache::Cache<int, DtorCounter> cache(4, /*shard_count=*/1);
    cache.put(1, DtorCounter(1));
    EXPECT_EQ(DtorCounter::live_count.load(), 1);

    EXPECT_TRUE(cache.erase(1));
    EXPECT_EQ(DtorCounter::live_count.load(), 0);
    EXPECT_FALSE(cache.erase(1));  // second erase is a no-op, not a double-free
}

TEST_F(CacheApiTest, ClearDestroysEveryValueExactlyOnce) {
    corecache::Cache<int, DtorCounter> cache(4, /*shard_count=*/2);
    cache.put(1, DtorCounter(1));
    cache.put(2, DtorCounter(2));
    cache.put(3, DtorCounter(3));
    EXPECT_EQ(DtorCounter::live_count.load(), 3);

    cache.clear();
    EXPECT_EQ(DtorCounter::live_count.load(), 0);
    EXPECT_EQ(cache.size(), 0u);
}

TEST_F(CacheApiTest, OverwriteExistingKeyDestroysOldValueOnce) {
    corecache::Cache<int, DtorCounter> cache(4, /*shard_count=*/1);
    cache.put(1, DtorCounter(1));
    EXPECT_EQ(DtorCounter::live_count.load(), 1);

    cache.put(1, DtorCounter(100));  // overwrite: old value destroyed exactly once
    EXPECT_EQ(DtorCounter::live_count.load(), 1);

    auto value = cache.get(1);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->id, 100);
}

TEST(CacheApiMoveOnly, MoveOnlyValueCompilesAndWorksViaGetShared) {
    corecache::Cache<std::string, std::unique_ptr<int>> cache(10);

    cache.put("key", std::make_unique<int>(7));
    std::shared_ptr<std::unique_ptr<int>> value = cache.get_shared("key");
    ASSERT_NE(value, nullptr);
    ASSERT_NE(*value, nullptr);
    EXPECT_EQ(**value, 7);

    EXPECT_EQ(cache.get_shared("missing"), nullptr);
}

TEST(CacheApiSizeCapacityClear, ReportCorrectly) {
    corecache::Cache<int, int> cache(50, /*shard_count=*/4);
    EXPECT_EQ(cache.capacity(), 50u);
    EXPECT_EQ(cache.size(), 0u);

    for (int i = 0; i < 10; ++i) {
        cache.put(i, i * i);
    }
    EXPECT_EQ(cache.size(), 10u);

    cache.clear();
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.capacity(), 50u);  // capacity is fixed, clear() doesn't change it
}

TEST(CacheApiSizeCapacityClear, CapacitySplitsExactlyAcrossShards) {
    // 10 capacity over 4 shards: 3,3,2,2 -- sums to exactly 10, no inflation.
    corecache::Cache<int, int> cache(10, /*shard_count=*/4);
    EXPECT_EQ(cache.capacity(), 10u);

    for (int i = 0; i < 1000; ++i) {
        cache.put(i, i);
    }
    // Total resident entries can never exceed the total declared capacity.
    EXPECT_LE(cache.size(), 10u);
}

TEST(CacheApiGet, MissReturnsNullopt) {
    corecache::Cache<int, int> cache(10);
    EXPECT_FALSE(cache.get(42).has_value());
    EXPECT_EQ(cache.stats().misses.load(), 1u);
}

TEST(CacheApiGet, ArcPolicyCompilesAndBasicallyWorks) {
    corecache::Cache<int, int, corecache::ArcPolicy<int>> cache(10, /*shard_count=*/2);
    cache.put(1, 100);
    auto value = cache.get(1);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 100);
}

}  // namespace
