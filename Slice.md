# Slice

Arena-backed typed slices (`include/arena.h`). Requires GNU C: statement
expressions, `__auto_type`, `__typeof__`, `_Static_assert`, `_Generic`
(also builds under `-std=c23`, `cosmocc -mcosmo`).

Notation below: `s` movable `Slice(T)*`, `&b` fixed `FixedSlice(T)*`,
`N`/`n` element counts (`isize`), "evaluated once" = the macro binds the
argument into a temporary; the type checks are unevaluated.

## 1. Types

### 1.1 Descriptor layout

```c
typedef struct SliceInternal { void *data; isize len; isize cap; } SliceInternal;

#define Slice(T)      union { struct { T *data; isize len; isize cap; }; MovableKind _kind; }
#define FixedSlice(T) union { struct { T *data; isize len; isize cap; }; FixedKind   _kind; }
```

- `Slice(T)` / `FixedSlice(T)` expand to distinct anonymous union types:
  always name them with a typedef; members, parameters, and fields all use
  the typedef. `New` zero-initializes a movable descriptor to empty
  (`data == NULL, len == 0, cap == 0`).
- `_kind` overlays `data`, is never read at runtime, and exists only for
  `_Generic` class dispatch. Do not read or write it. Hand-written
  descriptors that need `Push`/`Reserve`/`PushN` must carry a matching tag;
  a swapped *known* tag compiles silently (the tag is the claim).
- `_SliceAssertStatic` (every typed operation): `sizeof`, `data`/`len`/`cap`
  offsets, and alignment all equal `SliceInternal`.

### 1.2 Ownership and passing

| | Movable `Slice` | `FixedSlice` |
|---|---|---|
| Store / pass | by pointer (`ints *s`) | by value (`fints b`), `&b` at each op |
| Descriptor lifetime | arena (`New`) | owner scope / containing record |
| Data lifetime | arena (both classes) | arena |
| Growth | relocates off-tip, extends at tip | none; growth ops are compile errors |
| Value copy (`*dst = *src`, struct copy, `memcpy`) | pins the old block; goes stale on growth through the original | valid view (backing never moves); `len`/`cap` diverge per copy |
| Return by value | legal, aliases backing; callers must not grow an older copy | legal and safe |

Copy rule: a descriptor copy is an alias, never an independent slice. Use
`Clone` for an independent element copy; pass movable slices and any struct
holding one by pointer.

## 2. Operations

### 2.1 Growth policy

All growth ends in `arena_slice_reserve` (exact `min_cap`; no-op when
`cap >= min_cap`; at-tip extends in place, else fresh block + `memmove` of
`len` elements). One-step growth (`arena_slice_grow`, used by `Push`):
**+16 at the tip, else `cap + max(cap/2, 16)`**.

| Form | Class | Effect | Result |
|---|---|---|---|
| `SliceFixed(arena, &b, cap)` | fixed | `b = {NULL,0,0}`, then reserve `cap`; orphans prior backing | — (`do/while`) |
| `SliceFromArray(arena, s, a)` | movable only | copy real non-empty constant-extent C array `a` into fresh backing; `cap == len` | `*s` (descriptor, by value) |
| `TryPush(s)` | either; never grows | slot at `data[len++]` iff `len < cap` | `T*` slot, `NULL` when full |
| `TryPush(arena, s)` | movable only | slot from spare capacity if any; else extend in place iff at tip (empty always qualifies), else `NULL` when full off-tip | `T*` slot, `NULL` on failure |
| `Push(arena, s)` | movable only | grow as needed (may relocate) | `T*` slot, never `NULL` |
| `PushN(arena, s, n)` | movable only | `n > 0`; reserve `len + n` iff short; `len += n` at once | `T*` base of the `n` slots, never `NULL` |
| `Reserve(arena, s, n)` | movable only | `cap = max(cap, n)` (may relocate); counted fill: reserve once, then loop a stack-owned copy with unchecked 1-arg `TryPush` (below) | — (`do/while`) |
| `Pop(s, &out)` | either (structural) | `*out = data[--len]` iff non-empty | `bool`; `false` leaves `*out` untouched (nodiscard) |
| `At(s, i)` | either (structural) | trap unless `0 <= i < len` | `T&` lvalue, valid across growth |
| `Ix(s, ptr)` | either (structural) | trap unless `ptr` addresses an element within `len` | `isize` index; `At(s, Ix(s, p)) == *p` |
| `Clone(arena, out, src[, start[, len]])` | either (structural) | shallow copy of `src[start:start+len]` (default full range) into fresh backing; replaces `out` backing (`NULL` when empty) | — (`do/while`) |
| `SliceAtTip(arena, s)` | either | `true` iff immediate growth would not move (`cap == 0`, or backing ends at `arena->cur`) | `bool`; momentary — any later allocation can invalidate it |

