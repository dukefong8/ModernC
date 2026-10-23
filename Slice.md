# Arena Slice Design Review

Status: working review of the arena-backed Slice API against
`/Users/duke/dev/goose/docs/goose_spec.md`.

Scope reviewed:

- `include/arena.h` — Slice classes, ownership tags, Push/Reserve/Clone handling.
- `src/slice_tests.c` and `src/cspan_tests.c` — runtime tests.
- `test/typecheck/{pass,fail}` — compile-time fixtures.

Not reviewed here:

- Unrelated application/demo code.

---

## 1. Current slice model

### `Slice(T)`

`Slice(T)` is a plain descriptor union type. The macro expands directly to an
anonymous union, so the result is a normal type name usable in a typedef, a
struct field, `sizeof`, `New`, or a function signature:

```c
typedef struct { unsigned char _; } MovableKind;

#define Slice(T)       \
  union {              \
    struct { T *data; isize len; isize cap; }; \
    MovableKind _kind; \
  }

#define FixedSlice(T)  \
  union {              \
    struct { T *data; isize len; isize cap; }; \
    FixedKind _kind;   \
  }
```

A movable `Slice` descriptor is arena-owned. Allocate it with `New`, which
zero-initializes it into a valid empty slice, and pass the pointer to every
operation:

```c
typedef Slice(int) ints;

ints* s = New(arena, ints);
Push(arena, s, 42);
int v = s->data[0];
```

A `FixedSlice` descriptor is a value owned by its scope or containing record;
store and pass it by value and take its address at the operation:

```c
fints b = {0};
SliceFixed(arena, &b, 16);
Push(&b, 42);
```

Name the type with a typedef: each `Slice(T)` expansion is a distinct
anonymous union type, so use the same typedef for allocation, parameters, and
fields. A movable descriptor lives behind a pointer field (`ints* items;`);
a fixed descriptor can sit inline (`fints items;`). Movable growth may
relocate `data`; fixed data never relocates. Plain descriptor copies are legal
C and alias silently, so `Clone` remains the explicit element copy. See the
storage/passing rubric below for the full comparison.

`_kind` overlays `data` and is never read at run time; it lets `_Generic`
select the movable (`MovableKind`) or fixed (`FixedKind`). Descriptor size,
offsets, and alignment match `SliceInternal`.

### Operations

- `SliceFromArray(arena, dst, a)` — copy into
  arena storage (`cap == len`); source lifetime no longer matters.
- `SliceFixed(arena, &b, cap)` — initialize the fixed class.
- `Push(s, v)` — structural checked no-growth append; works for either class,
  never allocates or relocates, returns the new index or -1 when full
  (nodiscard). Pass a fixed value as `&b`.
- `Push(arena, s, v)` — the only growing form, movable class only; may
  relocate. `Reserve(arena, s, n)` may relocate too.
- `At(s, i)` — bounds-checked element access, an lvalue valid across moves.
- `Clone(arena, out, src[, start[, len]])` — destination-only copy;
  works on either slice class.
- `PushUnchecked(s, v)` — unchecked in-capacity write.
- `Pop(s, &out)` — last element into `*out`, false when empty (nodiscard).
- `SliceAtTip(arena, s)` — momentary query: true when the non-empty backing
  block ends at the arena tip, so an immediate grow/reserve extends in place.
  Any later allocation changes the tip; this is not a lifetime or no-move
  guarantee. For stable references, reserve capacity first, then use a
  non-growing append.

### Storage and passing rubric

Movable `Slice` is stored and passed by pointer. `FixedSlice` is stored and
passed by value; fixed operations take the address of that value.

| Aspect | Movable `Slice` (store/pass pointer) | `FixedSlice` (store/pass value) |
|---|---|---|
| Declaration | `ints* s = New(arena, ints);` | `fints b = {0};` |
| Struct field | `ints* items;` | `fints items;` |
| Passing | `ints*`; all callers share descriptor identity | `fints`; callee gets a view, use `&local` for ops |
| Descriptor lifetime | arena | owner scope / containing record |
| Data lifetime | arena | arena |
| Descriptor copy | value copy pins the old block and goes stale after growth | value copy remains a valid view of fixed storage |
| Aliasing | pointer aliases see every mutation; value copies do not follow relocation | copies share data; `len`/`cap` are per-descriptor |
| Mutation | through the pointer is visible everywhere | data writes are shared; appends advance only the local copy |
| Relocation | off-tip growth may relocate `data` | never relocates; there is no growth path |
| Return by value | possible, but callers must not grow an older copy | supported and safe |
| At-tip query | `SliceAtTip(arena, s)` is advisory: true only for an immediate in-place growth | same query works; fixed data never moves anyway |
| Cost | arena descriptor + pointer indirection | inline descriptor, no allocation |

