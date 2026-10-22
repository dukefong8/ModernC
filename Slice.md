# Arena Slice Design Review

Status: working review of the arena-backed Slice API against
`/Users/duke/dev/goose/docs/goose_spec.md`.

Scope reviewed:

- `include/arena.h` — Slice classes, ownership tags, Push/Reserve/Clone handling.
- `src/arena_tests.c` — runtime tests.
- `test/typecheck/{pass,fail}` — compile-time fixtures.

Not reviewed here:

- `include/cspan.h` and `src/cspan_tests.c` (committed separately).
- Unrelated application/demo code.

---

## 1. Current slice model

### `Slice(T)`

`Slice(T)` is a one-element array of a descriptor union, built with
`__typeof__` so the typedef stays usable as a normal type name:

```c
typedef struct { unsigned char _; } MovableKind;

#define Slice(T)                                                     \
  __typeof__((                                                       \
      (union {                                                       \
        struct { T *data; isize len; isize cap; };                   \
        MovableKind _kind;                                             \
      }[1]){}))

#define FixedSlice(T)                                              \
  __typeof__((                                                       \
      (struct { T *data; isize len; isize cap; }[1]){}))
```

The array wrapper is the pass-by-reference seam: a function parameter of slice
type decays to a pointer to the caller's descriptor, `t = s` and `ints t = s`
are compile errors, and `s->data[i]` is the typed element accessor. `_kind`
overlays `data` and is never read at run time; it lets `_Generic` select the
movable (`MovableKind`) or fixed (`FixedKind`).
Descriptor size, offsets, and alignment match `SliceInternal`; the array
itself has the same size as one descriptor (three words). Explicit raw
descriptor copies (`copy[0] = s[0]`) and struct copies alias silently, as the
array wrapper makes them deliberate actions.

### Operations

- `SliceFromArray(arena, dst, a)` — copy into
  arena storage (`cap == len`); source lifetime no longer matters.
- `SliceFixed(arena, b, cap)` / `Push(b, v)` — fixed class; no growth,
  `Push` returns the new index or -1 when full.
