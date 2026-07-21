#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <memory>
#include <unordered_map>

#include "corecache/detail/intrusive_list.hpp"

namespace corecache {

// An EvictionPolicy owns the bookkeeping structures (lists, ghost lists,
// adaptive parameters) that decide what stays cached and what gets evicted.
// It never touches Value storage -- Shard owns the map from Key to Entry,
// and Policy only links/unlinks the intrusive Node embedded in each Entry.
//
//   touch(node)             -- record a hit on an already-resident node.
//   admit(node, classify(k)) -- record insertion of a new node, given the
//                                classification of its key; returns the
//                                node to physically evict from the cache's
//                                map (or nullptr if nothing was evicted).
//   remove(node)             -- explicit removal (erase()), no eviction
//                                bookkeeping (e.g. no ghost-listing).
template <typename P, typename K>
concept EvictionPolicy = requires(P p, const K& key, typename P::Node& node) {
    typename P::Node;
    { p.touch(&node) } -> std::same_as<void>;
    { p.admit(&node, p.classify(key)) } -> std::same_as<typename P::Node*>;
    { p.remove(&node) } -> std::same_as<void>;
};

// Classic O(1) LRU: a single intrusive doubly linked list, MRU at head.
//
// LRU has no notion of ghost entries or adaptive state, so `classify()`
// always reports the same trivial value -- it exists purely so LruPolicy's
// admit() call shape matches the EvictionPolicy concept, which is shared
// with ArcPolicy where classification is meaningful.
template <typename Key>
class LruPolicy {
  public:
    struct Node {
        Key key{};
        Node* prev = nullptr;
        Node* next = nullptr;
    };

    enum class Classification { kMiss };

    explicit LruPolicy(std::size_t capacity) noexcept : capacity_(capacity) {}

    Classification classify(const Key& /*key*/) const noexcept { return Classification::kMiss; }

    // Hit: move to MRU (head).
    void touch(Node* node) noexcept { list_.move_to_front(node); }

    // Insert a brand-new node at MRU. If that pushes the list over capacity,
    // unlink and return the LRU (tail) victim for the caller to erase.
    //
    // A zero-capacity policy (a shard that got none of the total capacity
    // when C < shard_count) refuses every insert: it returns `node` itself,
    // unlinked, signaling "do not store this."
    Node* admit(Node* node, Classification /*classification*/) noexcept {
        if (capacity_ == 0) {
            return node;
        }
        list_.push_front(node);
        if (list_.size() > capacity_) {
            return list_.pop_back();
        }
        return nullptr;
    }

    void remove(Node* node) noexcept { list_.unlink(node); }

    [[nodiscard]] std::size_t size() const noexcept { return list_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  private:
    detail::IntrusiveList<Node> list_;
    std::size_t capacity_;
};

// Adaptive Replacement Cache (Megiddo & Modha, "ARC: A Self-Tuning, Low
// Overhead Replacement Cache", FAST 2003).
//
// Four intrusive lists per policy instance:
//   T1 -- real, resident entries seen once recently ("recency").
//   T2 -- real, resident entries seen at least twice ("frequency").
//   B1 -- ghost list: keys recently evicted from T1 (no Value payload).
//   B2 -- ghost list: keys recently evicted from T2 (no Value payload).
// `p` is the adaptive target size for |T1| (0 <= p <= capacity). Invariant:
// |T1| + |B1| <= capacity, |T2| + |B2| <= capacity.
//
// Design decision: ARC state is per-shard, not one global instance shared
// across shards. A single global ARC would be textbook-accurate (it adapts
// on the full workload), but its T1/T2/B1/B2/p mutation is inherently
// sequential -- sharding the lock around one global ARC instance would
// serialize every get()/put(), hit or miss, defeating the entire point of
// sharding. Per-shard ARC trades some adaptation accuracy (each shard only
// sees the hashed slice of the workload routed to it) for genuine
// scalability under contention -- the same tradeoff real partitioned
// production caches make. That tradeoff is deliberate, not an oversight.
template <typename Key>
class ArcPolicy {
  public:
    struct Node {
        Key key{};
        Node* prev = nullptr;
        Node* next = nullptr;
    };

    enum class Classification { kT1Hit, kT2Hit, kB1Hit, kB2Hit, kMiss };

    explicit ArcPolicy(std::size_t capacity) noexcept : capacity_(capacity) {}

