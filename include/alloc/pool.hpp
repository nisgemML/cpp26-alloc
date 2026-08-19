#pragma once
// alloc/pool.hpp — RAII pool with complete C++26 monadic interface.

#include "alloc/contracts.hpp"
#include "alloc/slab.hpp"
#include <expected>
#include <utility>
#include <print>

namespace alloc {

template <typename T, std::size_t SlotsPerSlab = 4096uz, std::size_t MaxSlabs = 16uz>
class Pool;

// ─────────────────────────────────────────────────────────────────────────────
// PoolPtr<T>: unique-ownership handle. Returns slot to pool on destruct.
// ─────────────────────────────────────────────────────────────────────────────
template <typename T, std::size_t SlotsPerSlab = 4096uz, std::size_t MaxSlabs = 16uz>
class PoolPtr {
public:
    using pool_type = Pool<T, SlotsPerSlab, MaxSlabs>;

    PoolPtr() noexcept = default;
    PoolPtr(T* p, pool_type* pool) noexcept : ptr_{p}, pool_{pool} {}
    ~PoolPtr() noexcept { reset(); }

    PoolPtr(PoolPtr&& o) noexcept : ptr_{o.ptr_}, pool_{o.pool_}
        { o.ptr_ = nullptr; o.pool_ = nullptr; }

    PoolPtr& operator=(PoolPtr&& o) noexcept {
        if (this != &o) {
            reset();
            ptr_ = o.ptr_; pool_ = o.pool_;
            o.ptr_ = nullptr; o.pool_ = nullptr;
        }
        return *this;
    }

    PoolPtr(PoolPtr const&)            = delete;
    PoolPtr& operator=(PoolPtr const&) = delete;

    [[nodiscard]] T*   get()       const noexcept { return ptr_;          }
    [[nodiscard]] T&   operator*() const noexcept { return *ptr_;         }
    [[nodiscard]] T*   operator->()const noexcept { return ptr_;          }
    [[nodiscard]] bool empty()     const noexcept { return ptr_ == nullptr;}
    explicit operator bool()       const noexcept { return ptr_ != nullptr;}

    void reset() noexcept {
        if (ptr_ && pool_) {
            ptr_->~T();
            (void)pool_->slab_.free(ptr_);
            ptr_ = nullptr; pool_ = nullptr;
        }
    }

private:
    T*          ptr_{nullptr};
    pool_type*  pool_{nullptr};
};

// ─────────────────────────────────────────────────────────────────────────────
// Pool<T>
// ─────────────────────────────────────────────────────────────────────────────
template <typename T, std::size_t SlotsPerSlab, std::size_t MaxSlabs>
class Pool {
public:
    friend class PoolPtr<T, SlotsPerSlab, MaxSlabs>;
    using ptr_type    = PoolPtr<T, SlotsPerSlab, MaxSlabs>;
    using result_type = std::expected<ptr_type, AllocError>;

    Pool() = default;

    // ── make() — C++26 monadic result ─────────────────────────────────────────
    template <typename... Args>
    [[nodiscard("Discarding make() drops the object immediately")]]
    result_type make(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        return slab_.alloc()
            .transform([&](T* raw) -> ptr_type {
                return ptr_type{::new (raw) T(std::forward<Args>(args)...), this};
            });
    }

    // ── Diagnostics ───────────────────────────────────────────────────────────
    [[nodiscard]] std::string    format_stats() const { return slab_.format_stats(); }
    [[nodiscard]] std::size_t    num_slabs()    const noexcept { return slab_.num_slabs(); }
    [[nodiscard]] std::generator<T const*> slots() const { return slab_.slots(); }
    [[nodiscard]] std::size_t    shrink()       noexcept { return slab_.shrink(); }

    void print_stats() const { std::print("{}\n", format_stats()); }

private:
    SlabAllocator<T, SlotsPerSlab, MaxSlabs> slab_;
};

}  // namespace alloc
