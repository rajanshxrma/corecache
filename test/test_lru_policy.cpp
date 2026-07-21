#include "corecache/policy.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

using Policy = corecache::LruPolicy<int>;

TEST(LruPolicy, EvictsInFillOrderAtCapacity) {
    Policy policy(3);
    Policy::Node a{1}, b{2}, c{3}, d{4};

    EXPECT_EQ(policy.admit(&a, policy.classify(1)), nullptr);
    EXPECT_EQ(policy.admit(&b, policy.classify(2)), nullptr);
    EXPECT_EQ(policy.admit(&c, policy.classify(3)), nullptr);
    EXPECT_EQ(policy.size(), 3u);

    // Cache is full; inserting a 4th key evicts the least-recently-used (a).
    Policy::Node* victim = policy.admit(&d, policy.classify(4));
    ASSERT_NE(victim, nullptr);
    EXPECT_EQ(victim->key, 1);
    EXPECT_EQ(policy.size(), 3u);
}

TEST(LruPolicy, TouchRefreshesRecencyAndChangesEvictionVictim) {
    Policy policy(3);
    Policy::Node a{1}, b{2}, c{3}, d{4};

    policy.admit(&a, policy.classify(1));
    policy.admit(&b, policy.classify(2));
    policy.admit(&c, policy.classify(3));

    // Without a touch, 'a' (oldest) would be evicted next. Touching 'a'
    // makes it MRU, so 'b' becomes the next victim instead.
    policy.touch(&a);
    Policy::Node* victim = policy.admit(&d, policy.classify(4));
    ASSERT_NE(victim, nullptr);
    EXPECT_EQ(victim->key, 2);
}

TEST(LruPolicy, TouchOnExistingKeyNeverEvicts) {
    Policy policy(2);
    Policy::Node a{1}, b{2};
    policy.admit(&a, policy.classify(1));
    policy.admit(&b, policy.classify(2));
    ASSERT_EQ(policy.size(), 2u);

    // Simulates Cache::put() on a key that already exists: the caller must
    // call touch(), not admit(), for an existing node. That path can never
    // report an eviction victim because it never inserts a new node.
    policy.touch(&a);
    policy.touch(&b);
    EXPECT_EQ(policy.size(), 2u);
}

TEST(LruPolicy, RemoveUnlinksExplicitly) {
    Policy policy(3);
    Policy::Node a{1}, b{2}, c{3};
    policy.admit(&a, policy.classify(1));
    policy.admit(&b, policy.classify(2));
    policy.admit(&c, policy.classify(3));

    policy.remove(&b);
    EXPECT_EQ(policy.size(), 2u);

    // Space freed by remove() means the next admit should not evict.
    Policy::Node d{4};
    Policy::Node* victim = policy.admit(&d, policy.classify(4));
    EXPECT_EQ(victim, nullptr);
    EXPECT_EQ(policy.size(), 3u);
}

TEST(LruPolicy, CapacityOneEdgeCase) {
    Policy policy(1);
    Policy::Node a{1}, b{2}, c{3};

    EXPECT_EQ(policy.admit(&a, policy.classify(1)), nullptr);
    EXPECT_EQ(policy.size(), 1u);

    Policy::Node* victim1 = policy.admit(&b, policy.classify(2));
    ASSERT_NE(victim1, nullptr);
    EXPECT_EQ(victim1->key, 1);
    EXPECT_EQ(policy.size(), 1u);

    Policy::Node* victim2 = policy.admit(&c, policy.classify(3));
    ASSERT_NE(victim2, nullptr);
    EXPECT_EQ(victim2->key, 2);
    EXPECT_EQ(policy.size(), 1u);
}

TEST(LruPolicy, ZeroCapacityRefusesEveryInsert) {
    Policy policy(0);
    Policy::Node a{1};
    Policy::Node* victim = policy.admit(&a, policy.classify(1));
    // Zero-capacity policies signal "refused" by returning the node itself.
    EXPECT_EQ(victim, &a);
    EXPECT_EQ(policy.size(), 0u);
}

}  // namespace
