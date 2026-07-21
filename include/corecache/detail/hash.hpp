#pragma once

#include <cstddef>
#include <cstdint>

namespace corecache::detail {

// splitmix64-style finalizer. Re-mixes a hash value so that shard selection
// stays well-distributed even for std::hash specializations that are
// near-identity (e.g. std::hash<int>, std::hash<uint64_t> on many standard
// library implementations just return the value unchanged), which would
// otherwise cluster badly under a raw `% shard_count`.
constexpr std::size_t mix64(std::size_t h) noexcept {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

// Computes which shard a key belongs to. shard_count need not be a power of
// two -- modulo indexing is used throughout this library.
template <typename Key, typename Hash>
std::size_t shard_for(const Key& key, std::size_t shard_count, const Hash& hash) {
    return mix64(static_cast<std::size_t>(hash(key))) % shard_count;
}

}  // namespace corecache::detail
