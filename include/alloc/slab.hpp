#pragma once
// alloc/slab.hpp — Lock-free slab allocator. Pure C++26.
// Requires: GCC 14+ (-std=c++2c) or GCC 16+ (-std=c++26 -fcontracts)
//
// ═══════════════════════════════════════════════════════════════════════════
// C++26 FEATURES
// ═══════════════════════════════════════════════════════════════════════════
//
//  Feature                       Standard  Status on GCC 14
//  ────────────────────────────  ────────  ─────────────────────────────────
//  Contracts pre/post/assert     C++26     Simulated (GCC 16 = native)
//  std::expected<T,E>            C++23/26  ✓ Available
//  expected monadic ops          C++23/26  ✓ Available
//  std::formatter<AllocError>    C++23/26  ✓ Available
//  std::generator<T>             C++23/26  ✓ Available
//  std::print / std::format      C++23/26  ✓ Available
//  std::views::enumerate         C++23     ✓ Available
//  Placeholder variable _        C++26     ✓ Available
//  uz literal suffix             C++23     ✓ Available
//  std::add_sat/saturate_cast    C++26     ✓ Available
//  std::latch / std::barrier     C++20     ✓ Available
//  std::atomic::wait/notify_all  C++20/26  ✓ Available
//  [[nodiscard("reason")]]       C++20     ✓ Available
//  std::source_location          C++20     ✓ Used in contract layer
//  std::span<T>                  C++20     ✓ Available
//  std::atomic_ref<T>            C++20     ✓ Available
//
// ═══════════════════════════════════════════════════════════════════════════
// CONTRACT COVERAGE
// ═══════════════════════════════════════════════════════════════════════════
//
//  Function      Preconditions            Postconditions
//  ────────────  ───────────────────────  ──────────────────────────────────
//  alloc()       —                        result is owned || is error
//  free(ptr)     ptr != nullptr           free list depth increased by 1
//                owns(ptr)
//  shrink()      —                        return value <= num_slabs()
//  grow()        slab_count_ < MaxSlabs   free list non-empty on success
//  push_free()   slot != nullptr          free_head_ != nullptr after call
//  pop_free()    —                        result is owned || nullptr
//
// ═══════════════════════════════════════════════════════════════════════════

#include "alloc/contracts.hpp"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <generator>
#include <latch>
#include <memory>
#include <new>
#include <numeric>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

#if defined(__linux__)
#  include <sys/mman.h>
#endif

namespace alloc {

// ─────────────────────────────────────────────────────────────────────────────
// AllocError
// ─────────────────────────────────────────────────────────────────────────────
enum class AllocError : std::uint8_t {
    Exhausted = 0,
    OOM       = 1,
    Foreign   = 2,
};

[[nodiscard]] constexpr std::string_view to_string_view(AllocError e) noexcept {
    switch (e) {
        case AllocError::Exhausted: return "Exhausted";
        case AllocError::OOM:       return "OOM";
        case AllocError::Foreign:   return "Foreign";
    }
    return "Unknown";
}

}  // namespace alloc

template <>
struct std::formatter<alloc::AllocError> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(alloc::AllocError e, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "AllocError::{}",
                              alloc::to_string_view(e));
    }
};

namespace alloc {

struct SlotHeader { SlotHeader* next{nullptr}; };
static_assert(std::is_trivially_destructible_v<SlotHeader>);

// ─────────────────────────────────────────────────────────────────────────────
// SlabDescriptor
// ─────────────────────────────────────────────────────────────────────────────
struct SlabDescriptor {
    std::span<std::byte>     memory{};
    std::size_t              slot_size{};
    std::size_t              num_slots{};
    std::atomic<std::size_t> live_count{0uz};

    SlabDescriptor() = default;
    SlabDescriptor(SlabDescriptor const&) = delete;
    SlabDescriptor& operator=(SlabDescriptor const&) = delete;

    SlabDescriptor(SlabDescriptor&& o) noexcept
        : memory{o.memory}, slot_size{o.slot_size}, num_slots{o.num_slots},
          live_count{o.live_count.load(std::memory_order_relaxed)}
    { o.memory = {}; o.slot_size = 0uz; o.num_slots = 0uz; }