Pros and cons:

- **Movable by pointer** — Pros: one descriptor identity, so `len`/`cap` and
  relocation stay coherent across callers; arena owns the descriptor.
  Cons: an extra pointer; a value copy is a trap because `data` can relocate.
  Tested by `movable_by_pointer_aliasing_and_relocation`,
  `movable_descriptor_lifetime`, and `fixed_never_relocates_movable_does`.
- **Fixed by value** — Pros: no descriptor allocation or indirection; can be
  returned by value, embedded in records, and copied freely because the
  backing block never moves. Cons: copies have independent `len`/`cap`, so
  appending through one copy does not advance another; writes still alias.
  Tested by `fixed_by_value_aliasing_and_mutation`,
  `fixed_by_value_lifetime`, and `fixed_never_relocates_movable_does`.

`Push`/`Reserve` dispatch on `_kind` with an inlined
`_Static_assert(_Generic(_s->_kind, MovableKind: 1, default: 0), "expected a movable Slice")`:
`MovableKind` → movable, and `FixedKind` or an unknown tag fails the assertion.
`PushUnchecked`, `Pop`, and `Clone` remain structural.

Implementation split: the macros above exist where C needs a type name
(`sizeof(T)`, `__builtin_types_compatible_p`), a `_kind` `_Generic`
dispatch, a declaration initializer, or a return value. Only the capacity
growth itself is type-erased (`arena_slice_reserve`, `arena_slice_grow`,
`_arena_at_tip`, exposed as `SliceAtTip(arena, s)`); slot computation, typed
stores, and descriptor updates stay inline in the macros. Statement macros use
`do { } while (0)`; value-returning
macros (`Push`, `PushUnchecked`, `Pop`, 3-arg `SliceFromArray`) use a
statement expression, and `ArenaOOM` must keep `setjmp` in the caller's frame
as a controlling expression.

Evaluation: each operation macro binds its slice, value, and capacity
arguments once (`slice`/`value`/`min_cap` are evaluated once; the checks are
unevaluated). `slice.macro_args_evaluated_once` and
`slice.clone_evaluates_args_once` cover the entry points for the bound
arguments.

---

## 2. Class mapping to Goose

Goose reference sections: §1 (size classes/stack invariant), §3 (array family,
references, slices), §4 (values/copy/assignment), §5 (resize/shrink rules), §9
(lifetimes/safety).

| C class | API | Nearest Goose type | Assessment |
|---|---|---|---|
| 1. Fixed | `FixedSlice`, `SliceFixed(arena, &b, cap)`, `Push(&b, v)`, `Pop(&b, &out)` | `T[..]` / `T[..k]` | Fixed-capacity, no-growth store; metadata is `isize`, no `pop_n`/`resize`/`clear`. Capacity is a size limit, not the class marker. |
| 2. Movable | `SliceFromArray(arena, dst, array)`, `Clone`, ordinary `Slice` + 3-arg `Push`/`Reserve` | **No Goose equivalent** | Growth may relocate; aliases can dangle after `Push`/`Reserve`. Class 2 is a C arena-vector extension. |

The class boundary is the type and growth policy, not a bound check:
`FixedSlice` cannot grow at all, and ordinary `Slice` is movable.
Capacity only says how much room exists; at the arena tip, growth extends
in place instead of relocating.

### Operation guarantees by class

Columns: what the compiler rejects, what `Assert` enforces in release
("always"), what only debug builds check, and the gap to the nearest Goose
type. "Layout" = size/offsets/alignment vs `SliceInternal`.

#### Class 1 — Fixed (`FixedSlice`, tag `FixedKind`)

| Operation | Compile-time | Runtime (always) | Debug only | Gap vs `T[..]` / `T[..k]` |
|---|---|---|---|---|
| `SliceFixed(arena, &b, cap)` | layout + tag; element size/alignment/stride | `cap >= 0`; reserve | slice-state check | capacity is runtime, not compile-time `k` |
| `Push(&b, v)` | value type (hard error); layout (structural, no tag needed) | full → `-1`, no append; never relocates | slice-state check | no `append`, `index_of`; single-element push only; result is nodiscard |
| `Pop(&b, &out)` | output type; layout (structural, no tag needed) | empty → `false`, `*out` untouched | slice-state check | no `pop_n`/`resize`/`clear`; no shrink-liveness proof |
| `PushUnchecked(&b, v)` | value type; layout | — | spare-capacity `len < cap` | release overrun corrupts silently |

