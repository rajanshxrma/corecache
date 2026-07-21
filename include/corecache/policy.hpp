#pragma once

#include <concepts>
#include <cstddef>

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

}  // namespace corecache
