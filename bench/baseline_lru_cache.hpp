#pragma once

#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace corecache::bench {

// The fair baseline: a capacity-bounded LRU with the same feature set as
// corecache, but with ONE std::mutex around every operation, including
// reads -- the honest "what you'd write without thinking about
// concurrency." This, not a raw unordered_map, is corecache's primary
// comparison point; a raw map has no capacity bound or eviction at all and
// is included in the single-threaded benchmarks only as a theoretical
// ceiling, never as a real alternative.
template <typename Key, typename Value>
class BaselineLruCache {
public:
    explicit BaselineLruCache(std::size_t capacity) : capacity_(capacity) {}

    std::optional<Value> get(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            return std::nullopt;
        }
        list_.splice(list_.begin(), list_, it->second);
        return it->second->second;
    }

    void put(Key key, Value value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->second = std::move(value);
            list_.splice(list_.begin(), list_, it->second);
            return;
        }
        list_.emplace_front(std::move(key), std::move(value));
        map_[list_.front().first] = list_.begin();
        if (capacity_ == 0) {
            map_.erase(list_.front().first);
            list_.pop_front();
            return;
        }
        if (map_.size() > capacity_) {
            auto last = std::prev(list_.end());
            map_.erase(last->first);
            list_.pop_back();
        }
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_.size();
    }

private:
    using ListT = std::list<std::pair<Key, Value>>;

    std::size_t capacity_;
    mutable std::mutex mutex_;
    ListT list_;
    std::unordered_map<Key, typename ListT::iterator> map_;
};

}  // namespace corecache::bench