| `Push(arena, &b, v)` / `Reserve(arena, &b, n)` | **compile error** — inlined `_Static_assert` requires `MovableKind` | n/a | n/a | n/a (this *is* the enforcement) |

#### Class 2 — Movable (`Slice`, tag `MovableKind`)

Growth may relocate; prior `T*` and subslice descriptors go
stale, while indices and `At(s, i)` stay valid. **No Goose equivalent** — Goose resizables never move.

| Operation | Compile-time | Runtime (always) | Debug only | Gaps |
|---|---|---|---|---|
| `SliceFromArray(arena, dst, a)` | real, non-empty, constant-extent array; length/size fit `isize`; alignment/stride; dst must be movable `Slice` (inlined `_Static_assert` requires `MovableKind`); dst element type match | arena OOM policy; `cap == len` | — | source lifetime solved by copying, but arena lifetime remains; not an inline `T[k]` value |
| `Push(s, v)` | value type; layout (structural, no tag needed) | full → `-1`, no append; never allocates or relocates | slice-state check | no growth by design; use 3-arg `Push` to grow |
| `Push(arena, s, v)` | value type; layout; `MovableKind` required | full slice-state check on the grow path; OOM policy | slice-state check | stale aliases undetected (old block not freed, ASAN often blind) |
| `Reserve(arena, s, n)` | layout + tag via dispatch | state check + arg validation, even in release; may relocate | — (checked in release too) | same relocation hazard on capacity change alone |
| `Pop(s, &out)` | output type; layout (structural) | empty → `false` | slice-state check | popped-slot reuse undetected |
| `PushUnchecked(s, v)` | value type; layout | — | state + spare capacity | release overrun undetected |
| `Clone(arena, out, src[, start[, len]])` | layout; out-vs-src element type | source/range bounds; args evaluated once | — | shallow; no relocatable trait |

#### Dispatch across classes

| Input to 3-arg `Push`/`Reserve` | Result |
|---|---|
| `MovableKind` | movable path |
| `FixedKind` | **compile error** (`expected a movable Slice`) |
| Unknown hand-written tag | **compile error** (`expected a movable Slice`) |
| Tagless descriptor | **compile error** (`no member named '_kind'`) |

A swapped *known* tag on a hand-written descriptor still compiles silently —
the tag is the claim and C cannot second-guess a well-formed claim. Only
macro-generated descriptors are correct by construction.

Goose's own class column is fixed / variable / resizable, and the array forms
map as follows:

- `FixedSlice` ↔ limited arrays `T[..k]` and `T[..]`: capacity is chosen at
  construction, length is limited by it, and push/pop work in place up to
  capacity. C does not distinguish a compile-time `k` from a runtime capacity.
- Ordinary movable `Slice` ↔ **no Goose array class**. Its shape resembles
  `T[>..]` or `T[]`, but Goose resizables never move; relocation is a C
  extension and breaks Goose's reference-lifetime guarantee. At the arena
  tip, growth still extends in place.
- `SliceFromArray` / `Clone` ↔ owning copies of `T[k]` / `T[]` values, but the
  result is an arena-backed descriptor that can later become movable, not an
  inline array value.
- Goose slices `T[:]` / `T[:k]` have no C equivalent now that the zero-copy
  view constructor is gone; `Slice` descriptors can be made structurally, but
  that is unsupported and unchecked.

### §1.1 / §1.3 — size classes and stack invariant

- Partial: the C design models variable/resizable operationally. Data storage
  is always arena-backed, and a C array is reached through an owning
  `SliceFromArray` copy. Descriptors follow the pass/store rubric: movable by
  pointer, fixed by value.
- Movable descriptors are arena-owned values: allocate with `New` and pass
  the pointer. Fixed descriptors are values owned by their scope/record; pass
  them by value and take `&b` at the operation. Fixed storage never relocates.
- Divergence: Goose has no movable resizable array. `arena_slice_reserve` can
  relocate ordinary slices; Goose `[>..]` growth is a bump and `[..]`/`[..k]`
  never move.