- `Push(arena, s, v)` / `Reserve(arena, s, n)` — movable class; may relocate.
- `At(s, i)` — bounds-checked element access, an lvalue valid across moves.
- `Clone(arena, out, src[, start[, len]])` — destination-only copy
  (arrays can't be returned); works on either slice class.
- `PushUnchecked(s, v)` — unchecked in-capacity write.
- `Pop(s, &out)` — last element into `*out`, false when empty (nodiscard).
- `SliceMoveAssign(dst, src)` — move descriptor, clear source.

`Push`/`Reserve` dispatch on `_kind` with only `MovableKind` named and no
`default`: `MovableKind` → movable, and `FixedKind`
matches nothing — a fixed slice in a 3-argument form is a constraint error
naming its kind. `PushUnchecked`, `Pop`, `Clone`, and `SliceMoveAssign`
remain structural.

Implementation split: the macros above exist where C needs a type name
(`sizeof(T)`, `__builtin_types_compatible_p`), a `_kind` `_Generic`
dispatch, a declaration initializer, or a return value. Only the capacity
growth itself is type-erased (`arena_slice_reserve`, `arena_slice_grow`,
`arena_at_tip`); slot computation, typed stores, and descriptor updates stay
inline in the macros. Statement macros use `do { } while (0)`; value-returning
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
| 1. Fixed | `FixedSlice`, `SliceFixed`, `Push(b, v)`, `Pop(b, &out)` | `T[..]` / `T[..k]` | Fixed-capacity, no-growth store; metadata is `isize`, no `pop_n`/`resize`/`clear`. Capacity is a size limit, not the class marker. |
| 2. Movable | `SliceFromArray(arena, array)`, `Clone`, ordinary `Slice` + 3-arg `Push`/`Reserve` | **No Goose equivalent** | Growth may relocate; aliases can dangle after `Push`/`Reserve`. Class 2 is a C arena-vector extension. |

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
| `SliceFixed(arena, b, cap)` | layout + tag; element size/alignment/stride | `cap >= 0`; reserve | slice-state check | capacity is runtime, not compile-time `k` |
| `Push(b, v)` | value type (hard error); layout + tag | full → `-1`, no append | — (no state check) | no `append`, `index_of`; single-element push only; result is nodiscard |
| `Pop(b, &out)` | output type; layout (structural, no tag needed) | empty → `false`, `*out` untouched | slice-state check | no `pop_n`/`resize`/`clear`; no shrink-liveness proof |
| `PushUnchecked(b, v)` | value type; layout | — | spare-capacity `len < cap` | release overrun corrupts silently |

| `Push(arena, b, v)` / `Reserve(arena, b, n)` | **compile error** — `FixedKind` matches no `_Generic` association | n/a | n/a | n/a (this *is* the enforcement) |

#### Class 2 — Movable (`Slice`, tag `MovableKind`)

Growth may relocate; prior `T*` and subslice descriptors go
stale, while indices and `At(s, i)` stay valid. **No Goose equivalent** — Goose resizables never move.

| Operation | Compile-time | Runtime (always) | Debug only | Gaps |
|---|---|---|---|---|
| `SliceFromArray(arena, dst, a)` | real, non-empty, constant-extent array; length/size fit `isize`; alignment/stride; dst must be movable `Slice` (`FixedKind` is a `_Generic` constraint error); dst element type match | arena OOM policy; `cap == len` | — | source lifetime solved by copying, but arena lifetime remains; not an inline `T[k]` value |
| `Push(arena, s, v)` | value type; layout + tag via dispatch | full slice-state check on the grow path; OOM policy | slice-state check | stale aliases undetected (old block not freed, ASAN often blind) |
| `Reserve(arena, s, n)` | layout + tag via dispatch | state check + arg validation, even in release; may relocate | — (checked in release too) | same relocation hazard on capacity change alone |
| `Pop(s, &out)` | output type; layout (structural) | empty → `false` | slice-state check | popped-slot reuse undetected |
| `PushUnchecked(s, v)` | value type; layout | — | state + spare capacity | release overrun undetected |
| `Clone(arena, out, src[, start[, len]])` | layout; out-vs-src element type | source/range bounds; args evaluated once | — | shallow; no relocatable trait |
| `SliceMoveAssign(dst, src)` | layout | `dst != src`; src cleared | — | raw copies (`copy[0] = s[0]`, `memcpy`, struct copy) alias silently |

#### Dispatch across classes

| Input to 3-arg `Push`/`Reserve` | Result |
|---|---|
| `MovableKind` | movable path |
| `FixedKind` | **compile error** naming the kind |
| Unknown hand-written tag | **compile error** (`not compatible with any generic association type`) |
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

- Partial: the C design models variable/resizable operationally. The
  stack-fixed `T[k]` view class has been removed: every slice is arena-backed,
  and a C array is reached through an owning `SliceFromArray` copy.
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
- `SliceFromArray(arena, array)` copies eagerly, so the source array lifetime
  is not a hazard; the copy still inherits arena lifetime.

### §4 — values, copying, assignment

- `Clone` is an explicit copy; cross-class assignment is
  rejected; good.
- `ints t = s` and `t = s` are compile errors (array semantics), so
  pass-by-value copies the pointer/decays rather than the descriptor.
  Explicit raw copies (`copy[0] = s[0]`) and struct copies alias silently;
  `SliceMoveAssign` is the explicit transfer. Goose non-fixed arrays are
  values; assignment semantics differ.
- `Clone(arena, out, src)` writes into a named destination slice; arrays
  cannot be returned, so there is no expression form.
- A whole `= ` assignment of a limited array is not modeled; `Push`, `Pop`,
  and `Clone`/`SliceMoveAssign` are the mutation paths.

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

Current result: **12 compiled, 27 rejected**.

### Pass fixtures (12)

- `slice_classes_legal` — both classes and legal conversion paths.
- `fixed_slice` — fixed layout, `SliceFixed`, fixed `Push`,
  `PushUnchecked`, `Clone`.
- `const_fixed_copy` — const-element fixed → const-element Slice via `Clone`.
- `from_array_typed`, `from_array_copy`, `from_array_alignment` — real/const/
  compound-literal/over-aligned arrays copied by `SliceFromArray`.
- `data_lvalue_and_layout`, `duplicate_expansions`, `multi_token_types` —
  typed `s->data[i]` access, layout identity, multi-expansion coexistence.
- `push_typed`, `push_unchecked_typed`, `pop_typed` — value/output type checks.

### Fail fixtures, grouped (27)

- `SliceFromArray` misuse (9): pointer, pointer-to-array, VLA, zero-length,
  element type mismatch, const mismatch, typed-destination mismatch,
  fixed-destination handle, and a
  const array copied through the typed destination.
- Value-type misuse (7): wrong scalar, wrong struct, unchecked wrong scalar,
  push into non-slice, fixed Push wrong scalar, const
  fixed Push, const movable Push.
- Pop misuse (2): wrong output type, const output destination.
- Slice `data` pointer type (1).
- Class/handle misuse (8): fixed→Slice and Slice→fixed assignment,
  growable `Push` on a fixed handle, `Reserve` on a fixed handle,
  fixed `Push` on a movable slice,
  unknown tag on 3-arg `Push`, unknown tag on `Reserve`, and
  `SliceMoveAssign` across element types.

Element-type guards for `Push`/`PushUnchecked` are hard `_Static_assert`
checks: they compare the value type with the slice element type through
`__builtin_types_compatible_p`, so a wrong scalar or struct is an error even
without `-Werror`. Top-level qualifiers are ignored by that builtin; pushing
an `int` into a `const int` slice consequently passes the type assert but is
still rejected by the typed store (`*_p = _v`) that follows. The same applies
to the typed pointer assignments in `SliceMoveAssign` (`dst->data =
src->data`) and the typed `SliceFromArray` form. Layout/array checks are
likewise `_Static_assert` errors.

---

## 4. Runtime coverage

Current result: **135 tests passed**, 0 build warnings.

Slice tests live in `src/arena_tests.c` (`slice.*`) plus 3 in `main.c`;
the rest cover `astr`, `cspan`, ADTs/demo.

### Classes and conversions

- `SliceFromArray`: arena copy owns its storage, survives source scope, typed
  destination form, `Pop`-able.
- Fixed→movable via `Clone`; movable→fixed only by rebuilding/copying.
- Movable relocates off-tip (old pointer changes) and extends in place
  at the tip (data unchanged), including in-capacity off-tip work.

### Empty edge cases

- Empty `{}` descriptor, empty `Clone`, first `Push` allocates.
- Empty fixed at the tip and off-tip: allocate at `cur`, stay aligned.

### Core slice behavior

- Clone: full/subrange/empty/argument evaluation (including once-only
  evaluation of arena, destination, source, and start).
- Entry-point argument evaluation: `Push`, `Reserve`, `Pop`, `SliceMoveAssign`,
  `SliceFixed`, and typed `SliceFromArray` evaluate each runtime
  argument exactly once.
- Push: growth, structs, in-place elements, fill survival.
- PushUnchecked: reserved fill, shared state, partial top-up.
- Pop: last element, cap preservation, slot reuse, struct values, Push/Pop
  round-trips, Pop-then-Push no-op (incl. after relocation), empty-safe false
  with `*out` untouched.
- Reserve: grow, preserve, grow-only, at-tip extension.
- Layout/type: multi-token, duplicate expansions, typed access, zero overhead.
- `SliceMoveAssign` moves a descriptor and clears the source.
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
| `Push(b, v)` fixed | No move, so element pointers stay valid. | Compile-time `FixedKind` tag check; full → `-1`, nodiscard. Not enforced: arena lifetime. |
| `Push(arena, b, v)` / `Reserve(arena, b, n)` | Fixed class: no growth path exists. `FixedKind` matches no `_Generic` association. | Compile error naming `FixedKind` as incompatible. |
| `PushUnchecked(s, v)` | Does not move, but release builds have no capacity check; an overrun can corrupt neighboring elements and other slices or aliases. | Debug-only spare-capacity `Assert`. Not enforced in release: bounds, overlap, or corruption. |
| `Pop(s, &out)` / `Pop(b, &out)` | No move; references to remaining elements stay valid. A reference to the popped slot is logically stale: the memory is not cleared, and the next `Push` may overwrite it. | Empty returns false without touching `out`. Not enforced: use of a pointer to the popped slot. |
| `SliceFixed(arena, b, n)` on an already-initialized handle | Overwrites `b` and orphans its previous backing; old pointers and subslices into that backing become stale. | Capacity/alignment and element-stride checks. Not enforced: old data aliases. |
| `Clone(arena, out, s)` | The source is not moved, but the clone is shallow: element-held pointers, slices, or self-relative references still refer to the source storage/lifetime. It also overwrites `out`, orphaning its previous backing. | Bounds/layout checks. Not enforced: relocatable/self-relative element types or arena lifetime. |
| Explicit descriptor copies (`copy[0] = s[0]`, `memcpy`, struct copy) | Two descriptors share one backing. If one movable copy grows or is re-initialized, the other still points at the old block; `len`/`cap` also diverge. Fixed storage does not move, but the descriptors drift apart. | Compile-time array semantics reject `t = s` and `ints t = s`; explicit raw copies are undetected. `SliceMoveAssign` is the explicit transfer. |
| Hand-written descriptor with `data` over caller memory | No view constructor exists, but a structural `{data, len, cap}` literal still compiles for the structural ops (`Pop`/`PushUnchecked`/`Clone`). 2-arg `Push` and the class-checked initializers now require the matching `_kind` tag. | Layout/type checks only. Not enforced: external-memory ownership, lifetime, or a later `Reserve` move. |
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
descriptor-identity guard: explicit copies alias silently.
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
   `Pop(s, &out)` and `Pop(b, &out)` cover single-element shrink,
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

7. **Two-argument `Push` is fixed-class-only; `PushUnchecked` stays structural.**
   `Push(b, v)` requires a `FixedSlice` handle (verified by tag). Only
   `PushUnchecked`, `Pop`, `Clone`, and `SliceMoveAssign` accept any
   layout-matching descriptor; the 3-argument forms dispatch on class.

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
| `FixedSlice` descriptor footprint and alignment | `_Static_assert` in `_FixedSliceAssertStatic` | every fixed operation |
| Array is a real, non-empty, constant-extent C array; element size nonzero; length/size fit `isize`; power-of-two alignment; stride multiple | `_SliceStaticAssert` in `_SliceAssertArray` | `SliceFromArray` |
| Value type matches the slice element type | `__builtin_types_compatible_p` in `_SliceAssertValueType` | all `Push` forms, `PushUnchecked`, `Pop` output |
| Element destination is writable / pointer assignment is compatible | typed store/assignment in the macro body (`_s->data[_idx] = _v`, `*_o = ...`, `dst->data = src->data`) | `Push`, `PushUnchecked`, `Pop`, `SliceMoveAssign`, typed `SliceFromArray` |
| Destination element type matches the copied array | `__builtin_types_compatible_p` in `SliceFromArray` | `SliceFromArray(arena, dst, array)` |
| Destination is a movable `Slice` | `_Generic` on `_kind` (`MovableKind` only, no `default`) in `SliceFromArray` | `SliceFromArray(arena, dst, array)` |
| Clone destination element type matches source | `__builtin_types_compatible_p` in `_CloneDst`/`_CloneToEnd` | `Clone` |
| Element size, alignment, and stride valid | `_Static_assert` in `SliceFixed` | fixed handle initialization |
| Class dispatch: `MovableKind` named, no `default` | `_Generic` with a single `MovableKind` association | wrong-class descriptors (incl. unknown hand-written tags) are compile errors on 3-arg `Push`/`Reserve` |
| Whole-descriptor copy/assign/return is not expressible | C array declarator semantics | `t = s`, `ints t = s`, returning a slice array |
| Two-argument `Push` requires `FixedSlice` | `_FixedSliceAssertStatic` layout + `FixedKind` tag check | any other class is a compile error |

### Runtime checks

| Check | Always or debug | Location |
|---|---|---|
| Move source/destination distinctness | always | `SliceMoveAssign` |
| Slice state: `len >= 0`, `len <= cap`, `data` present unless empty, data aligned | always | `arena_slice_reserve` (all growth paths end there) |
| Slice state for the other operations | debug only | `_SliceAssertRuntimeDebug` in 3-arg `Push`, `PushUnchecked`, `Pop`, `Reserve` |
| Fixed capacity `len < cap`, full → `-1` | always | two-argument `Push(b, v)` (nodiscard) |
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
- explicit raw descriptor copies (`copy[0] = s[0]`, `memcpy`, struct copy);
- a hand-written descriptor whose `data` points at caller memory;
- copying a slice array inside a struct by value.

ASAN/UBSAN are diagnostic aids, not part of the API contract.

### Copy policy for slice-holding records

The array wrapper blocks the common copy paths for a slice itself
(`t = s`, `ints t = s`, returning a slice by value, by-value slice
parameters) but C has no hook for an enclosing record. Pass any struct or
union containing a slice descriptor by pointer, never by value:

```c
struct Holder { ints s; };
struct Holder a = ...;
struct Holder b = a;        // copies the descriptor
b = a;                      // copies the descriptor
f(a);                       // by-value pass/return also copies it
memcpy(&b, &a, sizeof a);   // raw byte copy
```

Any of these leaves two descriptors sharing one backing block. Growth
through one can relocate it and leave the other pointing at the old block,
with stale `len`/`cap`. Use `Clone` for an independent element copy,
`SliceMoveAssign` for a descriptor move, and pass slice-holding structs by
pointer. Reviewers should treat a value copy of a slice-holding record as a
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
runtime suite:                  135 tests passed
make typecheck:                 12 compiled, 27 rejected
CC=clang -std=c23 typecheck:    12 compiled, 27 rejected
CC=cosmocc -mcosmo typecheck:   12 compiled, 27 rejected
build warnings:                 0
```

---

## 8. Recommended follow-ups

1. Add `pop_n`/`resize`/`clear` and shrink-liveness tests if targeting
   Goose §5.1/§5.3.
2. Add a relocatable-type opt-in for `Clone`.
3. Consider a scoped/opaque API to approximate §9.2 roots rather than relying
   on convention.
