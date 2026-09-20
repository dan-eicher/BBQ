# crt — the C container runtime

Six containers that generated code, the BBQ tools, javelina's compiler and
yoctojc's build tool all allocate through. They are small on purpose, but they
are not incidental: `javelina/compiler` alone holds ~3,400 references to them,
and BBQ's generated readers keep their parse stacks in `bbq_vec`.

Because they sit inside a compiler and a VM that process input nobody here
wrote, the contract below is about what happens when things go wrong, not only
about what the containers do.

## The contract

**Nothing aborts.** A library that calls `abort()` on a bad day hands whoever
supplies the input a way to kill the process. Allocation failure is a value
here, never a fatal event.

**Nothing writes out of bounds, on any path.** This is the property the rest of
the design exists to protect.

**Failure is sticky.** When an allocation is refused, the container is
*poisoned*: further mutations are no-ops, what is already in it stays valid and
readable, and `bbq_X_oom()` reports it. A caller that wants to know whether its
data is whole asks once, at a boundary that suits it; a caller that never asks
gets a short container, never a corrupted heap.

Lengths are preserved rather than zeroed on failure, deliberately. A container
that silently emptied itself would turn a partial result into a confidently
wrong one — a method body that looks complete — where a short container plus a
set flag is something a caller can recognise.

**Every allocation goes through a `bbq_alloc*`** (`NULL` = libc), captured at
init and kept for the container's lifetime, so nothing can be freed through an
allocator other than the one that allocated it.

**Two entry points, everywhere.** `bbq_X_init(...)` allocates from libc, which is
right for a compiler pass, a build tool or a test. `bbq_X_init_a(..., alloc)`
names an allocator, and anything processing untrusted input wants it — that is
where a budget goes. `bbq_htree` and `bbq_dict` additionally offer
`_create`/`_create_a`/`_destroy` for callers that thread a handle through
signatures rather than owning the storage; the handle comes from the same
allocator, so a budget still covers everything.

**Sizes are `size_t` internally and every growth is overflow-checked.** A request
too large to represent is refused; it never wraps into a small one.

**Hashes are seeded** from the OS, once per process.

**Nothing is thread-safe**, and nothing holds process-global mutable state.

### The one thing the sticky contract breaks

A loop whose termination depends on a mutation making progress no longer
terminates:

```c
while (bbq_vec_len(v) < need) bbq_vec_push(v, 0);   /* HANGS once poisoned */
```

A hang inside a VM is the same denial of service as an abort, so this shape is a
bug and no macro can diagnose it. Use `bbq_vec_fill`, or `bbq_vec_try_reserve`
and check. The same applies wherever an external counter is advanced past a push
and then used as an index.

## Bounding a hostile workload

Handling `malloc` returning `NULL` is not by itself a defence. On Linux with
overcommit `malloc` essentially never returns `NULL`; the kernel hands out the
mapping and the OOM killer arrives later, which is the same process death by
another route.

What bounds a hostile workload is a **ceiling**:

```c
bbq_budget b;
bbq_budget_init(&b, 64u << 20, NULL);        /* this evaluation gets 64 MB */
bbq_htree t;
bbq_htree_init(&t, bbq_budget_handle(&b));
...
if (bbq_htree_oom(&t)) return RESOURCE_EXHAUSTED;
```

Past the ceiling, allocation fails deterministically and every container is
required to survive it. `b.peak` and `b.denials` say what the workload actually
cost and whether it hit the wall.

The same mechanism is what makes the failure paths testable: `bbq_faulty` fails
the Nth allocation, so a test can sweep N across every allocation an operation
makes. That is sqlite's OOM discipline, and it is why these paths are covered
rather than merely present.

## The containers

| | for | notes |
|---|---|---|
| `bbq_vec` | growable array | `T*` with a hidden header; `NULL` is a valid empty vector |
| `bbq_buf` | byte buffer | append returns whether it took |
| `bbq_arena` | bump allocation | per-page occupancy, so nothing is stranded |
| `bbq_hmap` | 64-bit keys | open addressing, no deletion by design |
| `bbq_htree` | 64-bit keys, ordered | trie: no hash, no rehash, ascending iteration |
| `bbq_dict` | byte-string keys | seeded SipHash over `bbq_htree`, chained on collision |

### Choosing between `bbq_hmap` and `bbq_htree`

`bbq_htree` is a trie, so a key's position *is* the key: no hash, no rehash, and
iteration in ascending key order for free. Lookup is bounded by depth rather than
by the number of entries.

`bbq_hmap` is one contiguous array — mix the key, index, compare, usually one
cache line. A trie walk is a chain of dependent loads that cannot be prefetched.

Use `bbq_hmap` when you are asking "what did I assign this object?" millions of
times against keys you minted. Use `bbq_htree` when you want ordered iteration,
or a key space you do not control and do not want to rehash.

### `bbq_htree` and lazy expansion

A leaf sits at the shallowest depth that distinguishes its key, not at the
bottom. One key in an empty tree hangs directly off the root; a second pushes
both down only as far as the first nibble where they differ. This is the first
of the two ideas in ART (Leis, Kemper & Neumann, ICDE 2013).