- Divergence: Goose’s “one resizable per stack, always topmost” is static;
  C checks nothing of the sort — off-tip growth relocates.
- `_kind` is a C macro-dispatch mechanism, not a Goose concept.

### §3.3 — array family

- Partial: fixed push, fixed elements, and empty handling are
  represented.
- Divergence: Goose `[..]` and `[>..]` are distinct static types. Here only
  `FixedSlice` (fixed, no arena growth) remains; the growable side is the
  movable `Slice`, and the three-argument `Push`/`Reserve` on a fixed
  descriptor is a compile error.
- Divergence: metadata is `isize`; Goose supports `u32` default and
  `u8`–`u64`/`varint` length fields.
- Missing: variable-size elements, sequential arrays, `pop_n`,
  `resize`, `clear`, stale-reference type-safe reuse.

### §3.9 / §3.10 — references and slices

- Missing: roots, scoped slice lifetimes, static escape prevention.
- Missing: `T[:]`/`T[:k]` views over arrays; the zero-copy view constructor was
  removed, and `Slice` descriptors are not rooted slices with lifetime checks.
- Missing: relative references, pools, index/bound types, bounds-checked
  indexing.
- `Clone` has no relocatable-type trait and cannot reject
  self-relative values; Goose forbids copying those.
- `SliceFromArray(arena, dst, array)` copies eagerly, so the source array lifetime
  is not a hazard; the copy still inherits arena lifetime.

### §4 — values, copying, assignment

- `Clone` is an explicit element copy; cross-class assignment is
  rejected; good.
- Descriptors are ordinary C values now: `ints b = *a` and `*b = *a` compile
  and alias the backing block. Pass pointers and treat a descriptor copy as a
  deliberate alias, not an independent slice.
- `Clone(arena, out, src)` writes through a destination pointer. A descriptor
  can be returned by value, but the result aliases the same backing block;
  prefer pointers.
- A whole `=` assignment is not a deep copy; `Push`, `Pop`, and `Clone`
  are the mutation paths.

### §5.1 — grow-only `[>..]`

- At-tip movable push/reserve extends in place; off-tip growth relocates.
  There is no `[>..]`-style no-move guarantee anymore.
- Missing: `pop_n`, `clear`, `resize` downward, shrink liveness, live
  reference proving, balanced-call reasoning. Single-element `Pop` is
  present and empty-safe (false/out unchanged), a deliberate divergence
  from Goose's abort.
- The runtime `Assert` replaces a Goose compile-time obligation.

### §5.2 / §5.3 / §5.4

- §5.2: no grow-shrink `[>..<]` class.
- §5.3: single-element `Pop` and slot reuse are present; no
  `pop_n`/`resize`/`clear`, and no stale-reference reuse tests.
- §5.4: no `reusable` arrays or slice pools.

### §9 — references, lifetimes, safety

- §9.1/§9.2: **not implemented**. No roots, store rule, branch/return root
  checks, temporary lifetime enforcement, or escape checks.
- §9.3: runtime aborts exist for arena misuse and empty fixed push; reasonable
  C safety net. Fixed `Push` reports `-1` instead of aborting.
- §9.4: no type-safe reuse model; ASan poisoning is not Goose type safety.
- §9.5: partial C `const` handling (`const` array → mutable slice rejected;
  adding `const` allowed); no deep writability provenance.
- Movable relocation can leave aliases pointing at old arena storage; C does
  not detect this. Goose has no moving array to model.

### §10.4

- The Goose 48-bit limit is intentionally absent; C uses `isize`/`size_t`.
- `SliceInternal` is a C ABI/descriptor type, not a Goose array layout.

---

## 3. Compile-time coverage

Harness: `make typecheck` / `test/typecheck.sh`. Pass fixtures must compile
with `-Werror`; fail fixtures must fail for the expected reason.

Current result: **14 compiled, 26 rejected**.

### Pass fixtures (14)

- `slice_classes_legal` — both classes and legal conversion paths.
- `fixed_slice` — fixed layout, `SliceFixed`, fixed `Push`,
  `PushUnchecked`, `Clone`.
- `const_fixed_copy` — const-element fixed → const-element Slice via `Clone`.
- `from_array_typed`, `from_array_copy`, `from_array_alignment` — real/const/
  compound-literal/over-aligned arrays copied by `SliceFromArray`.