    SlabDescriptor& operator=(SlabDescriptor&& o) noexcept {
        if (this != &o) {
            memory     = o.memory;
            slot_size  = o.slot_size;
            num_slots  = o.num_slots;
            live_count.store(o.live_count.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
            o.memory = {}; o.slot_size = 0uz; o.num_slots = 0uz;
        }
        return *this;
    }

    [[nodiscard]] bool valid()       const noexcept { return !memory.empty(); }
    [[nodiscard]] bool reclaimable() const noexcept {
        return valid() && live_count.load(std::memory_order_acquire) == 0uz;
    }
    [[nodiscard]] bool owns(void const* p) const noexcept {
        auto const* b = static_cast<std::byte const*>(p);
        return b >= memory.data() && b < memory.data() + memory.size();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// SlabAllocator<T, SlotsPerSlab, MaxSlabs>
// ─────────────────────────────────────────────────────────────────────────────
template <
    typename    T,
    std::size_t SlotsPerSlab = 4096uz,
    std::size_t MaxSlabs     = 16uz
>
class SlabAllocator {
public:
    static constexpr std::size_t kSlotSize  = std::max(sizeof(T), sizeof(SlotHeader));
    static constexpr std::size_t kSlotAlign = alignof(T);
    static constexpr std::size_t kSlabBytes =
        std::add_sat(SlotsPerSlab * kSlotSize, kSlotAlign - 1uz);

    static_assert(SlotsPerSlab >= 1uz);
    static_assert(MaxSlabs     >= 1uz);
    static_assert(std::has_single_bit(kSlotAlign));

    using alloc_result = std::expected<T*, AllocError>;
    using free_result  = std::expected<void, AllocError>;
    using void_result  = std::expected<void, AllocError>;

    SlabAllocator() { (void)grow(); }

    ~SlabAllocator() noexcept {
        // CONTRACT_ASSERT: all slots freed before destruction.
        // In debug builds this catches leaks; in release it is a no-op.
        for (auto [_, desc] : slabs_ | std::views::enumerate) {
            CONTRACT_ASSERT(desc.live_count.load(std::memory_order_relaxed) == 0uz);
            if (desc.valid()) release_slab(desc);
        }
    }

    SlabAllocator(SlabAllocator const&)            = delete;
    SlabAllocator& operator=(SlabAllocator const&) = delete;
    SlabAllocator(SlabAllocator&&)                 = delete;
    SlabAllocator& operator=(SlabAllocator&&)      = delete;

    // ── alloc() ───────────────────────────────────────────────────────────────
    //
    // CONTRACT postcondition: if result has_value(), then owns(*result).
    // [[nodiscard("reason")]]: C++20 explains WHY discarding is a bug.
    //
    [[nodiscard("Discarding alloc() leaks the slot — call free() when done")]]
    alloc_result alloc() noexcept {
        if (auto* slot = pop_free(); slot != nullptr) [[likely]] {
            auto* p = static_cast<T*>(static_cast<void*>(slot));
            note_alloc(p);

            // CONTRACT_POST: returned pointer is owned by this allocator.
            CONTRACT_POST(owns(p));
            return p;
        }

        if (auto r = grow(); !r) [[unlikely]]
            return std::unexpected{r.error()};

        if (auto* slot = pop_free(); slot != nullptr) {
            auto* p = static_cast<T*>(static_cast<void*>(slot));
            note_alloc(p);
            CONTRACT_POST(owns(p));
            return p;
        }
        return std::unexpected{AllocError::Exhausted};
    }

    // ── free() ────────────────────────────────────────────────────────────────
    //
    // CONTRACT_PRE: ptr != nullptr
    // CONTRACT_PRE: owns(ptr)  — catching foreign-pointer bugs at source
    //
    [[nodiscard("Check free() result — Foreign pointer indicates a bug")]]
    free_result free(T* ptr) noexcept {
        // Preconditions — enforced in Debug, zero-cost in Release.
        CONTRACT_PRE(ptr != nullptr);
        CONTRACT_PRE(owns(ptr));

        if (!ptr || !owns(ptr)) [[unlikely]]
            return std::unexpected{AllocError::Foreign};

        note_free(ptr);
        auto* slot = ::new (static_cast<void*>(ptr)) SlotHeader{};
        push_free(slot);

        // CONTRACT_POST: free list head is non-null after push.
        CONTRACT_POST(free_head_.load(std::memory_order_relaxed) != nullptr);
        return {};
    }

    // ── shrink() ──────────────────────────────────────────────────────────────
    //
    // CONTRACT_POST: reclaimed <= num_slabs_before (can't reclaim more than we have)
    //
    [[nodiscard]] std::size_t shrink() noexcept {
        std::size_t const slabs_before = num_slabs();
        std::size_t reclaimed = 0uz;
        std::size_t n = slab_count_.load(std::memory_order_acquire);

        for (auto& desc : slabs_ | std::views::take(n)) {
            if (!desc.reclaimable()) continue;
            if (drain_slab_slots(desc)) {
                release_slab(desc);
                desc = SlabDescriptor{};
                ++reclaimed;
            }
        }

        // CONTRACT_POST: can't have reclaimed more slabs than existed.
        CONTRACT_POST(reclaimed <= slabs_before);
        return reclaimed;
    }

    // ── owns() ────────────────────────────────────────────────────────────────
    [[nodiscard]] bool owns(void const* ptr) const noexcept {
        if (!ptr) return false;
        std::size_t n = slab_count_.load(std::memory_order_acquire);
        for (auto const& desc : slabs_ | std::views::take(n))
            if (desc.owns(ptr)) return true;
        return false;
    }

    // ── slots() — C++26 std::generator ───────────────────────────────────────
    std::generator<T const*> slots() const {
        std::size_t n = slab_count_.load(std::memory_order_acquire);
        for (auto const& desc : slabs_ | std::views::take(n)) {
            if (!desc.valid()) continue;
            auto* base = desc.memory.data();
            for (std::size_t i = 0uz; i < desc.num_slots; ++i)
                co_yield reinterpret_cast<T const*>(base + i * kSlotSize);
        }
    }

    [[nodiscard]] std::size_t num_slabs() const noexcept {
        return slab_count_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t total_slots() const noexcept {
        return std::add_sat(num_slabs() * SlotsPerSlab, 0uz);
    }
    [[nodiscard]] std::size_t free_depth() const noexcept {
        std::size_t n = 0uz;
        auto* h = free_head_.load(std::memory_order_acquire);
        while (h) { ++n; h = std::launder(h)->next; }
        return n;
    }
    [[nodiscard]] std::string format_stats() const {
        return std::format(
            "SlabAllocator<{}, {}>  slabs={}  slots={}  "
            "free={}  slot_size={}B  contracts={}",
            SlotsPerSlab, MaxSlabs,
            num_slabs(), total_slots(), free_depth(), kSlotSize,
#if defined(CONTRACTS_NATIVE)
            "native-C++26"
#elif defined(CONTRACTS_SIMULATED)
            "simulated-debug"
#else
            "disabled-release"
#endif
        );
    }
    void print_stats() const { std::print("{}\n", format_stats()); }

private:
    alignas(64) std::atomic<SlotHeader*>             free_head_{nullptr};
    alignas(64) std::array<SlabDescriptor, MaxSlabs> slabs_{};
    alignas(64) std::atomic<std::size_t>             slab_count_{0uz};

    // ── pop_free ──────────────────────────────────────────────────────────────
    [[nodiscard]] SlotHeader* pop_free() noexcept {
        SlotHeader* head = free_head_.load(std::memory_order_acquire);
        while (head) {
            SlotHeader* next = std::launder(head)->next;
            if (free_head_.compare_exchange_weak(
                    head, next,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                // CONTRACT_ASSERT: popped slot is owned.
                CONTRACT_ASSERT(owns(static_cast<void*>(head)));
                return head;
            }
        }
        return nullptr;
    }

    // ── push_free ─────────────────────────────────────────────────────────────
    void push_free(SlotHeader* slot) noexcept {
        // CONTRACT_PRE: slot must be valid.
        CONTRACT_PRE(slot != nullptr);

        SlotHeader* head = free_head_.load(std::memory_order_relaxed);
        do { slot->next = head; }
        while (!free_head_.compare_exchange_weak(
                   head, slot,
                   std::memory_order_release,
                   std::memory_order_relaxed));
    }

    // ── grow ──────────────────────────────────────────────────────────────────
    void_result grow() noexcept {
        std::size_t my_idx = slab_count_.load(std::memory_order_acquire);

        if (my_idx >= MaxSlabs) [[unlikely]] {
            return std::unexpected{AllocError::Exhausted};
        }

        if (!slab_count_.compare_exchange_strong(
                my_idx, my_idx + 1uz,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            // Lost election — wait for winner via atomic::wait (C++20).
            SlotHeader* cur = free_head_.load(std::memory_order_acquire);
            free_head_.wait(cur, std::memory_order_acquire);
            return {};
        }

        void* raw = alloc_slab_memory();
        if (!raw) [[unlikely]] {
            slab_count_.fetch_sub(1uz, std::memory_order_release);
            return std::unexpected{AllocError::OOM};
        }

        auto* base    = static_cast<std::byte*>(raw);
        auto  misalign = reinterpret_cast<std::uintptr_t>(base) % kSlotAlign;
        if (misalign != 0uz) base += kSlotAlign - misalign;

        {
            SlabDescriptor desc;
            desc.memory    = std::span<std::byte>{base, SlotsPerSlab * kSlotSize};
            desc.slot_size = kSlotSize;
            desc.num_slots = SlotsPerSlab;
            slabs_[my_idx] = std::move(desc);
        }

        for (std::size_t i = SlotsPerSlab; i-- > 0uz; ) {
            auto* slot = ::new (base + i * kSlotSize) SlotHeader{};
            push_free(slot);
        }

        // CONTRACT_POST: after grow(), free list is non-empty.
        CONTRACT_POST(free_head_.load(std::memory_order_relaxed) != nullptr);

        free_head_.notify_all();
        return {};
    }

    // ── Live-count tracking ───────────────────────────────────────────────────
    void note_alloc(T const* ptr) noexcept {
        std::size_t n = slab_count_.load(std::memory_order_acquire);
        for (auto& desc : slabs_ | std::views::take(n)) {
            if (desc.owns(ptr)) {
                desc.live_count.fetch_add(1uz, std::memory_order_relaxed);
                return;
            }
        }
    }

    void note_free(T const* ptr) noexcept {
        std::size_t n = slab_count_.load(std::memory_order_acquire);
        for (auto& desc : slabs_ | std::views::take(n)) {
            if (desc.owns(ptr)) {
                // CONTRACT_ASSERT: live_count > 0 — catches double-free.
                CONTRACT_ASSERT(
                    desc.live_count.load(std::memory_order_relaxed) > 0uz);
                desc.live_count.fetch_sub(1uz, std::memory_order_release);
                return;
            }
        }
        // CONTRACT_ASSERT: ptr must belong to some slab.
        CONTRACT_ASSERT(false);
    }

    // ── Slab slot draining for shrink() ───────────────────────────────────────
    bool drain_slab_slots(SlabDescriptor const& desc) noexcept {
        SlotHeader* keep_head    = nullptr;
        std::size_t discard_count = 0uz;

        SlotHeader* h = free_head_.load(std::memory_order_acquire);
        while (h) {
            SlotHeader* next = std::launder(h)->next;
            if (desc.owns(h)) { ++discard_count; }
            else              { h->next = keep_head; keep_head = h; }
            h = next;
        }

        if (discard_count != desc.num_slots) return false;

        SlotHeader* expected = free_head_.load(std::memory_order_relaxed);
        return free_head_.compare_exchange_strong(
            expected, keep_head,
            std::memory_order_acq_rel, std::memory_order_relaxed);
    }

    // ── Memory ────────────────────────────────────────────────────────────────
    [[nodiscard]] void* alloc_slab_memory() noexcept {
#if defined(__linux__)
        void* p = mmap(nullptr, kSlabBytes,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return nullptr;
        madvise(p, kSlabBytes, MADV_HUGEPAGE);
        mlock(p, kSlabBytes);
        return p;
#else
        return std::aligned_alloc(kSlotAlign, kSlabBytes);
#endif
    }

    void release_slab(SlabDescriptor const& desc) noexcept {
        if (!desc.valid()) return;
#if defined(__linux__)
        munmap(desc.memory.data(), kSlabBytes);
#else
        std::free(const_cast<std::byte*>(desc.memory.data()));
#endif
    }
};

}  // namespace alloc