Counted fill after `Reserve` (room is guaranteed, so the `TryPush` result is
not checked; the loop must not exceed `n`):

```c
ints t = *s;
Reserve(arena, &t, t.len + n);
for (isize i = 0; i < n; i++) *TryPush(&t) = f(i);
*s = t;
```

Slots are uninitialized: the caller must write `*slot` before reading. A
slot dies at the next growing call on that slice; indices and `At(s, i)`
stay valid across moves. `New` storage is zeroed unless `NO_INIT`; `New`
evaluates `arena` once, copy-initializes from a `T*` 4th argument. OOM is
per-call: `longjmp` to the `ArenaOOM` handler (default, one-shot), `NULL`
(`OOM_NULL`), or trap (`OOM_TRAP` build).

Argument evaluation: every macro binds each runtime argument (`arena`,
`slice`, value, `min_cap`/`n`, `out`, `start`/`len`, index, pointer)
**exactly once** (`slice.macro_args_evaluated_once`,
`slice.clone_evaluates_args_once`, `slice.pushn_evaluates_args_once`).

## 3. Checks

### 3.1 Compile-time (hard errors, even without `-Werror`)

| Check | Mechanism | Applies to |
|---|---|---|
| Descriptor size/offsets/alignment match `SliceInternal` | `_Static_assert` in `_SliceAssertStatic` | every typed operation |
| `a` is a real, non-empty, constant-extent C array (not pointer/VLA); element size nonzero; length/size fit `isize`; alignment is a power of two; stride is a multiple of alignment | `_Static_assert` in `_SliceAssertArray` | `SliceFromArray` |
| Element type identity (`__builtin_types_compatible_p`: typedefs transparent, top-level qualifiers ignored, no conversions) | `_Static_assert` in `_AssertTypesCompatible` | `Pop` out-param, `SliceFromArray` dst-vs-array, `Clone` dst-vs-src, `Ix` pointer-vs-element |
| Slot/base return type is `T*` | `__typeof__(*data) *` | `TryPush`/`Push` slots, `PushN` base; wrong-type stores are ordinary C assignment diagnostics |
| Destination is a movable `Slice` | inlined `_Static_assert(_Generic(_kind, MovableKind: 1, …))` | `SliceFromArray`, `TryPush(arena, s)`, `Push`, `PushN`, `Reserve`; `FixedKind` or unknown hand-written tags fail; tagless descriptors fail on `(_kind)` member access |
| Operation argument is a descriptor pointer | member access in the macro body | all ops; passing the descriptor by value is rejected |
| 1-arg `TryPush` never grows | layout check only, no tag dispatch | structurally compatible descriptors (incl. unknown hand-written tags), like `Pop`/`Clone` |

### 3.2 Runtime (`Assert`: active in release too; traps, logs only in debug)

| Check | Location |
|---|---|
| Slice state (`len >= 0`, `len <= cap`, `data` present unless empty, aligned) | debug-only (`_SliceAssertRuntimeDebug` in the typed macros; erased under `NDEBUG`) |
| `min_cap >= 0` | always in `arena_slice_reserve` (every growth path) |
| `n > 0` | `PushN` arg |
| `n >= 0` | `SliceFixed`, `Reserve` args |
| Full → `NULL`, no append | `TryPush` (both forms) |
| At-tip extension only when `data + size*cap == arena->cur` | `arena_slice_grow_at_tip` |
| `At` bounds (`0 <= i < len`); `Ix` pointer within `len` | `At`, `Ix` |
| `Clone` source present unless empty; `0 <= start`, `0 <= len`, `start <= src->len - len` | `_CloneDst` |
| Empty `Pop` → `false`, `*out` untouched | `Pop` |

### 3.3 Nodiscard

`Pop` is nodiscard (`_PopOk` carries `warn_unused_result`): discarding the
return is a compile error under `-Werror`. `TryPush`/`Push` return slots
through a statement expression, which cannot carry `warn_unused_result` —
ignoring the slot compiles and (when full) silently skips the append. Always
bind or test the slot.