- `data_lvalue_and_layout`, `duplicate_expansions`, `multi_token_types` —
  typed `s->data[i]` access, layout identity, multi-expansion coexistence.
- `struct_fields` — a `New`-allocated record holding movable slices inline
  and by pointer, plus a fixed descriptor stored by value.
- `push_typed`, `push_unchecked_typed`, `pop_typed` — value/output type checks.
- `push_no_grow` — 2-argument `Push` on movable and fixed descriptors; it
  compiles for both and reports -1 when full.

### Fail fixtures, grouped (26)

- `SliceFromArray` misuse (9): pointer, pointer-to-array, VLA, zero-length,
  element type mismatch, const mismatch, typed-destination mismatch,
  fixed-destination value, and a const array copied through the typed
  destination.
- Value-type misuse (7): wrong scalar, wrong struct, unchecked wrong scalar,
  push into non-slice, fixed Push wrong scalar, const
  fixed Push, const movable Push.
- By-value descriptor (1): passing `Slice(T)` itself instead of `Slice(T)*`.
- Pop misuse (2): wrong output type, const output destination.
- Slice `data` pointer type (1).
- Class/handle misuse (6): fixed→Slice and Slice→fixed assignment,
  growable `Push` on a fixed value, `Reserve` on a fixed value, unknown tag on
  3-arg `Push`, and unknown tag on `Reserve`.

Element-type guards for `Push`/`PushUnchecked` are hard `_Static_assert`
checks: they compare the value type with the slice element type through
`__builtin_types_compatible_p`, so a wrong scalar or struct is an error even
without `-Werror`. Top-level qualifiers are ignored by that builtin; pushing
an `int` into a `const int` slice consequently passes the type assert but is
still rejected by the typed store (`*_p = _v`) that follows. The same applies
to the typed `SliceFromArray` form. Layout/array checks are likewise
`_Static_assert` errors.

---

## 4. Runtime coverage

Current result: **142 tests passed**, 0 build warnings.

Slice tests live in `src/slice_tests.c` (`slice.*`), with `cspan` fixtures
using arena-owned descriptors, plus 3 in `main.c`; the rest cover `astr`,
ADTs/demo.

### Classes and conversions

- `SliceFromArray`: arena copy owns its storage, survives source scope, typed
  destination form, `Pop`-able.
- Fixed→movable via `Clone`; movable→fixed only by rebuilding/copying.
- Movable relocates off-tip (old pointer changes) and extends in place
  at the tip (data unchanged), including in-capacity off-tip work.

### Empty edge cases

- `New`-zeroed descriptor, empty `Clone`, first `Push` allocates.
- Empty fixed at the tip and off-tip: allocate at `cur`, stay aligned.

### Core slice behavior

- Clone: full/subrange/empty/argument evaluation (including once-only
  evaluation of arena, destination, source, and start).
- Entry-point argument evaluation: `Push`, `Reserve`, `Pop`, `SliceFixed`, and
  typed `SliceFromArray` evaluate each runtime argument exactly once.
- Push: 2-arg no-growth reaches full → -1 and preserves data pointers even off-tip;
  3-arg growth relocates when needed; structs and fill survival.
- PushUnchecked: reserved fill, shared state, partial top-up.
- Pop: last element, cap preservation, slot reuse, struct values, Push/Pop
  round-trips, Pop-then-Push no-op (incl. after relocation), empty-safe false
  with `*out` untouched.
- Reserve: grow, preserve, grow-only, at-tip extension.
- Layout/type: multi-token, duplicate expansions, typed access, zero overhead.
- Arena ownership: `New`-zeroed descriptor, descriptor passed between
  functions, inline and pointer struct fields, stable descriptor address
  across backing growth.
- Pass/store rubric: fixed descriptors returned/stored/passed by value with
  shared data and independent `len`/`cap`; movable descriptors by pointer,
  with relocation visible through pointer aliases but not value copies.
- Lifetime/relocation: fixed descriptors outlive the building function and
  survive unrelated allocations/reset as values, while their arena data does
  not; movable descriptors and data survive nested `Scratch`; fixed data
  never relocates, movable off-tip growth does.
- `SliceAtTip`: empty slices report false; movable/fixed blocks report true
  at the tip; off-tip reserve relocates and returns to the tip, at-tip reserve
  extends in place.
- Overlap/scratch: shared backing, nested `Scratch` survival.

### Core arena behavior

