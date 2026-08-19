# Upgrading to Native C++26 Contracts (GCC 16)

## Current Status

This project runs **102/102 tests** on GCC 14 with simulated contracts.
Contract enforcement uses `std::source_location` + a violation handler
that matches P2900R6's `std::contracts::contract_violation` interface exactly.

To enable **native C++26 contracts** (`[[pre:]]` `[[post:]]` `[[assert:]]`),
install GCC 16 and rebuild with `-fcontracts`.

---

## Install GCC 16 (Ubuntu 22.04 / 24.04)

```bash
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt-get install gcc-16 g++-16
g++-16 --version  # should show 16.x
```

---

## Build with Native Contracts

```bash
# Native C++26 contracts — zero overhead in Release
g++-16 -std=c++26 -fcontracts \
    -O3 -march=native -DNDEBUG \
    -Iinclude tests/test_cpp26.cpp -lpthread \
    -o test_cpp26_contracts
./test_cpp26_contracts
```

The header `include/alloc/contracts.hpp` automatically detects
`__cpp_contracts >= 202410L` and switches to native syntax:

```cpp
// contracts.hpp activates this branch on GCC 16:
#define CONTRACT_PRE(cond)    [[pre: cond]]
#define CONTRACT_POST(cond)   [[post: cond]]
#define CONTRACT_ASSERT(cond) [[assert: cond]]
```

No code changes required in `slab.hpp` or `pool.hpp`.

---

## Contract Build Modes (P2900R6 §6.4)

| Flag                              | Mode    | Overhead | Violations  |
|-----------------------------------|---------|----------|-------------|
| `-fcontracts` (default)           | enforce | O(cond)  | abort       |
| `-fcontracts -fcontract-mode=off` | ignore  | zero     | not checked |
| `-fcontracts -fcontract-mode=assume` | assume | zero  | UB if false |

The `assume` mode lets the optimiser treat contract conditions as **axioms**
for dead-code elimination and constant propagation — a unique C++26 optimisation
opportunity not available with `assert()`.

Example: `alloc()` with `post(r : owns(*r))` lets the compiler eliminate
null checks on the return value throughout the call chain.

---

## Contracts in This Project

Every public function has contracts:

```cpp
// alloc() — postcondition: result is owned
[[nodiscard("Discarding alloc() leaks the slot")]]
alloc_result alloc() noexcept {
    // ...
    CONTRACT_POST(owns(p));   // [[post: owns(p)]] on GCC 16
    return p;
}

// free() — preconditions: non-null, owned
free_result free(T* ptr) noexcept {
    CONTRACT_PRE(ptr != nullptr);  // [[pre: ptr != nullptr]]
    CONTRACT_PRE(owns(ptr));       // [[pre: owns(ptr)]]
    // ...
}

// grow() — postcondition: free list non-empty on success
void_result grow() noexcept {
    // ...
    CONTRACT_POST(free_head_.load(...) != nullptr);
}

// note_free() — assert: double-free detection
void note_free(T const* ptr) noexcept {
    CONTRACT_ASSERT(live_count > 0uz);  // fires on double-free
}

// ~SlabAllocator() — assert: no leaks at destruction
~SlabAllocator() {
    CONTRACT_ASSERT(desc.live_count == 0uz);  // fires if objects leaked
}
```

---

## Custom Violation Handler

Replace the default handler (which calls `std::abort()`) with your own:

```cpp
#include "alloc/contracts.hpp"

// Custom handler: log to trading system, then terminate gracefully.
void my_handler(alloc::contracts::violation_info const& v) noexcept {
    trading_log::critical("Contract {} violation: '{}' at {}:{}",
        v.kind, v.condition,
        v.location.file_name(), v.location.line());
    std::terminate();
}

// Install before using the allocator:
alloc::contracts::violation_handler = &my_handler;
```

On GCC 16 with `-fcontracts`, native violations go through
`std::contracts::set_handler()` instead — the interface is equivalent.
