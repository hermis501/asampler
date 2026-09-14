#pragma once

#include <atomic>
#include <memory>
#include <utility>
#include <version>

namespace asampler
{
    // A shared_ptr that one thread can store() while another load()s a snapshot.
    //
    // C++20's std::atomic<std::shared_ptr<T>> is exactly this, and MSVC ships it.
    // Apple's libc++ does not, so the macOS build falls back to the older
    // std::atomic_load / std::atomic_store free functions. Both implementations
    // guard the pointer with a brief internal lock — the same real-time trade-off
    // CLAUDE.md describes — so behaviour is identical across platforms. Only
    // load() and store() are provided, because only those are used.
#if defined (__cpp_lib_atomic_shared_ptr)
    template <typename T>
    using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;
#else
    template <typename T>
    class AtomicSharedPtr
    {
    public:
        AtomicSharedPtr() noexcept = default;
        AtomicSharedPtr (const AtomicSharedPtr&) = delete;
        AtomicSharedPtr& operator= (const AtomicSharedPtr&) = delete;

       #if defined (__clang__)
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wdeprecated-declarations"
       #endif

        std::shared_ptr<T> load() const noexcept      { return std::atomic_load (&ptr); }
        void store (std::shared_ptr<T> desired) noexcept { std::atomic_store (&ptr, std::move (desired)); }

       #if defined (__clang__)
        #pragma clang diagnostic pop
       #endif

    private:
        std::shared_ptr<T> ptr;
    };
#endif
}