It is the difference between a tree whose cost is the *key width* and one whose
cost is the *number of keys*. Measured with `mallinfo2`, before and after:

| keys | was | now |
|---|---|---|
| dense `0..N` | 186 B/key | see `BbqHtree.LazyExpansionKeepsSparseKeysCheap` |
| pointer-strided | 330 B/key | |
| sparse random | **681 B/key** | |

The second ART idea — adaptive node sizes, `Node4`/`16`/`48`/`256` instead of a
fixed 16-wide array — is **not** implemented. A sparse node still costs 16
pointers. That is the next thing to do here if the memory matters.

## Where each idea comes from

- Injected allocator with a ceiling: Lua's `lua_Alloc`, sqlite's
  `sqlite3_mem_methods`. Refusing an allocation is a supported outcome, not a
  crash.
- Sticky error state: sqlite. The handle latches a failure, later calls are safe
  no-ops, one query at a boundary reports it — which is why sqlite call sites are
  not littered with per-call checks.
- Seeded hashing: the answer to hash flooding (Klink & Wälde, 28C3 2011), via
  SipHash (Aumasson & Bernstein, 2012), as adopted by Python, Ruby, Rust and
  Perl. SipHash-1-3 here.
- Exhaustive allocation-fault injection: sqlite's OOM harness.
- Iterator generation counters: Java's `modCount` fail-fast discipline.
- Lazy expansion: ART (Leis, Kemper & Neumann, ICDE 2013).

## Tests

```
ctest --test-dir build -R crt_tests --output-on-failure
```

`test/crt_test.cpp` is organised by the contract rather than by what has broken
before, so a law without a test is visible as a gap. It runs with
`ASAN_OPTIONS=detect_leaks=1` — a suite whose main subject is failure paths, run
with leak detection off, would be running with its principal assertion disabled.

Each invariant and the test that keeps it:

| invariant | test |
|---|---|
| a refused allocation writes nothing, ever | `BbqVecOom.*`, `BbqBufOom.*` |
| poison is sticky and keeps what was already there | `BbqVecOom.PoisonIsStickyAndKeepsWhatWasAlreadyThere` |
| every allocation failure point leaves a usable container | `Bbq{Arena,Htree,Hmap,Dict}Oom.*` (exhaustive sweeps) |
| everything taken is given back, at the size it was taken | `BbqOom.EveryContainerGivesBackExactlyWhatItTook`, `BbqArenaOom.EveryFailurePointGivesBackExactlyWhatItTook` |
| a budget covers the handle, not just the contents | `Bbq{Htree,Dict}.TheHandleItselfIsChargedToTheAllocator` |
| growth refuses rather than wrapping | `BbqVec.ReserveBeyondTheCeiling…`, `BbqArena.HugeRequestIsRefused…`, `BbqBuf.AppendThatWouldOverflow…` |
| no non-terminating growth loop | `BbqBuf.HugeReserveIsRefusedRatherThanLoopingForever`, `BbqHmap.AnUnrepresentableCapacityIsRefused…` (the ctest TIMEOUT is half of each assertion) |
| a full-width key is not narrowed | `BbqHtreeAdversarial.PointersDifferingOnlyAboveBitThirtyOne` |
| the trie stays shallow for sparse keys | `BbqHtree.LazyExpansionKeepsSparseKeysCheap` |
| the hash cannot be precomputed against | `BbqDictAdversarial.TheHashIsSeeded…` |
| a collider set does not degrade lookup | `BbqDictAdversarial.DjbTwoColliderSetDoesNotDegradeLookup` |
| a collision is a wasted hop, never a wrong answer | `BbqDict.CollidingKeysStayDistinctEntries`, `…EveryUnlinkPositionInACollisionChain` |
| an abandoned iteration leaks nothing | `BbqHtreeIter.Abandoning…`, `BbqDictIter.Abandoning…` |
| mutating mid-walk stops the walk | `Bbq{Htree,Dict}Iter.MutatingMidWalk…` |
| they behave like the things they claim to be | `Bbq{Htree,Hmap,Dict,Vec}Differential.*` vs `std::` oracles |

A release that passes the wrong size is invisible to a leak checker — the pointer
*is* freed — so the two "gives back exactly what it took" sweeps are the only
thing that sees it. That is what caught the arena growing its three parallel page
arrays one at a time and leaving the capacity that frees them behind.

The consumers that run on input nobody here wrote have their own ceiling sweeps,
against the same contract: `CrossBackend.ViewCReaderUnderAnArenaCeiling` in
`test/cross_backend_test.cpp` re-parses under every arena ceiling and fails on a
parse that reports success with a short index, and
`test_a_refused_arena_stops_labelling` in `burgc/tests/data/coverage_main.c` does
the same for the BURS labeller.

The collision chain is reached by installing a deliberately weak hash through
`bbq_dict_init_hashed`. With a seeded 64-bit hash, finding two colliding keys is
a birthday problem over 2^64, so the unlink paths would otherwise ship untested.

## SEE ALSO

`backends/c/runtime/bbq_read.h` (the generated reader's parse stacks),
`burgc/runtime/egraph.c` (the largest in-tree consumer).