### 3.4 Intentionally unchecked

No compile-time or runtime check proves these; they are caller obligations:

- liveness of a `data`/element pointer or slot across movable `Push` /
  `PushN` / `Reserve` / `Clone`-into-it;
- arena lifetime after `Scratch` exit, `arena_reset`, `arena_release`,
  `arena_free`;
- reuse of a popped slot; explicit descriptor copies (`=`, struct copy,
  `memcpy`); hand-written descriptors over caller memory; descriptors
  copied inside a struct by value.

ASan/UBSan are diagnostic aids, not API contract (old bump blocks are not
freed, so stale aliases often still read as live).

## 4. Stale-reference matrix

A reference = raw `T*` into backing, a slot, a subslice descriptor, or a
descriptor copy sharing the block. Only the Enforcement column is
guaranteed; everything else compiles and runs without a diagnostic.

| Operation | What goes stale | Enforcement |
|---|---|---|
| `SliceFromArray` | source array (copied — safe); new backing still has arena lifetime | array/type checks; arena lifetime not enforced |
| `Push` / `PushN` (movable) | every prior `T*`, slot, subslice, and descriptor copy into the old block on off-tip growth; at-tip growth moves nothing | layout/class checks; debug-only state check; aliases not enforced |
| `Reserve` (movable) | same, even when only capacity changes | layout/state checks; aliases not enforced |
| `TryPush` (both forms, room available) | nothing moves | full → `NULL`; arena lifetime not enforced |
| `Push` / `PushN` / `Reserve` / `TryPush(arena, s)` on fixed | n/a (no growth path) | compile error (`expected a movable Slice`) |
| `Pop` | the popped slot (memory kept, next append overwrites it); rest stays valid | empty → `false`, `*out` untouched; slot reuse not enforced |
| `SliceFixed` on an initialized value | prior backing and its aliases (orphaned) | capacity/stride checks; old aliases not enforced |
| `Clone` into `out` | `out`'s prior backing (orphaned); shallow: element-held pointers still alias the source | bounds/layout checks; relocatable element types not rejected; arena lifetime not enforced |
| Descriptor copies; `Scratch`/reset/release | everything sharing the block / range | none |
| Hand-written `{data, len, cap}` over caller memory | external ownership/lifetime; a later `Reserve` moves it into the arena | layout checks only |

Survival rules: store indices or offsets across growth; reacquire `data`
after any growing call; never keep popped-slot pointers; treat
`Scratch`/reset as freeing every slice in range.

## 5. Performance

Loop speed is decided by descriptor placement, not by which append macro is
spelled. A loop over an arena-owned descriptor keeps `len`/`data` in memory
(the stores may alias them); a loop over a stack-owned copy or a hoisted
base pointer lets the compiler keep them in registers and vectorize.

| Pattern (N = 8M `int`, fill-only, best-of-7) | ms | Codegen |
|---|---|---|
| Raw `d[i] = f(i)` (reference) | 0.42 | NEON, 16/iter |
| `PushN` over shuffled copy | 0.61 | NEON |
| Unchecked `TryPush` over shuffled copy + `Reserve(len+N)` + out-of-loop `Assert` | 0.77 | NEON, scalar tail |
| Shuffled copy + raw writes + single `len` bump | 0.61 | NEON |
| Stack-owned + checked `TryPush` loop | 3.90 | scalar (`cbz` per iter) |
| Direct arena-owned `TryPush` loop | 14.36 | scalar + descriptor reloads per iter |
| Direct arena-owned raw writes + `len` bump | 2.60 | scalar (stores may alias `s->len`) |

Measured with Apple clang 21, `-O2 -DNDEBUG`, Apple Silicon, checksums
equal (`N(N-1)/2`) in all patterns. Consequences:

- Counted fills: claim once (`PushN`), loop over the base. The loop has no
  per-iteration guard or call.
- Uncounted/conditional fills: `Reserve` a budget, then checked 1-arg
  `TryPush` — but keep it scalar-costed; do not expect parity with raw.
- Never loop over an arena-owned descriptor: shuffle to the stack first
  (`ints t = *s; …; *s = t`), for `TryPush` fills, `At` write loops, and
  `Pop` drains alike. `At` read loops (`sum += At(s, i)` with `i < s->len`)
  vectorize as-is; LLVM proves `i < len` and removes the check.
