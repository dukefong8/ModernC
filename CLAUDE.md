# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A single-binary C project exploring "modern C" idioms: one hand-written arena allocator
(`include/arena.h`) plus vendored macro-heavy single-header libraries (datatype99/interface99,
verstable, bgen, utest, ...). There is no library to link against — `src/` is a demo/test harness
for the headers.

## Commands

```bash
make            # == make debug: ASan+UBSan, -O0 -g3, -DLOGGING -DOOM_COMMIT -> build/cmd
make release    # -O2 -g -DNDEBUG -DOOM_COMMIT
make clean      # rm -rf build/
make deps       # re-download all vendored headers into include/ (network; clobbers local edits)
make watch      # requires `entr`: syntax-only recheck of every .c/.h on change
make typecheck  # compile-only fixtures: which arena.h slice misuses must fail to compile
```

Run tests by running the binary — it executes the demos in `main()` first, then every `UTEST`
case via `utest_main`, and its exit status is the test result:

```bash
./build/cmd                          # all tests
./build/cmd --filter='arena.oom*'    # single test / suite, `*` wildcards, `suite.name` form
```

`.envrc` sets `ASAN_OPTIONS`/`UBSAN_OPTIONS` with `abort_on_error=1:halt_on_error=1` (direnv, or
export by hand) so sanitizer findings fail fast instead of limping on.

Two Makefile quirks worth knowing before editing it: `SRC` is `find . -name '*.c'`, so any new
`.c` file anywhere (including `include/json.c`) is compiled in automatically — there is nothing to
register. And `NAME = cmd` does not correspond to any source file, so the `$(LIB_TARGET)` rule
inflates every object including `main.o`; it is vestigial, not a real library target.

## Architecture

**`include/arena.h` is the core of the project.** Everything else exists to exercise it. It is
header-only, `ARENA_INLINE` (always-inline static), and has two build-time personalities selected
by defines that also change `struct Arena`'s layout:

