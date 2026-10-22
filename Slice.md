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
typedef struct { unsigned char _; } TipKind;

#define Slice(T)                                                     \
  __typeof__((                                                       \
      (union {                                                       \
        struct { T *data; isize len; isize cap; };                   \
        MovableKind _kind;                                             \
      }[1]){}))

#define FixedSlice(T)                                              \
  __typeof__((                                                       \
      (struct { T *data; isize len; isize cap; }[1]){}))

#define TipSlice(T)                                                   \
  __typeof__((                                                       \
      (union {                                                       \
        struct { T *data; isize len; isize cap; };                   \
        TipKind _kind;                                               \
      }[1]){}))
```

The array wrapper is the pass-by-reference seam: a function parameter of slice
type decays to a pointer to the caller's descriptor, `t = s` and `ints t = s`
are compile errors, and `s->data[i]` is the typed element accessor. `_kind`
overlays `data` and is never read at run time; it lets `_Generic` select the
movable (`MovableKind`), at-tip (`TipKind`), or fixed (`FixedKind`).
Descriptor size, offsets, and alignment match `SliceInternal`; the array
itself has the same size as one descriptor (three words). Explicit raw
descriptor copies (`copy[0] = s[0]`) and struct copies alias silently, as the
array wrapper makes them deliberate actions.

### Operations

- `SliceFromArray(arena, a)` / `SliceFromArray(arena, dst, a)` — copy into
  arena storage (`cap == len`); source lifetime no longer matters.
- `SliceFixed(arena, b, cap)` / `Push(b, v)` — fixed class; no growth.
- `SliceTip(arena, t, cap)` / `Push(arena, t, v)` / `Reserve(arena, t, n)` —
  at-tip class; growth requires `SliceAtTip`, extends in place, aborts off-tip.
- `Push(arena, s, v)` / `Reserve(arena, s, n)` — movable class; may relocate.
- `SliceAtTip(arena, s)` — the at-tip precondition (block ends at `arena->cur`).
- `Clone(arena, out, src[, start[, len]])` — destination-only copy
  (arrays can't be returned); works on any slice class.
- `PushUnchecked(s, v)` — unchecked in-capacity write.
- `Pop(s, &out)` — last element into `*out`, false when empty.
- `SliceMoveAssign(dst, src)` — move descriptor, clear source.

`Push`/`Reserve` dispatch on `_kind` with all three tags named and no
`default`: `TipKind` → at-tip, `MovableKind` → movable, and `FixedKind`
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
unevaluated). The `arena` argument is passed through: on the tip-growth path
it is used twice (at-tip assert plus growth). `SliceAtTip` evaluates its
slice argument multiple times. `slice.macro_args_evaluated_once` and
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
| 2. Tip-resizable non-moving | `TipSlice`, `SliceTip`, `Push(arena, t, v)`, `Reserve(arena, t, n)` | `T[>..]` | References stay valid: growth requires `SliceAtTip`, extends in place, and aborts off-tip. Goose proves topmost statically, C asserts at runtime. |
| 3. Movable | `SliceFromArray(arena, array)`, `Clone`, ordinary `Slice` + 3-arg `Push`/`Reserve` | **No Goose equivalent** | Growth may relocate; aliases can dangle after `Push`/`Reserve`. Class 3 is a C arena-vector extension. |

The class boundary is the type and growth policy, not a bound check:
`FixedSlice` cannot grow at all, `TipSlice` grows only at the tip and keeps
references valid, and ordinary `Slice` is movable. Capacity only says how
much room exists, while `SliceAtTip` says whether room can be added without
moving.

### Operation guarantees by class

Columns: what the compiler rejects, what `Assert` enforces in release
("always"), what only debug builds check, and the gap to the nearest Goose
type. "Layout" = size/offsets/alignment vs `SliceInternal`.

#### Class 1 — Fixed (`FixedSlice`, tag `FixedKind`)

| Operation | Compile-time | Runtime (always) | Debug only | Gap vs `T[..]` / `T[..k]` |
|---|---|---|---|---|
| `SliceFixed(arena, b, cap)` | layout + tag; element size/alignment/stride | `cap >= 0`; at-tip reserve | slice-state check | capacity is runtime, not compile-time `k` |
| `Push(b, v)` | value type (hard error); layout + tag | `len < cap`, aborts | — (no state check) | no `append`, `index_of`; single-element push only |
| `Pop(b, &out)` | output type; layout (structural, no tag needed) | empty → `false`, `*out` untouched | slice-state check | no `pop_n`/`resize`/`clear`; no shrink-liveness proof |
| `PushUnchecked(b, v)` | value type; layout | — | spare-capacity `len < cap` | release overrun corrupts silently |

| `Push(arena, b, v)` / `Reserve(arena, b, n)` | **compile error** — `FixedKind` matches no `_Generic` association | n/a | n/a | n/a (this *is* the enforcement) |

#### Class 2 — Tip-resizable (`TipSlice`, tag `TipKind`)

| Operation | Compile-time | Runtime (always) | Debug only | Gap vs `T[>..]` |
|---|---|---|---|---|
| `SliceTip(arena, t, cap)` | layout + tag; element size/alignment/stride | `cap >= 0`; at-tip reserve | slice-state check | same static-vs-runtime capacity gap as class 1 |
| `Push(arena, t, v)` | value type; layout + tag via dispatch | in-capacity: plain write. Growth needs `SliceAtTip` + post-growth no-move assert; off-tip aborts | slice-state check | Goose proves topmost **statically**; C checks at growth time. Extra `cap` field Goose lacks |
| `Reserve(arena, t, n)` | layout + tag via dispatch | in-cap: no-op. Beyond cap needs `SliceAtTip` + no-move assert; off-tip aborts | slice-state check | same static-vs-runtime gap |
| `Pop(t, &out)` | output type; layout (structural) | empty → `false` | slice-state check | Goose shrinks `[>..]` only where no live reference exists; C never proves that |
| `PushUnchecked(t, v)` / `Clone(...)` out of `t` | value type / element compat; layout | clone bounds | spare (unchecked) / — | clone is shallow and movable: stability not preserved through copy |

#### Class 3 — Movable (`Slice`, tag `MovableKind`)

Growth may relocate; prior `T*`, `Push` returns, and subslice descriptors go
stale. **No Goose equivalent** — Goose resizables never move.

| Operation | Compile-time | Runtime (always) | Debug only | Gaps |
|---|---|---|---|---|
| `SliceFromArray(arena, a)` / `(arena, dst, a)` | real, non-empty, constant-extent array; length/size fit `isize`; alignment/stride; dst type match | arena OOM policy; `cap == len` | — | source lifetime solved by copying, but arena lifetime remains; not an inline `T[k]` value |
| `Push(arena, s, v)` | value type; layout + tag via dispatch | full slice-state check on the grow path; OOM policy | slice-state check | stale aliases undetected (old block not freed, ASAN often blind) |
| `Reserve(arena, s, n)` | layout + tag via dispatch | state check + arg validation, even in release; may relocate | — (checked in release too) | same relocation hazard on capacity change alone |
| `Pop(s, &out)` | output type; layout (structural) | empty → `false` | slice-state check | popped-slot reuse undetected |
| `PushUnchecked(s, v)` | value type; layout | — | state + spare capacity | release overrun undetected |
| `Clone(arena, out, src[, start[, len]])` | layout; out-vs-src element type | source/range bounds; args evaluated once | — | shallow; no relocatable trait |
| `SliceMoveAssign(dst, src)` | layout | `dst != src`; src cleared | — | raw copies (`copy[0] = s[0]`, `memcpy`, struct copy) alias silently |

#### Dispatch across classes

| Input to 3-arg `Push`/`Reserve` | Result |
|---|---|
| `TipKind` | at-tip path |
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
- `TipSlice` ↔ grow-only resizable `T[>..]`: growth never moves the element
  region and references stay valid. C replaces Goose's static "region tops its
  data stack" proof with the runtime `SliceAtTip` check and carries a `cap`
  field that Goose `[>..]` does not have.
- Ordinary movable `Slice` ↔ **no Goose array class**. Its shape resembles
  `T[>..]` or `T[]`, but Goose resizables never move; relocation is a C
  extension and breaks Goose's reference-lifetime guarantee.
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
- Divergence: Goose’s “one resizable per stack, always topmost” is static.
  TipSlice approximates it with `SliceAtTip` at run time.
- `_kind` is a C macro-dispatch mechanism, not a Goose concept.

### §3.3 — array family

- Partial: fixed push, tip growth, fixed elements, and empty handling are
  represented.
- Divergence: Goose `[..]` and `[>..]` are distinct static types. Here they are
  `FixedSlice` (fixed, no arena growth) and `TipSlice` (tip growth); the
  three-argument `Push`/`Reserve` on a fixed descriptor is a compile error.
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

- TipSlice push/reserve at tip is non-moving; good.
- Off-tip growth aborts rather than relocating; good approximation.
- Missing: `pop_n`, `clear`, `resize` downward, shrink liveness, live
  reference proving, balanced-call reasoning. Single-element `Pop` is
  present and empty-safe (false/out unchanged), a deliberate divergence
  from Goose's abort.
- TipSlice has no Goose-style shrink rule: `Pop` never proves that no live
  reference or slice points at the popped slot.
- The runtime `Assert` replaces a Goose compile-time obligation.

### §5.2 / §5.3 / §5.4

- §5.2: no grow-shrink `[>..<]` class.
- §5.3: single-element `Pop` and slot reuse are present; no
  `pop_n`/`resize`/`clear`, and no stale-reference reuse tests.
- §5.4: no `reusable` arrays or slice pools.

### §9 — references, lifetimes, safety

- §9.1/§9.2: **not implemented**. No roots, store rule, branch/return root
  checks, temporary lifetime enforcement, or escape checks.
- §9.3: runtime aborts exist for capacity/off-tip/empty fixed push; reasonable
  C safety net.
- §9.4: no type-safe reuse model; ASan poisoning is not Goose type safety.
- §9.5: partial C `const` handling (`const` array → mutable slice rejected;
  adding `const` allowed); no deep writability provenance.
- Class 3 relocation can leave aliases pointing at old arena storage; C does
  not detect this. Goose has no moving array to model.

### §10.4

- The Goose 48-bit limit is intentionally absent; C uses `isize`/`size_t`.
- `SliceInternal` is a C ABI/descriptor type, not a Goose array layout.

---

## 3. Compile-time coverage

Harness: `make typecheck` / `test/typecheck.sh`. Pass fixtures must compile
with `-Werror`; fail fixtures must fail for the expected reason.

Current result: **13 compiled, 28 rejected**.

### Pass fixtures (13)

- `slice_classes_legal` — all three classes and legal conversion paths.
- `fixed_slice` — fixed layout, `SliceFixed`, fixed `Push`,
  `PushUnchecked`, `Clone`.
- `tip_slice` — TipSlice layout, `SliceTip`, at-tip `Push`/`Reserve`, `Clone`.
- `const_fixed_copy` — const-element fixed → const-element Slice via `Clone`.
- `from_array_typed`, `from_array_copy`, `from_array_alignment` — real/const/
  compound-literal/over-aligned arrays copied by `SliceFromArray`.
- `data_lvalue_and_layout`, `duplicate_expansions`, `multi_token_types` —
  typed `s->data[i]` access, layout identity, multi-expansion coexistence.
- `push_typed`, `push_unchecked_typed`, `pop_typed` — value/output type checks.

### Fail fixtures, grouped (28)

- `SliceFromArray` misuse (8): pointer, pointer-to-array, VLA, zero-length,
  element type mismatch, const mismatch, typed-destination mismatch, and a
  const array copied through the typed destination.
- Value-type misuse (8): wrong scalar, wrong struct, unchecked wrong scalar,
  push into non-slice, tip wrong scalar, fixed Push wrong scalar, const
  fixed Push, const movable Push.
- Pop misuse (2): wrong output type, const output destination.
- Slice `data` pointer type (1).
- Class/handle misuse (9): fixed→Slice and Slice→fixed assignment,
  growable `Push` on a fixed handle, `Reserve` on a fixed handle,
  fixed `Push` on a movable slice, fixed `Push` on a TipSlice,
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

Current result: **142 tests passed**, 0 build warnings.

Slice tests live in `src/arena_tests.c` (`slice.*`) plus 3 in `main.c`;
the rest cover `astr`, `cspan`, ADTs/demo.

### Classes and conversions

- `SliceFromArray`: arena copy owns its storage, survives source scope, typed
  destination form, `Pop`-able.
- Class 1→3 via `Clone`; class 3→1 only by rebuilding/copying.
- Class 3 relocates off-tip (old pointer changes); class 2 extends in place
  (data and earlier pointers unchanged), including in-capacity off-tip work.
- Death tests (fork + trap): tip off-tip growth, fixed `Push` at cap 0.

### Empty and tip edge cases

- Empty `{}` descriptor, empty `Clone`, first `Push` allocates.
- Empty fixed/tip at the tip and off-tip: allocate at `cur`, stay aligned.

### Core slice behavior

- Clone: full/subrange/empty/argument evaluation (including once-only
  evaluation of arena, destination, source, and start).
- Entry-point argument evaluation: `Push`, `Reserve`, `Pop`, `SliceMoveAssign`,
  `SliceFixed`, `SliceTip`, and typed `SliceFromArray` evaluate each runtime
  argument exactly once.
- Push: growth, structs, in-place elements, fill survival.
- PushUnchecked: reserved fill, shared state, partial top-up.
- Pop: last element, cap preservation, slot reuse, struct values, Push/Pop
  round-trips, Pop-then-Push no-op (incl. after relocation), empty-safe false
  with `*out` untouched.
- Reserve: grow, preserve, grow-only, tip extension.
- Layout/type: multi-token, duplicate expansions, typed access, zero overhead.
- `SliceMoveAssign` moves a descriptor and clears the source.
- Overlap/scratch: shared backing, nested `Scratch` survival, tip re-extension.

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
| `SliceFromArray(arena, a)` / `SliceFromArray(arena, dst, a)` | The source array lifetime is no longer a hazard because the elements are copied. The new backing is arena storage, so arena reset/Scratch still invalidates it. | Compile-time array/type checks. Not enforced: arena lifetime. |
| `Push(arena, s, v)` on a movable `Slice` | Off-tip growth may `memmove` the elements. Every prior `T*`, `Push` return, and subslice descriptor into the old block is stale. At-tip growth extends in place. | Compile-time value-type check; debug-only slice-state check. Not enforced: stale aliases. The old bump block is not freed, so ASAN often still sees it as live. |
| `Reserve(arena, s, n)` on a movable `Slice` | Same relocation hazard, even when only capacity changes. | Layout and state checks. Not enforced: stale aliases. |
| `Push(arena, t, v)` / `Reserve(arena, t, n)` on a `TipSlice` | In-capacity work never moves. Growth requires `SliceAtTip`; it extends in place, so `data` and existing element pointers stay valid. Off-tip growth aborts. | Runtime `SliceAtTip` assert on the growth path, plus a post-growth no-move assert. Not enforced: a later movable `Push`/`Reserve` on a `Clone` still may relocate. |
| `Push(b, v)` fixed | No move, so element pointers stay valid. | Compile-time `FixedKind` tag check; `Assert(len < cap)` at runtime. Not enforced: arena lifetime. |
| `Push(arena, b, v)` / `Reserve(arena, b, n)` | Fixed class: no growth path exists. `FixedKind` matches no `_Generic` association. | Compile error naming `FixedKind` as incompatible. |
| `PushUnchecked(s, v)` | Does not move, but release builds have no capacity check; an overrun can corrupt neighboring elements and other slices or aliases. | Debug-only spare-capacity `Assert`. Not enforced in release: bounds, overlap, or corruption. |
| `Pop(s, &out)` / `Pop(b, &out)` | No move; references to remaining elements stay valid. A reference to the popped slot is logically stale: the memory is not cleared, and the next `Push` may overwrite it. | Empty returns false without touching `out`. Not enforced: use of a pointer to the popped slot. |
| `SliceFixed(arena, b, n)` on an already-initialized handle | Overwrites `b` and orphans its previous backing; old pointers and subslices into that backing become stale. | Capacity/alignment and element-stride checks. Not enforced: old data aliases. |
| `Clone(arena, out, s)` | The source is not moved, but the clone is shallow: element-held pointers, slices, or self-relative references still refer to the source storage/lifetime. It also overwrites `out`, orphaning its previous backing. | Bounds/layout checks. Not enforced: relocatable/self-relative element types or arena lifetime. |
| Explicit descriptor copies (`copy[0] = s[0]`, `memcpy`, struct copy) | Two descriptors share one backing. If one movable copy grows or is re-initialized, the other still points at the old block; `len`/`cap` also diverge. Fixed/Tip storage does not move, but the descriptors drift apart. | Compile-time array semantics reject `t = s` and `ints t = s`; explicit raw copies are undetected. `SliceMoveAssign` is the explicit transfer. |
| Hand-written descriptor with `data` over caller memory | No view constructor exists, but a structural `{data, len, cap}` literal still compiles for the structural ops (`Pop`/`PushUnchecked`/`Clone`). 2-arg `Push` and the class-checked initializers now require the matching `_kind` tag. | Layout/type checks only. Not enforced: external-memory ownership, lifetime, or a later `Reserve` move. |
| `Scratch` exit / `arena_reset` / `arena_release` / `arena_free` | All slice storage in the affected arena range becomes stale; bump memory may be reused by later allocations. | No slice-level check. ASAN may catch some freed memory, but not ordinary bump reuse. |
| `Pop` followed by `Push` | The popped slot can be reused; an old pointer to it observes the new value or a later overwrite. | None; this is a logical lifetime rule. |

`Clone` can also advance the arena tip. That does not invalidate
source pointers, but it can leave a `TipSlice` source off-tip, so later
`Push`/`Reserve` growth on it asserts.

What is actually enforced elsewhere:

- compile time: operation form/class, `Push` value type, `Pop` output type,
  slice/array layout, and `SliceFromArray` array validity;
- runtime: fixed capacity, `SliceAtTip` on at-tip growth, movable
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

1. **Class 1/2 share one fixed-capacity entry point.**
   `FixedSlice` and `TipSlice` are distinct types (fixed vs tip growth are
   compile-time classes), but both initialize through fixed-capacity macros
   (`SliceFixed`/`SliceTip`). Goose `T[..k]` also fixes `k` at compile
   time; C takes all capacities at runtime.

2. **Class 3 is C-only.**
   Ordinary Slice growth can move. Goose has no movable resizable array;
   references/pointers into class 3 can dangle after `Push`/`Reserve`.
   Document this as an extension or remove/split the class.

3. **Partial shrink API.**
   `Pop(s, &out)` and `Pop(b, &out)` cover single-element shrink,
   but `pop_n`, `resize` downward, and `clear` are absent. Pop also cannot
   enforce Goose's liveness rules: references into the slice that outlive the
   shrink remain the caller's responsibility. Empty Pop returns false and
   leaves `out` untouched rather than aborting, diverging from Goose §5's
   abort rule.

4. **No static roots/lifetimes.**
   `Scratch` slices can escape; at-tip stability is enforced only at growth
   time. C has no equivalent of §9.2. `SliceFromArray` removes the
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
| `FixedSlice`/`TipSlice` descriptor footprint and alignment | `_Static_assert` in `_FixedSliceAssertStatic`/`_TipSliceAssertStatic` | every fixed/tip operation |
| Array is a real, non-empty, constant-extent C array; element size nonzero; length/size fit `isize`; power-of-two alignment; stride multiple | `_SliceStaticAssert` in `_SliceAssertArray` | `SliceFromArray` |
| Value type matches the slice element type | `__builtin_types_compatible_p` in `_SliceAssertValueType` | all `Push` forms, `PushUnchecked`, `Pop` output |
| Element destination is writable / pointer assignment is compatible | typed store/assignment in the macro body (`*_p = _v`, `*_o = ...`, `dst->data = src->data`) | `Push`, `PushUnchecked`, `Pop`, `SliceMoveAssign`, typed `SliceFromArray` |
| Destination element type matches the copied array | `__builtin_types_compatible_p` in `_SliceFromArray3` | typed `SliceFromArray(arena, dst, array)` |
| Clone destination element type matches source | `__builtin_types_compatible_p` in `_CloneDst`/`_CloneToEnd` | `Clone` |
| Element size, alignment, and stride valid | `_Static_assert` in `SliceFixed`/`SliceTip` | fixed and tip handle initialization |
| Class dispatch: all three tags named, no `default` | `_Generic` with `TipKind`, `MovableKind`, `FixedKind` associations | wrong-class descriptors (incl. unknown hand-written tags) are compile errors on 3-arg `Push`/`Reserve` |
| Whole-descriptor copy/assign/return is not expressible | C array declarator semantics | `t = s`, `ints t = s`, returning a slice array |
| Two-argument `Push` requires `FixedSlice` | `_FixedSliceAssertStatic` layout + `FixedKind` tag check | any other class is a compile error |

### Runtime checks

| Check | Always or debug | Location |
|---|---|---|
| Move source/destination distinctness | always | `SliceMoveAssign` |
| Slice state: `len >= 0`, `len <= cap`, `data` present unless empty, data aligned | always | `arena_slice_reserve` (all growth paths end there) |
| Slice state for the other operations | debug only | `_SliceAssertRuntimeDebug` in 3-arg `Push`, `PushUnchecked`, `Pop`, `Reserve` |
| Fixed capacity `len < cap` | always | two-argument `Push(b, v)` |
| At-tip growth precondition (`SliceAtTip`) and no-move postcondition | always, when growth is needed | `_SliceGrowAtTip` / `_SliceReserveAtTip` |
| Reserve arguments (`min_cap >= 0`, element size positive, alignment power-of-two) and valid slice state | always | `arena_slice_reserve` (called by every `Reserve` path) |
| `PushUnchecked` spare capacity | debug only | `_SliceAssertSpareDebug` |
| `Clone` source and range (`data` non-null unless empty, `start`/`len` bounds) | always | `_CloneDst` |
| Fixed/tip capacity non-negative | always | `SliceFixed`, `SliceTip` |

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
runtime suite:                  142 tests passed
make typecheck:                 13 compiled, 28 rejected
CC=clang -std=c23 typecheck:    13 compiled, 28 rejected
CC=cosmocc -mcosmo typecheck:   13 compiled, 28 rejected
build warnings:                 0
```

---

## 8. Recommended follow-ups

1. Decide whether movable class 3 stays a supported C extension or goes in
   favor of `[>..]`-style no-move growth.
2. Add `pop_n`/`resize`/`clear` and shrink-liveness tests if targeting
   Goose §5.1/§5.3.
3. Add a relocatable-type opt-in for `Clone`.
4. Consider a scoped/opaque API to approximate §9.2 roots rather than relying
   on convention.