- Apple clang 21 segfaults (frontend) at `-O2 -DNDEBUG` on the spelling that
  calls `Reserve` directly on a stack-owned local and then loops unchecked
  `TryPush` over it; reserving through the arena-owned descriptor, copying it
  to the stack, and looping over the copy compiles. Numbers above otherwise
  differ by toolchain, not by spelling.

## 6. Tests and verification

```text
make debug            # ASan+UBSan, -O0
./build/cmd           # full suite; --filter='slice.*' for slices
make typecheck        # 18 pass (must compile, -Werror) + 29 fail (must reject)
CC='cc -std=c23' ./test/typecheck.sh
CC='cosmocc -mcosmo' ./test/typecheck.sh
```

Last recorded: **163 cases pass** (74 `slice.*`), **18 compiled, 29
rejected** on all three toolchains, 0 build warnings.

- `src/slice_tests.c`: construction (`SliceFromArray` ownership/scope,
  typed destination, over-aligned), empty edges (`New`-zeroed, empty
  `Clone`, first-`Push` allocation, fixed at/off-tip), `Clone`
  (full/subrange/empty), `Push` (at-tip/off-tip/relocation/structs),
  `PushN` (claim/grow/preserve/append/zero-traps/once-only), `Pop` (last/cap/slots/
  round-trips/empty-safe), `Reserve` (grow/preserve/grow-only/at-tip),
  layout/type identity, descriptor passing (pointer vs value, struct
  fields), lifetimes (fixed outlives builder, movable survives `Scratch`,
  fixed never relocates), `SliceAtTip` transitions, overlap/`Scratch`,
  nodiscard behavior, once-only argument evaluation per entry point.
- Fixtures, pass (18): `const_fixed_copy`, `data_lvalue_and_layout`,
  `duplicate_expansions`, `fixed_slice`, `from_array_alignment`,
  `from_array_copy`, `from_array_typed`, `ix_roundtrip`,
  `multi_token_types`, `pop_typed`, `push_at_tip`, `push_no_grow`,
  `push_typed`, `pushn_typed`, `slice_classes_legal`, `struct_fields`,
  `trypush_ignored_compiles`, `trypush_no_arena_movable`.
- Fixtures, fail (29): `SliceFromArray` misuse — `from_array_pointer`,
  `from_array_pointer_to_array`, `from_array_vla`, `from_array_zero_length`,
  `from_array_type_mismatch`, `from_array_const_mismatch`,
  `from_array_const_destination`,
  `from_array_assign_type_mismatch`,
  `from_array_fixed_destination`; const/value misuse — `push_into_non_slice`,
  `push_const_slice`, `fixed_push_const`; by-value — `slice_by_value`;
  `Pop` — `pop_const_out`; data-pointer — `slice_data_wrong_pointer_type`;
  class/handle — `fixed_assign_to_slice`, `slice_assign_to_fixed`,
  `push_at_tip_on_fixed`, `push_unknown_tag`, `reserve_on_fixed_handle`,
  `reserve_unknown_tag`, `growable_push_on_fixed`,
  `ix_pointer_type_mismatch`, `pushn_on_fixed`, `pushn_unknown_tag`;
  nodiscard — `pop_ignored`, `astr_concat_ignored`, `astr_cstrdup_ignored`,
  `astr_trim_ignored` (ignored-`TryPush` compiles by design:
  `trypush_ignored_compiles`).

## 7. Non-goals and divergences

- No static roots, lifetimes, or escape analysis: arena lifetime is the only
  lifetime. `Scratch` slices can escape; nothing checks it.
- Movable relocation is C-only (no Goose equivalent): raw pointers, slots,
  and descriptor copies dangle after off-tip growth; indices and `At` do not.
- `Clone` is shallow and unconditional: no relocatable/self-relative type
  trait; element-held pointers keep aliasing the source.
- Shrink is `Pop` only (single element, empty-safe `false` with `*out`
  untouched): no `pop_n`/`resize`/`clear`, no shrink-liveness rules.
- Metadata is `isize`/`size_t` throughout; no 48-bit limit, no `u32`/`varint`
  length fields; fixed capacity is runtime, not a compile-time parameter.
- `const` is shallow: `const` array → mutable slice is rejected, adding
  `const` is allowed; no deep writability provenance.
- `T[:]`/`T[:k]` views do not exist; the only array input is the owning
  `SliceFromArray` copy.