    // O(1) T1/T2/B1/B2 membership check via the internal location map.
    Classification classify(const Key& key) const noexcept {
        auto it = location_.find(key);
        if (it == location_.end()) {
            return Classification::kMiss;
        }
        switch (it->second.where) {
            case Where::kT1:
                return Classification::kT1Hit;
            case Where::kT2:
                return Classification::kT2Hit;
            case Where::kB1:
                return Classification::kB1Hit;
            case Where::kB2:
                return Classification::kB2Hit;
        }
        return Classification::kMiss;  // unreachable
    }

    // Case I: x is in T1 or T2 -- always promote to MRU of T2 on a hit,
    // regardless of which tier it came from.
    void touch(Node* node) noexcept {
        auto it = location_.find(node->key);
        const bool was_t1 = (it != location_.end()) && (it->second.where == Where::kT1);
        if (was_t1) {
            t1_.unlink(node);
        } else {
            t2_.unlink(node);
        }
        t2_.push_front(node);
        if (it != location_.end()) {
            it->second.where = Where::kT2;
            it->second.node = node;
        } else {
            location_.emplace(node->key, ListLocation{Where::kT2, node, nullptr});
        }
    }

    // `node` is a freshly constructed Node for a key that is not currently a
    // real (T1/T2) resident -- Cases II, III and IV. Returns the Node* to
    // physically remove from the cache's map, or nullptr if this call did
    // not evict a real entry.
    Node* admit(Node* node, Classification classification) noexcept {
        if (capacity_ == 0) {
            return node;  // zero-capacity shard: refuse every insert.
        }
        switch (classification) {
            case Classification::kB1Hit:
                return admit_b1_hit(node);
            case Classification::kB2Hit:
                return admit_b2_hit(node);
            case Classification::kT1Hit:
            case Classification::kT2Hit:
                // Contract: admit() is only invoked for keys classify() did
                // not report as a real hit. Fall back to miss-handling
                // defensively rather than corrupt state.
            case Classification::kMiss:
            default:
                return admit_miss(node);
        }
    }

    // Explicit removal (Cache::erase()): drop from whichever real list the
    // node is in. Deliberately does NOT ghost the key -- erase() is a
    // request to forget the key, not an eviction ARC should adapt around.
    void remove(Node* node) noexcept {
        auto it = location_.find(node->key);
        if (it == location_.end()) {
            return;
        }
        if (it->second.where == Where::kT1) {
            t1_.unlink(node);
        } else if (it->second.where == Where::kT2) {
            t2_.unlink(node);
        }
        location_.erase(it);
    }

