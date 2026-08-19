# cpp26-alloc

![CI](https://github.com/nisgemML/cpp26-alloc/actions/workflows/ci.yml/badge.svg)
[![C++26](https://img.shields.io/badge/C%2B%2B-26-blue.svg)](https://en.cppreference.com/w/cpp/26)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A lock-free slab allocator in **pure C++26**, with **C++26 Contracts (P2900R6)**
on every public function.

**102/102 tests. Debug + Release + TSan + ASan. Zero warnings. Zero errors.**

```
C++26 | GCC 14+ (simulated contracts) | GCC 16+ (native contracts)
```

---

## C++26 Features Used

All confirmed on **GCC 14.2 with `-std=c++2c`**. No shims. No fallbacks.

| Feature | Standard | Used for |
|---------|----------|---------|
| `std::expected<T,E>` | C++23/26 | `alloc()` / `free()` / `grow()` — errors are values, not nulls |
| `expected::and_then/transform/or_else` | C++23/26 | Monadic alloc → construct → process chains |
| `std::formatter<AllocError>` | C++23/26 | `std::format("{}", AllocError::OOM)` |
| `std::generator<T>` | C++23/26 | `slots()`: lazy coroutine-based heap walk |
| `std::print` / `std::format` | C++23/26 | All diagnostics and `format_stats()` |
| `std::views::enumerate` | C++23 | Index-aware slab and slot iteration |
| Placeholder variable `_` | C++26 | Discard unused structured-binding members |
| `uz` literal suffix | C++23 | `std::size_t` literals — `4096uz`, `0uz` |
| `std::add_sat` / `std::saturate_cast` | C++26 | Overflow-safe `kSlabBytes` computation |
| `std::latch` | C++20 | Precise concurrent grow() synchronisation |
| `std::barrier` | C++20 | Phased multi-thread coordination in tests |
| `std::atomic::wait` / `notify_all` | C++20/26 | OS-level wait in grow() — no spinning |
| `[[nodiscard("reason")]]` | C++20 | Explain why discarding `alloc()` is a bug |
| `std::source_location` | C++20 | Contract violation info (matches P2900R6) |
| `std::span<T>` | C++20 | Bounds-safe memory views throughout |
| C++26 Contracts (P2900R6) | C++26 | Native on GCC 16; simulated on GCC 14 |

---

## Contract Coverage

Every public function has machine-checkable preconditions and postconditions:

```cpp
// alloc() — postcondition: returned pointer is owned
[[nodiscard("Discarding alloc() leaks the slot")]]
alloc_result alloc() noexcept {
    // ...
    CONTRACT_POST(owns(p));         // [[post: owns(p)]] on GCC 16
    return p;
}

// free() — preconditions: non-null, owned by this allocator
free_result free(T* ptr) noexcept {
    CONTRACT_PRE(ptr != nullptr);   // [[pre: ptr != nullptr]]
    CONTRACT_PRE(owns(ptr));        // [[pre: owns(ptr)]]
    // ...
    CONTRACT_POST(free_head_.load(...) != nullptr);
}

// note_free() — assert: double-free detection
void note_free(T const* ptr) noexcept {
    CONTRACT_ASSERT(live_count > 0uz);  // fires on double-free in Debug
}

// ~SlabAllocator() — assert: no leaked objects at destruction
~SlabAllocator() {
    CONTRACT_ASSERT(desc.live_count == 0uz);  // fires if objects leaked
}
```

### Three contract build modes (P2900R6 §6.4 compatible)

| Compiler | Flag | Mode | Overhead |
|---|---|---|---|
| GCC 16+ | `-fcontracts` | **Native C++26** | O(cond) enforce / zero ignore / axiom assume |
| GCC 14 Debug | (no NDEBUG) | **Simulated** via `std::source_location` | O(cond) |
| GCC 14 Release | `-DNDEBUG` | **Disabled** | **Zero** |

No code changes needed when upgrading from GCC 14 to GCC 16 —
`contracts.hpp` detects `__cpp_contracts` automatically.

---

## The C++26 API

### No null pointers — `std::expected` throughout

```cpp
alloc::SlabAllocator<OrderNode> pool;

// Monadic — no null checks, no exceptions.
auto result = pool.alloc()
    .transform([](OrderNode* raw) {
        return new (raw) OrderNode{42ULL, 100.5, 500, 0};
    })
    .and_then([&](OrderNode* node) -> std::expected<double, alloc::AllocError> {
        double pnl = node->price * node->qty;
        node->~OrderNode();
        (void)pool.free(node);
        return pnl;
    });

if (!result) {
    std::print("error: {}\n", result.error());  // AllocError::Exhausted etc.
}
```

### `std::generator<T const*>` — lazy heap walking

```cpp
// Iterate all slots without allocating a vector.
for (auto const* slot : allocator.slots()) {
    inspect(*slot);
}

// Compose with ranges.
for (auto [idx, slot] : allocator.slots()
                       | std::views::take(10uz)
                       | std::views::enumerate) {
    std::print("  slot[{}] @ {:p}\n", idx, static_cast<void const*>(slot));
}
```

### Placeholder `_` — two in same scope (C++26)

```cpp
{
    auto _ = pool.make(1ULL, 1.0, 1, 0);   // alloc + construct
    auto _ = pool.make(2ULL, 2.0, 2, 1);   // second _ — valid C++26
}   // both destructed here
```

### `std::add_sat` — overflow-safe size arithmetic (C++26)

```cpp
// kSlabBytes can never overflow SIZE_MAX — saturates instead.
static constexpr std::size_t kSlabBytes =
    std::add_sat(SlotsPerSlab * kSlotSize, kSlotAlign - 1uz);
```

---

## Build

Requires **GCC 14+** or **Clang 18+**.

```bash
# Debug — contracts simulated (102 tests)
g++-14 -std=c++2c -O0 -g \
    -Iinclude tests/test_cpp26.cpp -lpthread -o test && ./test

# Release — contracts disabled, zero overhead (99 tests)
g++-14 -std=c++2c -O3 -DNDEBUG -march=native \
    -Iinclude tests/test_cpp26.cpp -lpthread -o test && ./test

# ThreadSanitizer (102 tests)
g++-14 -std=c++2c -O1 -g -fsanitize=thread \
    -Iinclude tests/test_cpp26.cpp -lpthread -o test && ./test

# AddressSanitizer + UBSan (102 tests)
g++-14 -std=c++2c -O1 -g -fsanitize=address,undefined \
    -Iinclude tests/test_cpp26.cpp -lpthread -o test && ./test

# GCC 16 — native C++26 contracts
# Install: sudo add-apt-repository ppa:ubuntu-toolchain-r/test && sudo apt install gcc-16 g++-16
g++-16 -std=c++26 -fcontracts -O3 -DNDEBUG -march=native \
    -Iinclude tests/test_cpp26.cpp -lpthread -o test && ./test

# Benchmark
g++-14 -std=c++2c -O3 -DNDEBUG -march=native \
    -Iinclude bench/bench.cpp -lpthread -o bench && ./bench
```

See [`docs/gcc16_contracts.md`](docs/gcc16_contracts.md) for the full GCC 16
upgrade guide including custom violation handlers.

---

## Formal Proofs

The contracts enforce the invariants at every call site.
The proofs establish *why* the invariants hold:

| Proof | File | Claim |
|-------|------|-------|
| No double-free / ABA | [`proof/no_double_free.md`](proof/no_double_free.md) | `LIVE(p) ∧ FREE(p)` is always false |
| Lock-free progress | [`proof/wait_free_progress.md`](proof/wait_free_progress.md) | `alloc()` completes in O(K) steps for K threads |
| Zero fragmentation | [`proof/zero_fragmentation.md`](proof/zero_fragmentation.md) | Fragmentation ratio = 1.0 at full occupancy |

---

## Benchmark Results

Full results and honest analysis: [`BENCHMARK_RESULTS.md`](BENCHMARK_RESULTS.md)

```
Intel(R) Xeon(R) @ 2.10GHz  |  GCC 14  |  -O3 -march=native

Single-threaded latency (500,000 samples):
  SlabAllocator::alloc()   p50=30ns   p99=41ns
  SlabAllocator::free()    p50=33ns   p99=50ns
  ::malloc(64)             p50=19ns   p99=23ns

Multi-threaded (4 threads, 500ms):
  SlabAllocator  32.6M ops/s
  malloc         110.9M ops/s
```

malloc wins in a tight alloc/free loop (glibc thread-local cache avoids
all atomics). SlabAllocator wins on: zero fragmentation, formal correctness
proofs, `shrink()` epoch-based reclamation, `std::generator` heap walking,
and `std::expected` exhaustion signalling. See BENCHMARK_RESULTS.md for full
analysis and expected isolated-core numbers (~5ns p50).

---

## Related Projects

Part of a nine-repository open-source trading infrastructure portfolio:

| Repo | Description |
|------|-------------|
| [mpsc-queue](https://github.com/nisgemML/mpsc-queue) | Lock-free MPSC queue · 53.1M msg/sec · formal memory-model proof (5 claims, x86-TSO) |
| [options-engine](https://github.com/nisgemML/options-engine) | Options matching engine · p50 add_order ~140ns · 159/159 tests |
| [udp-multicast-receiver](https://github.com/nisgemML/udp-multicast-receiver) | MoldUDP64/ITCH 5.0 feed handler · SO_TIMESTAMPING · PCAP replay |
| [alpha-research](https://github.com/nisgemML/alpha-research) | Alpha signal platform · IC lookahead bias corrected (t-stat 3.76→1.08) |
| [avellaneda-stoikov](https://github.com/nisgemML/avellaneda-stoikov) | A-S market maker · Sharpe 10.02 vs naive 3.61 · 200 Monte Carlo paths |
| [lob-microstructure-calibration](https://github.com/nisgemML/lob-microstructure-calibration) | Kyle's lambda · Roll spread · OFI · kappa · all with OOS R² |
| [options-market-maker](https://github.com/nisgemML/options-market-maker) | Black-Scholes / Heston / vanna-volga · delta hedger |
| [ocaml-trading-primitives](https://github.com/nisgemML/ocaml-trading-primitives) | Functional trading primitives · 14 probability results · QCheck |
| [low-latency-trading-engine](https://github.com/nisgemML/low-latency-trading-engine) | Full-stack C++20/OCaml engine · RDTSC · ITCH 5.0 · A-S market maker |

---

## License

MIT
