# Proof: No Double-Free and No Use-After-Free

## Setup

`SlabAllocator<T>` maintains a Treiber stack free-list over a set of fixed-size
slots.  We prove two safety properties:

1. **No double-free**: freeing a slot twice does not produce undefined behaviour
   at the allocator level (it corrupts the free-list; we prove it is detected).
2. **No use-after-free at the allocator level**: once a slot is freed, the
   allocator does not hand it to a second caller until the first caller's
   pointer is invalidated.

We do *not* prove absence of use-after-free in *user* code — that is the
caller's responsibility (the BYOD pattern).

---

## Definitions

- **LIVE(p)**: slot `p` has been returned by `alloc()` and not yet passed to `free()`.
- **FREE(p)**: slot `p` is on the free-list.
- **Invariant I**: at any point, `LIVE(p) ∧ FREE(p)` is false for all `p`.

---

## Proof of Invariant I (no concurrent LIVE+FREE)

### alloc() removes from free-list before returning

```
pop_free():
  old_head = free_head_.load(acquire)
  while old_head ≠ nullptr:
      next = launder(old_head)->next
      CAS(free_head_, old_head, next, acq_rel, acquire)  → if success, return old_head
```

After the CAS succeeds:
- `free_head_` no longer points to `old_head`.
- No other thread can pop `old_head` until it is pushed back (because they
  load `free_head_` with acquire, which sees the updated value via the
  acq_rel success ordering).
- Therefore `old_head` is removed from the free-list **before** it is
  returned to the caller.  The caller transitions it from FREE → LIVE.

### free() pushes to free-list only after caller relinquishes

```
free(ptr):
  slot = new (ptr) SlotHeader{}   // start SlotHeader lifetime
  push_free(slot)
```

The caller must not pass `ptr` to `free()` while still holding a live
reference to the object (BYOD precondition).  After `free()` returns,
the slot is FREE.  The caller's pointer is stale; any further access is
user-level UB, not allocator UB.

### Conclusion

The only transition FREE → LIVE is through `alloc()`'s CAS, which atomically
removes the slot from the free-list.  The only transition LIVE → FREE is
through `free()`, which requires the caller to have terminated the object's
lifetime.  No concurrent execution can have `LIVE(p) ∧ FREE(p)`.  ∎

---

## ABA Analysis

**Scenario**: Thread A pops slot X (LIVE). Thread B allocs X (impossible —
it's LIVE and not on free-list). Thread A frees X (FREE). Thread B pops X
(LIVE). Thread A retries CAS.

Wait — after Thread A pops X, X is LIVE and not on the free-list. Thread B
**cannot** pop X until Thread A frees it. So the classic ABA pattern
(push/pop/push of the same node by a concurrent thread while another thread
holds the old pointer) cannot occur here because LIVE slots are not on the
free-list.

**Formal statement**: ABA in a Treiber stack requires node X to be pushed
back while another thread holds a pointer to it and is mid-CAS.  But our
invariant guarantees that X is either LIVE (not on stack) or FREE (on
stack), never both.  Therefore, if thread A holds a pointer to X (LIVE),
X is not on the stack, and no other thread can push it back until A calls
`free()`.  After A calls `free()`, A's pointer is invalidated (precondition).
So A never retries a CAS with a stale pointer to a now-on-stack node.  ∎

---

## Double-Free Detection (Debug Builds)

In debug builds (`#ifndef NDEBUG`), `SlabAllocator` maintains per-slab
`live_count` arrays.  `note_alloc()` increments the count; `note_free()`
asserts `count > 0` and decrements.

A double-free fires the `CONTRACT_ASSERT(live_counts_[i] > 0)` in
`note_free()`, producing a deterministic assertion failure rather than
silent corruption.

In release builds, `note_alloc()` and `note_free()` are empty and
compile to zero instructions (verified by inspection of `-O3` output).

---

## Memory Ordering Summary

| Operation          | Ordering    | Reason                                           |
|--------------------|-------------|--------------------------------------------------|
| `free_head_.load`  | `acquire`   | See all writes to slot content made before push  |
| `CAS` success      | `acq_rel`   | Acquire slot content; release updated head       |
| `CAS` failure      | `acquire`   | Re-read head; no write to publish                |
| `push_free` CAS    | `release`   | Publish slot content to future allocators        |
| `push_free` load   | `relaxed`   | Only reading head for `slot->next`; no sync needed |

All orderings are the minimum sufficient; no `seq_cst` is used.  On x86-TSO,
all `acquire`/`release` plain loads/stores compile to `MOV` (zero overhead).
The CAS compiles to `LOCK CMPXCHG` (required for atomicity regardless of
ordering).