    // Introspection, used by tests to assert on internal ARC state directly.
    [[nodiscard]] std::size_t t1_size() const noexcept { return t1_.size(); }
    [[nodiscard]] std::size_t t2_size() const noexcept { return t2_.size(); }
    [[nodiscard]] std::size_t b1_size() const noexcept { return b1_.size(); }
    [[nodiscard]] std::size_t b2_size() const noexcept { return b2_.size(); }
    [[nodiscard]] std::size_t p() const noexcept { return p_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  private:
    enum class Where { kT1, kT2, kB1, kB2 };

    struct GhostNode {
        Key key{};
        GhostNode* prev = nullptr;
        GhostNode* next = nullptr;
    };

    struct ListLocation {
        Where where;
        Node* node = nullptr;  // valid iff where is kT1 or kT2 (owned by caller's Entry)
        std::unique_ptr<GhostNode> ghost;  // valid iff where is kB1 or kB2 (owned here)
    };

    // REPLACE(x, p): evicts exactly one real page from T1 or T2 into its
    // matching ghost list, per the paper's tie-break rule. `x_in_b2`
    // indicates whether the key that triggered this REPLACE call is itself
    // a member of B2 (only true for Case III), which breaks the |T1| == p
    // tie in favor of evicting from T1.
    Node* replace(bool x_in_b2) noexcept {
        if (t1_.size() >= 1 && (t1_.size() > p_ || (x_in_b2 && t1_.size() == p_))) {
            Node* victim = t1_.pop_back();
            location_.erase(victim->key);
            ghost_insert(Where::kB1, victim->key);
            return victim;
        }
        if (t2_.size() >= 1) {
            Node* victim = t2_.pop_back();
            location_.erase(victim->key);
            ghost_insert(Where::kB2, victim->key);
            return victim;
        }
        return nullptr;  // degenerate: nothing resident to evict.
    }

    void ghost_insert(Where where, const Key& key) {
        auto ghost = std::make_unique<GhostNode>();
        ghost->key = key;
        GhostNode* raw = ghost.get();
        if (where == Where::kB1) {
            b1_.push_front(raw);
        } else {
            b2_.push_front(raw);
        }
        location_[key] = ListLocation{where, nullptr, std::move(ghost)};
    }

    void ghost_remove(Where where, const Key& key) {
        auto it = location_.find(key);
        if (it == location_.end() || it->second.ghost == nullptr) {
            return;
        }
        GhostNode* raw = it->second.ghost.get();
        if (where == Where::kB1) {
            b1_.unlink(raw);
        } else {
            b2_.unlink(raw);
        }
        location_.erase(it);
    }

    // Deletes the LRU entry of a ghost list outright -- bookkeeping only,
    // never a real cache eviction.
    void ghost_evict_lru(Where where) {
        detail::IntrusiveList<GhostNode>& list = (where == Where::kB1) ? b1_ : b2_;
        GhostNode* victim = list.back();
        if (victim == nullptr) {
            return;
        }
        list.unlink(victim);
        location_.erase(victim->key);
    }

    // Case II: ghost hit in B1 -- x was evicted from T1 too eagerly. Grow
    // p (favor recency more), then graduate x straight to MRU of T2.
    Node* admit_b1_hit(Node* node) noexcept {
        const std::size_t b1n = b1_.size();
        const std::size_t b2n = b2_.size();
        const std::size_t delta = std::max<std::size_t>(1, b1n == 0 ? 0 : b2n / b1n);
        p_ = std::min(capacity_, p_ + delta);

        Node* victim = replace(/*x_in_b2=*/false);
        ghost_remove(Where::kB1, node->key);
        t2_.push_front(node);
        location_[node->key] = ListLocation{Where::kT2, node, nullptr};
        return victim;
    }

    // Case III: ghost hit in B2 -- x was evicted from T2 too eagerly.
    // Shrink p (favor frequency more), then graduate x to MRU of T2.
    Node* admit_b2_hit(Node* node) noexcept {
        const std::size_t b1n = b1_.size();
        const std::size_t b2n = b2_.size();
        const std::size_t delta = std::max<std::size_t>(1, b2n == 0 ? 0 : b1n / b2n);
        p_ = (delta > p_) ? 0 : p_ - delta;

        Node* victim = replace(/*x_in_b2=*/true);
        ghost_remove(Where::kB2, node->key);
        t2_.push_front(node);
        location_[node->key] = ListLocation{Where::kT2, node, nullptr};
        return victim;
    }

    // Case IV: x is in none of the four lists -- a true miss. New keys
    // always start at MRU of T1.
    //
    // NOTE on a literal-reading-vs-paper discrepancy: the outer
    // `|T1| + |B1| == c` branch has two sub-cases (|T1| < c and |T1| == c).
    // The published Megiddo-Modha algorithm calls REPLACE(x, p) ONLY in the
    // |T1| < c sub-case (paired with deleting B1's LRU ghost); the |T1| == c
    // sub-case deletes T1's LRU directly and that deletion alone is the
    // eviction for that branch -- REPLACE is not called again on top of it.
    // Calling REPLACE unconditionally after both sub-cases would double-
    // evict on every miss that lands in the |T1| == c sub-case, needlessly
    // shrinking real cache occupancy. This implementation follows the
    // published algorithm (single eviction per miss).
    Node* admit_miss(Node* node) noexcept {
        Node* victim = nullptr;
        const std::size_t t1n = t1_.size();
        const std::size_t b1n = b1_.size();
        const std::size_t t2n = t2_.size();
        const std::size_t b2n = b2_.size();

        if (t1n + b1n == capacity_) {
            if (t1n < capacity_) {
                ghost_evict_lru(Where::kB1);
                victim = replace(/*x_in_b2=*/false);
            } else {
                // B1 empty, T1 == capacity: direct T1 deletion is the
                // eviction for this branch.
                Node* v = t1_.pop_back();
                location_.erase(v->key);
                victim = v;
            }
        } else if (t1n + t2n + b1n + b2n >= capacity_) {
            if (t1n + t2n + b1n + b2n == 2 * capacity_) {
                ghost_evict_lru(Where::kB2);
            }
            victim = replace(/*x_in_b2=*/false);
        }

        t1_.push_front(node);
        location_[node->key] = ListLocation{Where::kT1, node, nullptr};
        return victim;
    }

    detail::IntrusiveList<Node> t1_;
    detail::IntrusiveList<Node> t2_;
    detail::IntrusiveList<GhostNode> b1_;
    detail::IntrusiveList<GhostNode> b2_;
    std::unordered_map<Key, ListLocation> location_;
    std::size_t capacity_;
    std::size_t p_ = 0;
};

}  // namespace corecache