- init/alloc/zero/no-init/copy-init/alignment; bump/reset/scratch/nesting;
  OOM NULL/longjmp/handler clearing; malloc/tip-only free; fill-to-capacity.

---

## 5. Stale references and known gaps

### Stale-reference enforcement matrix

A reference here means a raw `T*` into a slice's backing block, a `Push`
return, a subslice descriptor, or another descriptor copy that shares the
same storage. The library has no ownership, borrow, or liveness tracking.
Unless the Enforcement column names a compile-time check or `Assert`, a stale
reference can be created and used without a diagnostic.

| Operation | What can become stale | Enforcement |
|---|---|---|
| `SliceFromArray(arena, dst, a)` | The source array lifetime is no longer a hazard because the elements are copied. The new backing is arena storage, so arena reset/Scratch still invalidates it. | Compile-time array/type checks. Not enforced: arena lifetime. |
| `Push(arena, s, v)` on a movable `Slice` | Off-tip growth may `memmove` the elements. Every prior `T*` and subslice descriptor into the old block is stale; indices and `At(s, i)` stay valid. At-tip growth extends in place. | Compile-time value-type check; debug-only slice-state check. Not enforced: stale aliases. The old bump block is not freed, so ASAN often still sees it as live. |
| `Reserve(arena, s, n)` on a movable `Slice` | Same relocation hazard, even when only capacity changes. | Layout and state checks. Not enforced: stale aliases. |
| `Push(s, v)` (2-arg) | No move, so element pointers stay valid for either class. | Structural descriptor/type check (no `_kind` check); full → `-1`, no append, nodiscard. Not enforced: arena lifetime. |
| `Push(arena, &b, v)` / `Reserve(arena, &b, n)` | Fixed class: no growth path exists. Inlined `_Static_assert` requires `MovableKind`. | Compile error (`expected a movable Slice`). |
| `PushUnchecked(s, v)` | Does not move, but release builds have no capacity check; an overrun can corrupt neighboring elements and other slices or aliases. | Debug-only spare-capacity `Assert`. Not enforced in release: bounds, overlap, or corruption. |
| `Pop(s, &out)` / `Pop(&b, &out)` | No move; references to remaining elements stay valid. A reference to the popped slot is logically stale: the memory is not cleared, and the next `Push` may overwrite it. | Empty returns false without touching `out`. Not enforced: use of a pointer to the popped slot. |
| `SliceFixed(arena, &b, n)` on an already-initialized fixed value | Overwrites `b` and orphans its previous backing; old pointers and subslices into that backing become stale. | Capacity/alignment and element-stride checks. Not enforced: old data aliases. |
| `Clone(arena, out, s)` | The source is not moved, but the clone is shallow: element-held pointers, slices, or self-relative references still refer to the source storage/lifetime. It also overwrites `out`, orphaning its previous backing. | Bounds/layout checks. Not enforced: relocatable/self-relative element types or arena lifetime. |
| Explicit descriptor copies (`*dst = *src`, `memcpy`, struct copy) | Movable: a value copy pins the old block; a later growth through the original leaves the copy stale. Fixed: copies are valid views because the backing never relocates, but `len`/`cap` diverge. | None: descriptors are ordinary values, so copies compile and alias silently. Pass movable slices by pointer; fixed values may be copied. |
| Hand-written descriptor with `data` over caller memory | No view constructor exists, but a structural `{data, len, cap}` literal still compiles for the structural ops (`Pop`/`PushUnchecked`/`Clone`). 2-arg `Push` is structural and accepts any layout-matching descriptor. | Layout/type checks only. Not enforced: external-memory ownership, lifetime, or a later `Reserve` move. |
| `Scratch` exit / `arena_reset` / `arena_release` / `arena_free` | All slice storage in the affected arena range becomes stale; bump memory may be reused by later allocations. | No slice-level check. ASAN may catch some freed memory, but not ordinary bump reuse. |
| `Pop` followed by `Push` | The popped slot can be reused; an old pointer to it observes the new value or a later overwrite. | None; this is a logical lifetime rule. |

`Clone` can also advance the arena tip. That does not invalidate
source pointers (off-tip growth relocates instead of asserting).

What is actually enforced elsewhere:

- compile time: operation form/class, `Push` value type, `Pop` output type,
  slice/array layout, and `SliceFromArray` array validity;
- runtime: fixed capacity (`-1`, nodiscard), movable
  relocation, empty-safe `Pop`, and debug slice-state/spare-capacity checks.