- default: `arena_init(buf, size)` over a caller-supplied buffer (`malloc`'d or stack).
- `-DOOM_COMMIT` (set by both make targets): `arena_init(NULL, size)` reserves `size` bytes of
  virtual address space with `PROT_NONE` mmap and commits `ARENA_COMMIT_PAGE_COUNT` pages at a
  time via `mprotect`, so a multi-GB arena costs only the committed pages. `arena_release` munmaps
  in this mode, `free`s otherwise.
- `-DOOM_TRAP` removes the `jmp_buf *oom` field entirely and traps on OOM instead of longjmp.

OOM policy is per-call and three-way: default is `longjmp` to the `jmp_buf` registered with
`ArenaOOM(arena, jmpbuf)`; the `OOM_NULL` flag returns `NULL` instead; `OOM_TRAP` asserts. The
`ArenaOOM` macro *must* stay a single controlling expression containing `setjmp` (hence the odd
`_oom_result` temp) — the handler is cleared on fire, so it is one-shot.

The API surface built on top: `New(arena, T[, n][, NO_INIT | OOM_NULL | src_ptr])` (`_Generic`
selects copy-init when the 4th arg is a pointer), `Scratch(arena)` for scope-restored temporaries,
`Slice(T)` + `Push`/`PushGrow`/`Reserve`/`Clone`/`SliceAtTip`, and the `astr` length-prefixed string
family (`S(s)` for printf, `astr_split`/`astr_split_by_char` iterators, trim/slice/find/hash).

`PushGrow(arena, s)` is the always-growing form: it returns an uninitialized slot, requires a movable
`Slice`, and may relocate. `Push(arena, s)` grows only when the backing ends at the arena tip (or the
slice is empty): an in-place capacity bump, so nothing moves; off-tip it returns NULL when full
(uninitialized slot on success). `Push(&b)` is the fixed-capacity form: `FixedSlice` only, never
allocates or relocates, and returns NULL when full (uninitialized slot). A movable `Slice` must
name its arena, via `Push(arena, s)` or `PushGrow`; `Push(movable)` is a compile error. Elements
are written through the returned pointer: `*PushGrow(arena, s) = v;` (caller must initialize
`*slot` before reading). An element
whose contents are produced rather than known up front is written field by field through the
slot; `New` allocates a standalone object, it does not append. `Reserve(arena, s, n)` grows
capacity up front to avoid `NULL` checks in fill loops (debug builds still trap on state
violations). A held slot dies at the next growing call: reacquire it after `Push(arena, s)`,
`PushGrow`/`Reserve`, or reach elements by index with `At(s, i)`.

`Slice(T)` is a plain descriptor union (`T *data`, `len`, `cap`) that expands directly to an
anonymous union — no `__typeof__`, no array wrapper. `Slice(struct Point)` and
`Slice(const char*)` work as written, and repeat expansions stay distinct types, so name the
type with a typedef. Allocate a movable descriptor in the arena with `New(arena, ints)` (zeroed
into a valid empty slice) and pass the pointer to every operation; the descriptor and its
backing block share the arena lifetime. Its address stays fixed while `data` may relocate.
Elements are read through `s->data[i]`. Plain descriptor assignment, `memcpy`, and struct copies
compile and alias silently. `SliceFromArray(arena, out, array)` copies a C array into arena
storage; there is no zero-copy caller-backed view. Two classes, both separate types:
`FixedSlice` is fixed-capacity only, and ordinary `Slice` is movable (growth may relocate; at
the tip it extends in place). `Clone` writes through a destination pointer:
`Clone(arena, out, src[, start[, len]])`.

Storage/passing rubric: movable `Slice` is stored and passed by pointer (`ints* s = New(arena,
ints)`; `ints*` struct field), because growth can relocate `data` and a value copy would pin the
old block. `FixedSlice` is stored and passed by value (`fints b = {0};`; `fints` struct field),
because its backing never relocates; fixed operations take `&b`. A fixed value copy is a valid
view with shared data but independent `len`/`cap`. Use `Clone` for independent element copies.
There is no runtime guard for descriptor aliases; ASan generally will not catch them because
relocation does not free the old arena block.

The layout must stay identical to `SliceInternal`, which `arena_slice_reserve` memcpys —
`slice_tests.c` static-asserts the offsets, and `make typecheck` holds the compile-time fixtures.
Slice macros exist where C needs a type name, `_kind` dispatch, an initializer, or a return
value. Only capacity growth is type-erased (`arena_slice_reserve`, `arena_slice_grow`,
`_arena_at_tip`, exposed as `SliceAtTip(arena, s)`); slot computation, typed stores, and descriptor
updates stay inline in the macros. Statement macros use `do { } while (0)`; value-returning macros use a statement
expression only where required. Each operation macro binds its slice/value/capacity arguments
once (checks are unevaluated).

**Allocation order is semantically load-bearing.** Several "optimizations" are really contracts
with the caller: `arena_free` only reclaims when the pointer is at the bump tip; `PushGrow` grows in
place only when the slice sits at the tip; `astr_clone`/`astr_concat` skip copying when the source
ends exactly at `arena->cur`, and a concat whose tail starts right where an at-tip head ends (the
`s = astr_concat(arena, s, astr_format(arena, ...))` chain shape) extends `s` in place with no
copying at all. So these functions silently degrade or no-op depending on what was
allocated just before, and an unrelated allocation can turn a free into a leak. When you see code
that looks like a pointless redundant copy, it is usually defending against—or relying on—this
at-tip property.

**Memory is ASan-poisoned at every logical free point** (`arena_reset`, `Scratch` exit,
`arena_restore`, `arena_release`, `arena_free`, unallocated regions). Touching an arena pointer
past its logical lifetime is a hard ASan error in `debug` builds, not silent corruption. Do not
remove those `ASAN_POISON_MEMORY_REGION` calls or "simplify" the code they guard — this is the
project's main safety net.

**Harness layout:** `src/main.c` owns the process: it lazily creates a per-thread default arena
(`arena_default()`, thread-local, `DEFAULT_ARENA_SIZE` ≈ 4 GB aligned in the default build),
installs the `ArenaOOM` longjmp handler for it, runs the demos, then defers to `utest_main`.
`UTEST_STATE()` lives there too and must stay in exactly one translation unit.

Tests are colocated with what they test rather than in a separate tree — `src/arena_tests.c`,
`src/astr_tests.c`, and `UTEST` blocks inline in `demo.c`, `adt.h`, and `main.c`. Adding a test
file or block requires no registration.

The one exception is `test/typecheck*`, which holds what a runtime suite cannot: fixtures that
must (or must not) compile. They deliberately use a non-`.c` extension so `SRC`'s `find` never
builds them; `make typecheck` runs them.

The remaining `src/` files are self-contained demos of a style, each showing how a differently
shaped library plugs into the arena: `adt.h` (datatype99 `match`/`of` over an arena-allocated
expression tree), `object.h`/`object.c` (interface99 vtables), `demo.c` (verstable hash maps and
bgen priority queues, both wired to the arena through the `arena_malloc`/`arena_free` shim and
`CTX_TY Arena*`).

## Editor/format conventions

`.clang-format` is Google-based, `ColumnLimit: 110`, `AlignConsecutiveMacros: true`, and lists
`INITIALIZER`, `ML99_EVAL`, `vfunc`, `vfuncDefault` as `StatementMacros`; run `clang-format -i` on
files you touch. `.clangd` compiles with `-D_CLANGD`, and `adt.h` wraps its datatype99 code in
`#ifndef _CLANGD` because clangd cannot expand the metalang99 macros — keep macro-heavy code
inside such guards rather than fighting the language server.

Vendored headers under `include/` are third-party (also listed in `include/.package` for the
`pkg.sh` fetcher). Prefer changing `src/`, not the vendored copies — `make deps` overwrites them.
`include/utf8.h` is currently a 404 body fetched by a stale URL in the Makefile's `deps` target;
nothing includes it.
