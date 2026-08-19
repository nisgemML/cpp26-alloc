#pragma once
// alloc/contracts.hpp — C++26 Contracts (P2900R6) compatibility layer.
//
// ═══════════════════════════════════════════════════════════════════════════
// DESIGN
// ═══════════════════════════════════════════════════════════════════════════
//
// C++26 Contracts (P2900R6) are available in GCC 16 with -fcontracts
// and in Clang with -fexperimental-contracts.
//
// This header provides three build modes, selected automatically:
//
//  Mode 1 — Native C++26 contracts (GCC 16+ / Clang with -fcontracts)
//    CONTRACT_PRE(cond)    → [[pre: cond]]
//    CONTRACT_POST(cond)   → [[post: cond]]
//    CONTRACT_ASSERT(cond) → [[assert: cond]]
//    Violation handler: std::contracts::invoke_default_contract_violation_handler
//    Zero overhead in contract_build_level=ignore (Release).
//    Used as axioms by the optimiser in contract_build_level=assume.
//
//  Mode 2 — Enforced simulation (Debug builds without native contracts)
//    CONTRACT_PRE(cond)    → if (!cond) contract_violation_handler({...})
//    CONTRACT_POST(cond)   → if (!cond) contract_violation_handler({...})
//    CONTRACT_ASSERT(cond) → if (!cond) contract_violation_handler({...})
//    Uses std::source_location::current() — matches P2900R6 violation info.
//    [[noreturn]] handler: prints condition + location then std::abort().
//
//  Mode 3 — Zero overhead (NDEBUG without native contracts)
//    All CONTRACT_* macros expand to nothing.
//    Conditions are NOT evaluated — matches contract_build_level=ignore.
//
// ── How to upgrade to native contracts (GCC 16) ───────────────────────────
//
//   g++-16 -std=c++26 -fcontracts -Iinclude ...
//
//   The macro layer automatically detects __cpp_contracts and switches
//   to native syntax. No code changes required in slab.hpp or pool.hpp.
//
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdlib>
#include <cstdio>
#include <source_location>
#include <string_view>

namespace alloc::contracts {

// ── Contract violation info — mirrors std::contracts::contract_violation ──
// P2900R6 §5.3: violation info carries condition text + source location.
struct violation_info {
    std::string_view      condition;
    std::string_view      kind;       // "pre", "post", or "assert"
    std::source_location  location;
};

// ── Default violation handler — matches P2900R6 default behaviour ─────────
// Prints to stderr and calls std::abort().
// Replace with your own handler for custom behaviour (logging, exceptions).
[[noreturn]] inline void default_handler(violation_info const& v) noexcept {
    printf(stderr,
        "\nContract violation ({}): '{}'\n"
        "  at {}:{} in {}\n",
        v.kind, v.condition,
        v.location.file_name(),
        v.location.line(),
        v.location.function_name());
    std::abort();
}

// ── Violation handler function pointer ────────────────────────────────────
// Replace at startup for custom handling:
//   alloc::contracts::violation_handler = &my_handler;
inline void (*violation_handler)(violation_info const&) noexcept
    = &default_handler;

// ── Internal check helper ─────────────────────────────────────────────────
inline void check(bool condition,
                  std::string_view cond_str,
                  std::string_view kind,
                  std::source_location loc =
                      std::source_location::current()) noexcept
{
    if (!condition) [[unlikely]] {
        violation_handler({cond_str, kind, loc});
    }
}

}  // namespace alloc::contracts

// ═══════════════════════════════════════════════════════════════════════════
// Public macros — three modes
// ═══════════════════════════════════════════════════════════════════════════

#if defined(__cpp_contracts) && __cpp_contracts >= 202410L
// ── Mode 1: Native C++26 contracts ────────────────────────────────────────
// GCC 16+ with -fcontracts or Clang with -fexperimental-contracts.
#  define CONTRACT_PRE(cond)    [[pre: cond]]
#  define CONTRACT_POST(cond)   [[post: cond]]
#  define CONTRACT_ASSERT(cond) [[assert: cond]]
#  define CONTRACTS_NATIVE 1

#elif defined(NDEBUG)
// ── Mode 3: Zero overhead release ─────────────────────────────────────────
// Conditions not evaluated — matches contract_build_level=ignore.
#  define CONTRACT_PRE(cond)    /* pre: cond */
#  define CONTRACT_POST(cond)   /* post: cond */
#  define CONTRACT_ASSERT(cond) /* assert: cond */
#  define CONTRACTS_DISABLED 1

#else
// ── Mode 2: Enforced simulation (Debug) ───────────────────────────────────
// std::source_location gives precise violation info matching P2900R6.
#  define CONTRACT_PRE(cond) \
     ::alloc::contracts::check((cond), #cond, "pre", \
         std::source_location::current())
#  define CONTRACT_POST(cond) \
     ::alloc::contracts::check((cond), #cond, "post", \
         std::source_location::current())
#  define CONTRACT_ASSERT(cond) \
     ::alloc::contracts::check((cond), #cond, "assert", \
         std::source_location::current())
#  define CONTRACTS_SIMULATED 1
#endif