None of these checks proves that a data pointer is still live. There is no
descriptor-identity guard: plain descriptor copies alias silently.
Store indices or offsets across growth, reacquire `data` after a movable
`Push`/`Reserve`, do not keep pointers to popped slots, and treat
`Scratch`/reset as freeing every slice in its range. ASAN/UBSAN are diagnostic
aids, not part of the API; sanitizer coverage for stack lifetime depends on the
flags, and old bump blocks are not freed.

### Known gaps

These are the areas a reviewer should not assume are covered.

1. **Fixed and movable share one fixed-capacity entry point.**
   `FixedSlice` initializes through the fixed-capacity macro
   (`SliceFixed`). Goose `T[..k]` also fixes `k` at compile
   time; C takes all capacities at runtime.

2. **Movable is C-only.**
   Ordinary Slice growth can move. Goose has no movable resizable array;
   references/pointers into movable slices can dangle after `Push`/`Reserve`.
   Indices and `At(s, i)` stay valid across moves.

3. **Partial shrink API.**
   `Pop(s, &out)` and `Pop(&b, &out)` cover single-element shrink,
   but `pop_n`, `resize` downward, and `clear` are absent. Pop also cannot
   enforce Goose's liveness rules: references into the slice that outlive the
   shrink remain the caller's responsibility. Empty Pop returns false and
   leaves `out` untouched rather than aborting, diverging from Goose §5's
   abort rule.

4. **No static roots/lifetimes.**
   `Scratch` slices can escape. C has no equivalent of §9.2. `SliceFromArray` removes the
   source-array lifetime issue by copying, but still inherits arena lifetime.
   A hand-written descriptor whose `data` points at caller memory is
   structurally valid and undetected. See the stale-reference matrix above
   for the per-operation breakdown.

5. **No relocatable/self-relative trait.**
   `Clone` can copy element types containing self-relative
   offsets; Goose §3.9 would reject those copies. A typed trait or separate
   clone APIs would be needed.

6. **`_kind` is internal.**
   It overlays `data`, is never read at runtime, and exists only for
   `_Generic` dispatch. Do not read/write it. Hand-written descriptors that
   want the three-argument `Push`/`Reserve` must add a matching tag.

7. **Two-argument `Push` is the no-growth form for both classes.**
   `Push(s, v)` is structural: it accepts a movable `Slice`, a `FixedSlice`
   value, or any layout-matching descriptor, never allocates or relocates, and
   returns -1 when full. The
   3-argument form is the only growth path and requires `MovableKind`.
   `PushUnchecked` remains the unchecked fast path.

9. **Metadata and limits.**
   `isize` is not Goose’s `u32`/`u8`–`u64`/`varint` metadata model. The Goose
   48-bit limit is intentionally removed.

10. **Pointer comparison in tests.**
    `empty_off_tip_can_grow` uses `>= cur` plus alignment checks rather than
    equality, because `arena_alloc` may align the first allocation.

---

## 6. Check surface: compile-time vs runtime

Every slice operation is checked structurally, but the checks split into hard
compile-time diagnostics and runtime `Assert`s. "Always" below means the check
is present in release too (`Assert`); "debug" means `NDEBUG` removes it.

### Compile-time checks (hard errors, even without `-Werror`)

