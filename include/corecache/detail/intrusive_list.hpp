#pragma once

#include <cstddef>

namespace corecache::detail {

// Minimal, header-only intrusive doubly linked list.
//
// NodeT must expose `NodeT* prev` and `NodeT* next` members. This class owns
// no node memory -- callers own node lifetime entirely; IntrusiveList only
// manages linkage between nodes it is given. By convention the head is the
// most-recently-used end and the tail is the least-recently-used end, which
// is what every policy in this library (LRU's single list, ARC's T1/T2/B1/B2)
// relies on.
//
// Not thread-safe on its own -- callers (Shard) provide the exclusive lock
// under which all mutating list operations happen.
template <typename NodeT>
class IntrusiveList {
  public:
    IntrusiveList() = default;
    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;
    IntrusiveList(IntrusiveList&&) = delete;
    IntrusiveList& operator=(IntrusiveList&&) = delete;

    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] NodeT* front() const noexcept { return head_; }
    [[nodiscard]] NodeT* back() const noexcept { return tail_; }

    // Links `node` at the head (MRU position). `node` must not already be
    // linked into this or any other list.
    void push_front(NodeT* node) noexcept {
        node->prev = nullptr;
        node->next = head_;
        if (head_ != nullptr) {
            head_->prev = node;
        }
        head_ = node;
        if (tail_ == nullptr) {
            tail_ = node;
        }
        ++size_;
    }

    // Unlinks `node` from wherever it currently sits. `node` must currently
    // be linked into this list.
    void unlink(NodeT* node) noexcept {
        if (node->prev != nullptr) {
            node->prev->next = node->next;
        } else {
            head_ = node->next;
        }
        if (node->next != nullptr) {
            node->next->prev = node->prev;
        } else {
            tail_ = node->prev;
        }
        node->prev = nullptr;
        node->next = nullptr;
        --size_;
    }

    // Unlinks and returns the LRU/tail node, or nullptr if the list is empty.
    NodeT* pop_back() noexcept {
        NodeT* victim = tail_;
        if (victim != nullptr) {
            unlink(victim);
        }
        return victim;
    }

    // Moves an already-linked node to the head. No-op if already at the head.
    void move_to_front(NodeT* node) noexcept {
        if (head_ == node) {
            return;
        }
        unlink(node);
        push_front(node);
    }

  private:
    NodeT* head_ = nullptr;
    NodeT* tail_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace corecache::detail
