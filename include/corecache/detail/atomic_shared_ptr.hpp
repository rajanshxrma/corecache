#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace corecache::detail {

// Presents the same API surface as C++20's std::atomic<std::shared_ptr<T>>
// (load / store / is_lock_free), implemented via the pre-C++20
// std::atomic_load_explicit / std::atomic_store_explicit / atomic_is_lock_free
// free functions for shared_ptr.
//
// Why not std::atomic<std::shared_ptr<T>> directly: on this toolchain's
// libc++ (Apple clang 21 / _LIBCPP_VERSION 220106), instantiating
// std::atomic<std::shared_ptr<T>> fails to compile --
// "std::atomic<T> requires that T be a trivially copyable type" -- because
// libc++ has not implemented the C++20-mandated atomic<T> partial
// specialization for shared_ptr (verified directly against this compiler,
// not assumed from documentation). This is a real platform gap, not a
// design choice.
//
// The pre-C++20 free functions -- deprecated by the standard in favor of
// atomic<T>, but still implemented by this libc++ and confirmed to compile
// warning-free even under -Wall -Wextra -Wpedantic -Wdeprecated-declarations
// -Werror -- provide the same atomicity guarantee this design relies on:
// every load/store hands back a fully-formed shared_ptr, so there are no
// torn reads, regardless of whether the implementation is internally
// lock-free. On this platform it is not: is_lock_free() correctly reports
// false via this path, which corecache surfaces at startup rather than
// assuming either way.
template <typename T>
class AtomicSharedPtr {
  public:
    AtomicSharedPtr() noexcept = default;
    explicit AtomicSharedPtr(std::shared_ptr<T> initial) noexcept : value_(std::move(initial)) {}

    AtomicSharedPtr(const AtomicSharedPtr&) = delete;
    AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

    std::shared_ptr<T> load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
        return std::atomic_load_explicit(&value_, order);
    }

    void store(std::shared_ptr<T> desired,
               std::memory_order order = std::memory_order_seq_cst) noexcept {
        std::atomic_store_explicit(&value_, std::move(desired), order);
    }

    [[nodiscard]] bool is_lock_free() const noexcept { return std::atomic_is_lock_free(&value_); }

  private:
    mutable std::shared_ptr<T> value_;
};

}  // namespace corecache::detail