| Check | Mechanism | Applies to |
|---|---|---|
| `Slice(T)` size, `data`/`len`/`cap` offsets, and alignment match `SliceInternal` | `_Static_assert` in `_SliceAssertStatic` | every typed Slice operation |
| `FixedSlice` descriptor footprint and alignment | `_SliceAssertStatic` inlined at each fixed entry point | `Push(&b, v)`, `SliceFixed` |
| Array is a real, non-empty, constant-extent C array; element size nonzero; length/size fit `isize`; power-of-two alignment; stride multiple | `_Static_assert` in `_SliceAssertArray` | `SliceFromArray` |
| Value type matches the slice element type | `__builtin_types_compatible_p` in `_SliceAssertValueType` | all `Push` forms, `PushUnchecked`, `Pop` output |
| Element destination is writable / pointer assignment is compatible | typed store/assignment in the macro body (`_s->data[_idx] = _v`, `*_o = ...`) | `Push`, `PushUnchecked`, `Pop`, typed `SliceFromArray` |
| Destination element type matches the copied array | `__builtin_types_compatible_p` in `SliceFromArray` | `SliceFromArray(arena, dst, array)` |
| Destination is a movable `Slice` | inlined `_Static_assert(_Generic(...))` in `SliceFromArray` | `SliceFromArray(arena, dst, array)` |
| Clone destination element type matches source | `__builtin_types_compatible_p` in `_CloneDst`/`_CloneToEnd` | `Clone` |
| Element size, alignment, and stride valid | `_Static_assert` in `SliceFixed` | fixed value initialization |
| Class dispatch: `MovableKind` required | inlined `_Static_assert(_Generic(...))` | wrong-class descriptors (incl. unknown hand-written tags) are compile errors on 3-arg `Push`/`Reserve` and `SliceFromArray` |
| Operation argument is a pointer to the descriptor | member access through the macro body (`_s->data`, `_s->_kind`) | `Push`, `Pop`, `Reserve`, `At`, `Clone`; passing the descriptor itself by value is rejected |
| Two-argument `Push` is structural and never grows | `_SliceAssertStatic` + `_SliceAssertValueType` | any layout-matching descriptor; no `_kind` requirement, no growth path |
| Three-argument `Push` requires a movable `Slice` | inlined `_Static_assert(_Generic(...))` | `FixedSlice` is a compile error |

### Runtime checks

| Check | Always or debug | Location |
|---|---|---|
| Slice state: `len >= 0`, `len <= cap`, `data` present unless empty, data aligned | always | `arena_slice_reserve` (all growth paths end there) |
| Slice state for the other operations | debug only | `_SliceAssertRuntimeDebug` in 3-arg `Push`, `PushUnchecked`, `Pop`, `Reserve` |
| Capacity `len < cap`, full → `-1`, no append | always | two-argument `Push(s, v)` for either class (nodiscard) |
| Reserve arguments (`min_cap >= 0`, element size positive, alignment power-of-two) and valid slice state | always | `arena_slice_reserve` (called by every `Reserve` path) |
| `PushUnchecked` spare capacity | debug only | `_SliceAssertSpareDebug` |
| `Clone` source and range (`data` non-null unless empty, `start`/`len` bounds) | always | `_CloneDst` |
| Fixed capacity non-negative | always | `SliceFixed` |

### Intentionally unchecked

These stay in the stale-reference matrix above, because no compile-time or
runtime check proves them:

- liveness of a `data`/element pointer after a movable `Push`/`Reserve`;
- arena lifetime after `Scratch`, `arena_reset`, `arena_release`, or `arena_free`;
- reuse of a popped slot;
- explicit descriptor copies (`*dst = *src`, `memcpy`, struct copy);
- a hand-written descriptor whose `data` points at caller memory;
- copying a slice descriptor inside a struct by value.

ASAN/UBSAN are diagnostic aids, not part of the API contract.

### Copy policy for slice-holding records

Descriptors are ordinary values, so every copy path compiles: `b = a`,
`f(a)` by value, a struct assignment, or `memcpy` duplicates the descriptor.
Pass descriptors and any struct holding one by pointer, and treat a by-value
copy as an alias, not an independent slice:

```c
struct Holder { ints s; };
struct Holder* a = New(arena, struct Holder);
struct Holder b = *a;       // copies the descriptor
b = *a;                     // copies the descriptor
f(*a);                      // by-value pass/return also copies it
memcpy(&b, a, sizeof *a);   // raw byte copy
```

Any of these leaves two descriptors sharing one backing block. Growth
through one can relocate it and leave the other pointing at the old block,
with stale `len`/`cap`. Use `Clone` for an independent element copy,
and pass slice-holding structs by pointer. Reviewers should treat a value copy of a slice-holding record as a
bug, since neither the compiler nor the runtime will catch it.

---

## 7. Verification commands

```text
make debug
./build/cmd
make typecheck
CC='clang -std=c23' ./test/typecheck.sh
```

Last recorded results:

```text
runtime suite:                  142 tests passed
make typecheck:                 14 compiled, 26 rejected
CC=clang -std=c23 typecheck:    14 compiled, 26 rejected
CC=cosmocc -mcosmo typecheck:   14 compiled, 26 rejected
build warnings:                 0
```

---

## 8. Recommended follow-ups

1. Add `pop_n`/`resize`/`clear` and shrink-liveness tests if targeting
   Goose §5.1/§5.3.
2. Add a relocatable-type opt-in for `Clone`.
3. Consider a scoped/opaque API to approximate §9.2 roots rather than relying
   on convention.
