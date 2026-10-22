/**
 * @file arena.h
 * @brief Bump-pointer arena allocator plus `astr` strings and `Slice` arrays.
 *
 * Single-header, no library to link. See nullprogram.com blog posts linked
 * below for the allocator design.
 *
 * @see https://nullprogram.com/blog/2023/09/27/
 * @see https://nullprogram.com/blog/2023/10/05/
 */

#ifndef ARENA_H_
#define ARENA_H_

#include <setjmp.h>
#include <stdalign.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AlignPow2(x, b)     (((x) + (b) - 1) & (~((b) - 1)))
#define AlignDownPow2(x, b) ((x) & (~((b) - 1)))
#define AlignPadPow2(x, b)  (-(x) & ((b) - 1))
#define IsPow2(x)           ((x) != 0 && ((x) & ((x) - 1)) == 0)
#define IsPow2OrZero(x)     ((((x) - 1) & (x)) == 0)

#define Min(a, b) ((a) < (b) ? (a) : (b))
#define Max(a, b) ((a) > (b) ? (a) : (b))

#define KB(n) (((size_t)(n)) << 10)
#define MB(n) (((size_t)(n)) << 20)
#define GB(n) (((size_t)(n)) << 30)
#define TB(n) (((size_t)(n)) << 40)

#ifdef __GNUC__
#define ARENA_INLINE static inline __attribute__((always_inline))
#else
#define ARENA_INLINE static inline
#endif

#ifdef __GNUC__
#define ARENA_LIKELY(xp)   __builtin_expect((bool)(xp), true)
#define ARENA_UNLIKELY(xp) __builtin_expect((bool)(xp), false)
#else
#define ARENA_LIKELY(xp)   (xp)
#define ARENA_UNLIKELY(xp) (xp)
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define ARENA_NODISCARD [[nodiscard]]
#elif defined(__GNUC__)
#define ARENA_NODISCARD __attribute__((warn_unused_result))
#else
#define ARENA_NODISCARD
#endif

// clang-format off
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define ASAN_DETECTED
#  endif
#elif defined(__SANITIZE_ADDRESS__)
#  define ASAN_DETECTED
#endif

#if defined(ASAN_DETECTED)
#  if defined(__has_include)
#    if __has_include(<sanitizer/asan_interface.h>)
#      define ASAN_ENABLED
#    endif
#  else
#    define ASAN_ENABLED
#  endif
#endif
// clang-format on

#ifdef ASAN_ENABLED
#include <sanitizer/asan_interface.h>
#else
#define ASAN_POISON_MEMORY_REGION(addr, size)   ((void)(addr), (void)(size))
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) ((void)(addr), (void)(size))
#endif

// Platform-specific debug breakpoint
#ifdef __clang__
#define DEBUG_TRAP() __builtin_debugtrap()
#elif defined(__x86_64__)
#define DEBUG_TRAP() __asm__("int3; nop")
#elif defined(__GNUC__)
#define DEBUG_TRAP() __builtin_trap()
#else
#include <signal.h>
#define DEBUG_TRAP() raise(SIGTRAP)
#endif

#ifndef NDEBUG
#define ASSERT_LOG(c) fprintf(stderr, "Assertion failed: " c " at %s %s:%d\n", __func__, __FILE__, __LINE__)
#else
#define ASSERT_LOG(c) (void)0
#endif

/**
 * @brief Runtime assertion with debug trap on failure.
 * @param c Condition to check
 *
 * Unlike standard assert(), this is not compiled out in release builds.
 */
#define Assert(c)               \
  do {                          \
    if (ARENA_UNLIKELY(!(c))) { \
      ASSERT_LOG(#c);           \
      DEBUG_TRAP();             \
    }                           \
  } while (0)

#ifdef OOM_COMMIT
#include <sys/mman.h>
#include <unistd.h>

// Pages to commit at once when growing arena (default 1MB on 4KB pages)
#ifndef ARENA_COMMIT_PAGE_COUNT
#define ARENA_COMMIT_PAGE_COUNT 256
#endif

// @return System page size in bytes
ARENA_INLINE size_t arena_os_pagesize(void) {
  return (size_t)sysconf(_SC_PAGESIZE);
}

/**
 * @brief Reserve virtual address space without physical memory.
 * @param size Bytes to reserve
 * @return Pointer to reserved space, or NULL on failure
 */
ARENA_INLINE void* arena_os_reserve(size_t size) {
  void* ptr = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return (ptr == MAP_FAILED) ? NULL : ptr;
}

/**
 * @brief Commit physical memory to reserved pages.
 * @param ptr Start of reserved region
 * @param size Bytes to commit
 * @return true on success, false on failure
 */
ARENA_INLINE bool arena_os_commit(void* ptr, size_t size) {
  return mprotect(ptr, size, PROT_READ | PROT_WRITE) == 0;
}

/**
 * @brief Release physical memory while keeping virtual reservation.
 * @param ptr Start of committed region
 * @param size Bytes to decommit
 */
ARENA_INLINE void arena_os_decommit(void* ptr, size_t size) {
  if (size > 0) {
    madvise(ptr, size, MADV_DONTNEED);
  }
}
#endif

typedef unsigned char byte;
typedef ptrdiff_t isize;  // Signed size type for pointer arithmetic

/**
 * @brief Arena allocator context.
 *
 * Manages contiguous memory region with bump-pointer allocation.
 * All fields are internal - use provided functions/macros.
 */
typedef struct Arena Arena;
struct Arena {
  byte* beg;  // Start of arena memory
  byte* cur;  // Current allocation position
  byte* end;  // End of committed memory
#ifndef OOM_TRAP
  jmp_buf* oom;  // Longjmp target for OOM recovery
#endif
#ifdef OOM_COMMIT
  isize commit_size;   // Bytes to commit per growth
  isize reserve_size;  // Total reserved virtual space
#endif
};

// Allocation flags
enum {
  _NO_INIT = 1u << 0,   // Skip zero-initialization
  _OOM_NULL = 1u << 1,  // Return NULL on OOM instead of longjmp
};

typedef struct {
  unsigned mask;
} ArenaFlag;

static const ArenaFlag NO_INIT = {_NO_INIT};
static const ArenaFlag OOM_NULL = {_OOM_NULL};

#ifdef __GNUC__
/**
 * Auto-free pointer when leaving scope.
 *
 *   __autofree void *ptr = malloc(size);
 */
#define __autofree __attribute__((__cleanup__(autofree_impl)))
static void autofree_impl(void* p) {
  free(*((void**)p));
}
#else
#warning "__autofree not supported on this compiler"
#define __autofree
#endif

/**
 * Allocate memory for type T from arena.
 *
 *   int *x = New(arena, int);                 // zeroed int
 *   int *arr = New(arena, int, 10);           // 10 zeroed ints
 *   int *raw = New(arena, int, 10, NO_INIT);  // uninitialized
 *   struct point *p2 = New(arena, struct point, 1, p);  // copy from p
 */
#define New(...)                  _NEWX(__VA_ARGS__, _NEW4, _NEW3, _NEW2)(__VA_ARGS__)
#define _NEWX(a, b, c, d, e, ...) e
#define _NEW2(a, t)               (t*)arena_alloc(a, sizeof(t), alignof(t), 1, (ArenaFlag){0})
#define _NEW3(a, t, n)            (t*)arena_alloc(a, sizeof(t), alignof(t), n, (ArenaFlag){0})
#define _NEW4(a, t, n, z) \
  (t*)_Generic((z), t*: arena_alloc_init, ArenaFlag: arena_alloc)(a, sizeof(t), alignof(t), n, (z))

#define CONCAT_(a, b) a##b
#define CONCAT(a, b)  CONCAT_(a, b)
#define ARENA_ORIG    CONCAT(_arena_, __LINE__)

/**
 * Scope-bound arena checkpoint. Restores `arena->cur` on scope exit.
 * Pointers allocated inside must not escape the scope.
 *
 *   { Scratch(arena); char *tmp = New(arena, char, 100); ... }
 */
#define Scratch(arena)                       \
  __arena_restore Arena* ARENA_ORIG = arena; \
  Arena arena[] = {*ARENA_ORIG}

#ifdef __GNUC__
#define __arena_restore __attribute__((__cleanup__(arena_restore)))
#else
#define __arena_restore
#endif

static void arena_restore(Arena** a) {
  ASAN_POISON_MEMORY_REGION(a[0]->cur, a[0]->end - a[0]->cur);
#ifdef OOM_COMMIT
  if (a[0]->commit_size) {
    isize orig_size = a[0]->end - a[0]->beg;
    arena_os_decommit(a[0]->end, a[0]->reserve_size - orig_size);
  }
#endif
}

/**
 * Type-erased slice layout. Every slice descriptor is `{data, len, cap}`;
 * offsets are static-asserted in arena_tests.c.
 */
typedef struct SliceInternal {
  void* data;
  isize len;
  isize cap;
} SliceInternal;

/**
 * Define a dynamic array type: `typedef Slice(long) i64s;`
 *
 * A descriptor is one `{T *data; isize len, cap; }` value wrapped in a
 * one-element array, so a function parameter of slice type decays to a
 * pointer to the caller's descriptor: mutations are visible to the caller,
 * and `t = s`, `ints t = s`, or returning a slice by value are compile
 * errors. Access elements as `s->data[i]`; pass `s` itself, not `&s`.
 * Zero-initialize before first use (`{}`).
 *
 * Two classes, selected by type (see Slice.md for the Goose mapping):
 * movable `Slice` (growth may relocate) and `FixedSlice` (fixed capacity,
 * never grows). `Push`/`Reserve` dispatch on the zero-storage `_kind` tag, which
 * overlays `data` and is never read at runtime.
 *
 * Lifetime: `Push`/`Reserve` on a movable slice may relocate, so re-read
 * `s->data` after growth instead of keeping element pointers. `Scratch`,
 * `arena_reset`, and `arena_release` invalidate all slice storage; `Clone`
 * is shallow. Raw descriptor copies (`copy[0] = s[0]`, `memcpy`, struct
 * copies) alias silently: use `Clone` for element copies,
 * `SliceMoveAssign` for moves, and pass slice-holding structs by pointer.
 * Forging a descriptor over caller memory is unsupported and undetected.
 */
/* Type tags used only by _Generic; they overlay data and are never read. */
// clang-format off
typedef struct { unsigned char _; } MovableKind;
typedef struct { unsigned char _; } FixedKind;
// clang-format on

#define Slice(T)       \
  __typeof__(((union { \
    struct {           \
      T* data;         \
      isize len;       \
      isize cap;       \
    };                 \
    MovableKind _kind; \
  }[1]){}))

/**
 * Fixed-capacity handle (Goose T[..]), tagged `FixedKind`. Three-argument
 * `Push`/`Reserve` do not compile on it (no matching `_Generic`
 * association). Ops: `Push(b, v)`, `Pop(b, &out)`, `PushUnchecked`.
 * Copy out with `Clone`. For growth that never moves while capacity
 * lasts, `Reserve` up front and stay within `cap`.
 *
 * The two-argument `Push` requires a `FixedSlice`: it appends without
 * growing, returning -1 at cap instead of relocating. Pass the arena to grow.
 */
#define FixedSlice(T)  \
  __typeof__(((union { \
    struct {           \
      T* data;         \
      isize len;       \
      isize cap;       \
    };                 \
    FixedKind _kind;   \
  }[1]){}))

/**
 * Layout checks: descriptor size, field offsets, and alignment match
 * `SliceInternal`, the type-erased layout `arena_slice_reserve` memcpys.
 * Element alignment is irrelevant here (`data` is a pointer).
 */
#define _SliceAssertStatic(s)                                                             \
  do {                                                                                    \
    _Static_assert(sizeof(*(s)) == sizeof(SliceInternal),                                 \
                   "Slice(T) must be layout-compatible with SliceInternal");              \
    _Static_assert(offsetof(__typeof__(*(s)), data) == offsetof(SliceInternal, data) &&   \
                       offsetof(__typeof__(*(s)), len) == offsetof(SliceInternal, len) && \
                       offsetof(__typeof__(*(s)), cap) == offsetof(SliceInternal, cap),   \
                   "Slice(T) must order data/len/cap like SliceInternal");                \
    _Static_assert(_Alignof(__typeof__(*(s))) == _Alignof(SliceInternal),                 \
                   "Slice(T) must be aligned like SliceInternal");                        \
  } while (0)

/** Fixed descriptor: same footprint as SliceInternal, tagged FixedKind. */
#define _FixedSliceAssertStatic(s)                                           \
  do {                                                                       \
    _SliceAssertStatic(s);                                                   \
    (void)_SliceStaticAssert(_Generic((s)->_kind, FixedKind: 1, default: 0), \
                             "expected a FixedSlice handle");                \
  } while (0)

/**
 * `_Static_assert` usable inside an expression (initializer, comma chain,
 * macro body): the struct declaration scopes the check without emitting code.
 */
#define _SliceStaticAssert(cond, msg) \
  (sizeof(struct {                    \
    _Static_assert(cond, msg);        \
    int _slice_static_assert_;        \
  })                                  \
       ? 0                            \
       : 0)

/** Element count of a real C array, as isize. */
#define _SliceArrayLen(a) ((isize)(sizeof(a) / sizeof(*(a))))

/**
 * Reject anything that is not a real, constant-extent C array. The `_Generic`
 * test matches `&(a)` against "pointer to array of exactly N elements", so
 * decayed pointers (and VLAs, whose length is not a constant expression)
 * fail. Expression-form so it can guard an initializer.
 */
#define _SliceAssertArray(a)                                                                            \
  (_SliceStaticAssert(_Generic(&(a), __typeof__ (*(a))(*)[_SliceArrayLen(a)]: 1, default: 0),           \
                      "array argument must be a C array, not a pointer") &                              \
   _SliceStaticAssert(_SliceArrayLen(a) > 0, "array argument must be a non-empty array") &              \
   _SliceStaticAssert(sizeof(*(a)) > 0, "array element type must not be zero-size") &                   \
   _SliceStaticAssert(_SliceArrayLen(a) <= PTRDIFF_MAX, "array length does not fit isize") &            \
   _SliceStaticAssert(sizeof(a) <= (size_t)PTRDIFF_MAX, "array size does not fit isize") &              \
   _SliceStaticAssert(IsPow2(_Alignof(__typeof__(*(a)))), "element alignment must be a power of two") & \
   _SliceStaticAssert(sizeof(*(a)) % _Alignof(__typeof__(*(a))) == 0,                                   \
                      "element size must be a multiple of its alignment"))

/**
 * Hard element-type check for `Push`/`PushUnchecked` values and `Pop` output.
 * `__builtin_types_compatible_p` ignores top-level qualifiers; a mismatch is
 * a `_Static_assert` error even without `-Werror`.
 */
#define _SliceAssertValueType(data, v)                                                       \
  (void)_SliceStaticAssert(__builtin_types_compatible_p(__typeof__(v), __typeof__(*(data))), \
                           "value type does not match slice element type")

/**
 * Move a descriptor's contents into another, clearing the source.
 * (`{0}` first; slices cannot be assigned or returned as wholes.)
 */
#define SliceMoveAssign(dst, src)                                              \
  do {                                                                         \
    __auto_type _ma_dst = (dst);                                               \
    __auto_type _ma_src = (src);                                               \
    _SliceAssertStatic(_ma_dst);                                               \
    _SliceAssertStatic(_ma_src);                                               \
    Assert(_ma_dst != _ma_src && "SliceMoveAssign: dst and src are the same"); \
    _ma_dst->data = _ma_src->data;                                             \
    _ma_dst->len = _ma_src->len;                                               \
    _ma_dst->cap = _ma_src->cap;                                               \
    _ma_src->data = NULL;                                                      \
    _ma_src->len = 0;                                                          \
    _ma_src->cap = 0;                                                          \
  } while (0)

/**
 * Copy a C array into arena storage: `cap == len`, own backing block, source
 * may die immediately. Result is an ordinary movable slice.
 *
 * Writes into an existing movable `Slice` destination (a `FixedSlice` is a
 * compile error). Wrap comma-containing arrays in extra parens:
 * `((int[]){1, 2, 3})`.
 */
#define SliceFromArray(arena, s, a)                                                                        \
  ({                                                                                                       \
    __auto_type _fa_dst = (s);                                                                             \
    (void)_SliceStaticAssert(__builtin_types_compatible_p(__typeof__(*(_fa_dst->data)), __typeof__(*(a))), \
                             "slice element type does not match the array");                               \
    _SliceAssertStatic(_fa_dst);                                                                           \
    (void)_Generic(_fa_dst->_kind, MovableKind: 0); /* FixedSlice cannot be initialized this way */        \
    __auto_type _fa_arena = (arena);                                                                       \
    (void)_SliceAssertArray(a);                                                                            \
    isize _fa_len = _SliceArrayLen(a);                                                                     \
    _fa_dst->data = New(_fa_arena, __typeof__(*(a)), _fa_len, (a));                                        \
    _fa_dst->len = _fa_len;                                                                                \
    _fa_dst->cap = _fa_len;                                                                                \
    *_fa_dst;                                                                                              \
  })

/** Null `data` means empty; nothing is read through it. */
#define _SliceAligned(s) \
  ((s)->data == NULL || (((uintptr_t)(s)->data & ((uintptr_t)_Alignof(__typeof__(*(s)->data)) - 1)) == 0))

/** Always-on state check: `len >= 0`, `len <= cap`, non-null data unless empty, aligned data. */
#define _SliceAssertRuntime(s)                                                                            \
  do {                                                                                                    \
    Assert((s)->len >= 0 && "slice.len must be non-negative");                                            \
    Assert((s)->len <= (s)->cap && "slice.len must not exceed cap");                                      \
    Assert(((s)->data != NULL || (s)->len == 0) && "Invalid slice: no data");                             \
    Assert(((s)->data == NULL || _SliceAligned(s)) && "slice data must be aligned for its element type"); \
  } while (0)

/** Debug-only version of the state check above. */
#ifndef NDEBUG
#define _SliceAssertRuntimeDebug(s) _SliceAssertRuntime(s)
#else
#define _SliceAssertRuntimeDebug(s) (void)0
#endif

/** Debug-only `len < cap` guard; release `PushUnchecked` trusts the caller. */
#ifndef NDEBUG
#define _SliceAssertSpareDebug(s) \
  Assert((s)->len < (s)->cap && "PushUnchecked needs spare capacity: Reserve first, or use Push")
#else
#define _SliceAssertSpareDebug(s) (void)0
#endif

/**
 * Append a value. Forms:
 *
 *   Push(arena, s, v)   movable Slice: may grow/relocate
 *   Push(b, v)           FixedSlice: fixed cap, never grows
 *
 * The three-argument form names only `MovableKind` in its `_Generic` check
 * (no `default`), so a `FixedSlice` is a constraint error rather than a
 * silent growth path.
 *
 * Each form returns the index of the new element (`len - 1`); reach it with
 * `At(s, idx)` or `s->data[idx]`. An index stays valid across later moves,
 * while a raw element pointer does not. The fixed form is nodiscard (a full
 * push reports -1); the growable forms always succeed. `Pop` never relocates.
 */
#define Push(...)               _PushX(__VA_ARGS__, _Push3, _Push2)(__VA_ARGS__)
#define _PushX(a, b, c, d, ...) d

// Identity wrappers so the fallible forms carry the unused-result check
// through the macro expansion. Only the forms that can fail without
// aborting are wrapped (fixed Push reports -1 when full, Pop reports false
// when empty); the infallible forms always succeed and stay quiet when
// discarded.
ARENA_NODISCARD ARENA_INLINE isize _PushIdx(isize idx) {
  return idx;
}

ARENA_NODISCARD ARENA_INLINE bool _PopOk(bool ok) {
  return ok;
}

/* Fixed form: `FixedSlice` handle; no arena, no growth. Returns the new
 * element's index, or -1 when full (no append, no abort). Nodiscard: a
 * full push is silent unless the caller checks the result. */
#define _Push2(slice, val)               \
  _PushIdx(({                            \
    __auto_type _b = (slice);            \
    __auto_type _v = (val);              \
    _SliceAssertValueType(_b->data, _v); \
    _FixedSliceAssertStatic(_b);         \
    isize _b_idx = -1;                   \
    if (_b->len < _b->cap) {             \
      _b_idx = _b->len++;                \
      _b->data[_b_idx] = _v;             \
    }                                    \
    _b_idx;                              \
  }))

/* Three-argument form: only `MovableKind` is named, so a `FixedSlice`
 * (`FixedKind`) or any unknown tag is a constraint error rather than a
 * silent growth path. Growth is type-erased (`arena_slice_grow`). */
#define _Push3(arena, slice, val)                                                       \
  ({                                                                                    \
    __auto_type _s = (slice);                                                           \
    __auto_type _v = (val);                                                             \
    _SliceAssertValueType(_s->data, _v);                                                \
    _SliceAssertStatic(_s);                                                             \
    _SliceAssertRuntimeDebug(_s);                                                       \
    (void)_Generic(_s->_kind, MovableKind: 0); /* FixedSlice cannot grow this way */    \
    if (ARENA_UNLIKELY(_s->len >= _s->cap))                                             \
      arena_slice_grow((arena), _s, sizeof(*_s->data), alignof(__typeof__(*_s->data))); \
    isize _idx = _s->len++;                                                             \
    _s->data[_idx] = _v;                                                                \
    _idx;                                                                               \
  })

/**
 * Append within existing capacity; no arena, no allocation. Batch pattern:
 *
 *   Reserve(arena, s, s->len + n);
 *   for (isize i = 0; i < n; i++) PushUnchecked(s, (int64_t)i);
 *
 * Debug traps when out of room; release is unchecked (caller owns the bound).
 * Same value contract as `Push`, evaluated once; returns the new element's index.
 */
#define PushUnchecked(slice, val)        \
  ({                                     \
    __auto_type _s = (slice);            \
    __auto_type _v = (val);              \
    _SliceAssertValueType(_s->data, _v); \
    _SliceAssertStatic(_s);              \
    _SliceAssertRuntimeDebug(_s);        \
    _SliceAssertSpareDebug(_s);          \
    isize _idx = _s->len++;              \
    _s->data[_idx] = _v;                 \
    _idx;                                \
  })

/**
 * Remove the last element into `*out`; returns false (leaving `*out`
 * untouched) when empty. Never aborts, moves, or allocates. The slot is
 * not cleared and may be reused by the next `Push`. Structural: no arena
 * or `_kind` needed. Nodiscard: an empty pop is silent unless the caller
 * checks the result.
 */
#define Pop(slice, out)                   \
  _PopOk(({                                \
    __auto_type _s = (slice);             \
    __auto_type _o = (out);               \
    _SliceAssertValueType(_s->data, *_o); \
    _SliceAssertStatic(_s);               \
    _SliceAssertRuntimeDebug(_s);         \
    bool _popped = _s->len > 0;           \
    if (_popped)                          \
      *_o = _s->data[--_s->len];          \
    _popped;                              \
  }))

/**
 * Bounds-checked element access: `At(s, i)` is an lvalue for `s->data[i]`
 * through the current handle, so it stays valid across moves (unlike a held
 * element pointer). Both arguments are evaluated once. The bounds check is
 * always on, even in release; `s->data[i]` remains the unchecked form.
 */
#define At(s, i)                                                           \
  (*({                                                                     \
    __auto_type _at_s = (s);                                               \
    isize _at_i = (isize)(i);                                              \
    _SliceAssertStatic(_at_s);                                             \
    _SliceAssertRuntimeDebug(_at_s);                                       \
    Assert(_at_i >= 0 && _at_i < _at_s->len && "At: index out of bounds"); \
    &_at_s->data[_at_i];                                                   \
  }))

/**
 * Grow capacity to at least `min_cap` (checks always on, even in release).
 * At/below `cap` is a no-op. Movable `Slice` may relocate (old pointers go
 * stale); at the arena tip it extends in place. There is no `Reserve` form
 * for `FixedSlice` (capacity is fixed once by `SliceFixed`).
 */
#define Reserve(arena, slice, min_cap)                                                             \
  do {                                                                                             \
    __auto_type _s = (slice);                                                                      \
    isize _rsv_cap = (min_cap);                                                                    \
    _SliceAssertStatic(_s);                                                                        \
    _SliceAssertRuntimeDebug(_s);                                                                  \
    Assert(_rsv_cap >= 0 && "reserve size must be non-negative");                                  \
    (void)_Generic(_s->_kind, MovableKind: 0); /* FixedSlice cannot reserve this way */            \
    arena_slice_reserve((arena), _s, sizeof(*_s->data), alignof(__typeof__(*_s->data)), _rsv_cap); \
  } while (0)

/**
 * Initialize a `FixedSlice` (Goose T[..]): `len == 0`, no growth.
 * `Push(arena, b, ...)` and `Reserve(arena, b, ...)` are compile errors.
 * Use `Clone` to copy out.
 */
#define SliceFixed(arena, slice, capacity)                                                             \
  do {                                                                                                 \
    __auto_type _bs = (slice);                                                                         \
    isize _bs_cap = (capacity);                                                                        \
    _FixedSliceAssertStatic(_bs);                                                                      \
    _Static_assert(sizeof(_bs->data[0]) > 0, "fixed-size elements only");                              \
    _Static_assert(IsPow2(_Alignof(__typeof__(_bs->data[0]))), "element alignment");                   \
    _Static_assert(sizeof(_bs->data[0]) % _Alignof(__typeof__(_bs->data[0])) == 0, "element stride");  \
    Assert(_bs_cap >= 0 && "fixed capacity must be non-negative");                                     \
    _bs->data = NULL;                                                                                  \
    _bs->len = 0;                                                                                      \
    _bs->cap = 0;                                                                                      \
    if (_bs_cap > 0)                                                                                   \
      arena_slice_reserve((arena), _bs, sizeof(*_bs->data), alignof(__typeof__(*_bs->data)), _bs_cap); \
  } while (0)

/**
 * Copy a slice (or subrange) into a destination slice:
 * `Clone(arena, out, src[, start[, len]])`. Written in place (arrays cannot
 * be returned). Shallow: element-held pointers/slices still target the
 * original storage; the source descriptor is unchanged.
 */
#define _CloneDst(arena, out, src, start, length)                                                  \
  do {                                                                                             \
    __auto_type _cd_out = (out);                                                                   \
    __auto_type _cd_src = (src);                                                                   \
    isize _cd_st = (start);                                                                        \
    isize _cd_len = (length);                                                                      \
    _SliceAssertStatic(_cd_out);                                                                   \
    _SliceAssertStatic(_cd_src);                                                                   \
    (void)_SliceStaticAssert(                                                                      \
        __builtin_types_compatible_p(__typeof__(*_cd_out->data), __typeof__(*_cd_src->data)),      \
        "clone element type does not match the source");                                           \
    Assert((_cd_src->data != NULL || _cd_len == 0) && "Clone needs data to copy");                 \
    Assert(_cd_st >= 0 && _cd_len >= 0 && _cd_st <= _cd_src->len - _cd_len);                       \
    if (_cd_len > 0)                                                                               \
      _cd_out->data = New((arena), __typeof__(*_cd_src->data), _cd_len, (_cd_src->data + _cd_st)); \
    else                                                                                           \
      _cd_out->data = NULL;                                                                        \
    _cd_out->len = _cd_out->cap = _cd_len;                                                         \
  } while (0)

#define Clone(...)                     _CloneX(__VA_ARGS__, _Clone5, _Clone4, _Clone3)(__VA_ARGS__)
#define _CloneX(a, b, c, d, e, f, ...) f
#define _Clone3(arena, out, src)                       \
  do {                                                 \
    __auto_type _c3_src = (src);                       \
    _CloneDst((arena), out, _c3_src, 0, _c3_src->len); \
  } while (0)
#define _Clone4(arena, out, src, start)                              \
  do {                                                               \
    __auto_type _c4_src = (src);                                     \
    isize _c4_st = (start);                                          \
    _CloneDst((arena), out, _c4_src, _c4_st, _c4_src->len - _c4_st); \
  } while (0)
#define _Clone5(arena, out, src, start, length) _CloneDst((arena), out, (src), (start), (length))

/**
 * @brief Initialize an arena.
 * @param buf Caller buffer, or NULL to reserve/commit virtual space (`OOM_COMMIT`).
 * @param size Buffer size, or virtual reservation size.
 * @return The initialized arena context.
 */
ARENA_INLINE Arena arena_init(byte* buf, isize size) {
  Arena a = {0};
#ifdef OOM_COMMIT
  if (!buf) {
    isize page_size = arena_os_pagesize();
    size = AlignPow2(size, page_size);
    a.commit_size = page_size * ARENA_COMMIT_PAGE_COUNT;
    a.reserve_size = Max(a.commit_size, size);

    buf = arena_os_reserve(a.reserve_size);
    if (!buf) {
      perror("arena_init mmap");
      abort();
    }

    if (!arena_os_commit(buf, a.commit_size)) {
      perror("arena_init mprotect");
      abort();
    }

    size = a.commit_size;
  }
#endif
  a.beg = a.cur = buf;
  a.end = buf ? buf + size : 0;

  ASAN_POISON_MEMORY_REGION(buf, size);
  return a;
}

/**
 * @brief Release arena memory back to the OS.
 * @param arena Arena to release.
 *
 * Only frees memory the arena reserved itself; a caller-supplied buffer stays
 * the caller's to free. Invalidates all allocations; zeroes the struct.
 */
ARENA_INLINE void arena_release(Arena* arena) {
#ifdef OOM_COMMIT
  if (arena->commit_size) {
    munmap(arena->beg, arena->reserve_size);
  }
#endif
  memset(arena, 0, sizeof(Arena));
}

/**
 * @brief Reset arena to initial state.
 * @param arena Arena to reset.
 *
 * Invalidates all allocations but keeps the memory.
 */
ARENA_INLINE void arena_reset(Arena* arena) {
  ASAN_POISON_MEMORY_REGION(arena->beg, arena->end - arena->beg);
  arena->cur = arena->beg;
}

/**
 * @brief Register an OOM recovery point.
 * @param arena Arena to guard.
 * @param jmpbuf Recovery buffer; must outlive any allocation using it.
 * @return Non-zero after a `longjmp` from a failed allocation (one-shot: the
 * handler clears itself when OOM fires; `OOM_TRAP` builds trap instead).
 *
 *   if (ArenaOOM(arena, jmpbuf)) { fputs("OOM\n", stderr); exit(1); }
 */
#ifndef OOM_TRAP
// Internal helper that keeps setjmp as the complete controlling expression.
ARENA_INLINE jmp_buf* arena_oom_set(Arena* arena, jmp_buf* jmpbuf) {
  Assert(arena != NULL && "arena cannot be NULL");
  arena->oom = jmpbuf;
  return jmpbuf;
}
#define ArenaOOM(arena, jmpbuf)                       \
  ({                                                  \
    int _oom_result = 0;                              \
    if (setjmp(*arena_oom_set((arena), &(jmpbuf)))) { \
      (arena)->oom = NULL;                            \
      _oom_result = 1;                                \
    }                                                 \
    _oom_result;                                      \
  })
#else
#define ArenaOOM(arena, jmpbuf) ((void)jmpbuf, false)
#endif

/**
 * @brief Slow allocation path (fast path is inlined in `arena_alloc`).
 * @param arena Arena allocator.
 * @param size Size per element.
 * @param align Alignment requirement.
 * @param count Number of elements.
 * @param flags Allocation flags.
 * @return Pointer to allocated memory, or NULL/`longjmp` on OOM.
 *
 * Handles commit-on-demand growth when the fast path fails.
 */
static void* arena_alloc_grow(Arena* arena, isize size, isize align, isize count, ArenaFlag flags) {
  byte* current = arena->cur;
  isize avail = arena->end - current;
  isize pad = AlignPadPow2((uintptr_t)current, align);

  isize total_size;
  if (ARENA_UNLIKELY(__builtin_mul_overflow(size, count, &total_size))) {
    goto HANDLE_OOM;
  }

  if (total_size > avail - pad) {
#ifdef OOM_COMMIT
    if (arena->commit_size) {
      // Compute how much to commit in a single syscall
      isize needed = total_size + pad - avail;
      isize commit = AlignPow2(needed, arena->commit_size);
      isize committed = arena->end - arena->beg;

      if (commit > arena->reserve_size - committed) {
        goto HANDLE_OOM;
      }

      if (!arena_os_commit(arena->end, commit)) {
        perror("arena_alloc mprotect");
        goto HANDLE_OOM;
      }

      ASAN_POISON_MEMORY_REGION(arena->end, commit);
      arena->end += commit;
    } else
#endif
    {
      goto HANDLE_OOM;
    }
  }

  arena->cur += pad + total_size;
  current += pad;

  ASAN_UNPOISON_MEMORY_REGION(current, total_size);
  return flags.mask & _NO_INIT ? current : memset(current, 0, total_size);

HANDLE_OOM:
  if (flags.mask & _OOM_NULL)
    return NULL;
#ifdef OOM_TRAP
  Assert(!OOM_TRAP);
#else
  jmp_buf* oom = arena->oom;
  Assert(oom);
  longjmp(*oom, 1);
#endif
  return NULL;
}

/**
 * @brief Allocate aligned memory from arena.
 * @param arena Arena allocator.
 * @param size Size of each element.
 * @param align Alignment requirement (must be power of 2).
 * @param count Number of elements.
 * @param flags Allocation flags (`NO_INIT`, `OOM_NULL`).
 * @return Pointer to allocated memory (zeroed unless `NO_INIT`).
 */
ARENA_INLINE void* arena_alloc(Arena* arena, isize size, isize align, isize count, ArenaFlag flags) {
  Assert(size >= 0);
  Assert(count >= 0);
  Assert(IsPow2(align));
  Assert(arena->beg <= arena->cur && arena->cur <= arena->end && "corrupt arena");

  byte* current = arena->cur;
  isize avail = arena->end - current;
  isize pad = AlignPadPow2((uintptr_t)current, align);

  // Fast path: allocation fits in current committed region
  isize total_size;
  if (ARENA_LIKELY(!__builtin_mul_overflow(size, count, &total_size) && total_size <= avail - pad)) {
    arena->cur += pad + total_size;
    current += pad;
    ASAN_UNPOISON_MEMORY_REGION(current, total_size);
    return flags.mask & _NO_INIT ? current : memset(current, 0, total_size);
  }

  return arena_alloc_grow(arena, size, align, count, flags);
}

/**
 * @brief Allocate and initialize memory by copying from source.
 * @param arena Arena allocator.
 * @param size Size per element.
 * @param align Alignment requirement.
 * @param count Number of elements.
 * @param initptr Source data to copy from.
 * @return Pointer to allocated and initialized memory.
 */
ARENA_INLINE void* arena_alloc_init(Arena* arena, isize size, isize align, isize count, const void* initptr) {
  Assert(initptr != NULL && "initptr cannot be NULL");
  void* ptr = arena_alloc(arena, size, align, count, NO_INIT);
  memmove(ptr, initptr, size * count);  // initptr may alias ptr (e.g. escaped Scratch data)
  return ptr;
}

/**
 * @brief Reach `min_cap` capacity (existing `len` elements preserved).
 * @param arena Arena to allocate from.
 * @param slice Pointer to slice descriptor.
 * @param size Size per element.
 * @param align Alignment requirement.
 * @param min_cap Capacity to reach.
 *
 * No-op when `cap >= min_cap`; otherwise extend in place at the tip, else
 * allocate a fresh block and move. Slice is type-erased here; size/alignment
 * come from the caller's element type.
 */
// True when a slice's storage ends exactly at the arena's bump pointer, so its
// capacity can be extended without moving anything.
ARENA_INLINE bool arena_at_tip(const Arena* arena, const void* data, isize size, isize cap) {
  return (uintptr_t)data == (uintptr_t)arena->cur - size * cap;
}

ARENA_INLINE void arena_slice_reserve(Arena* arena, void* slice, isize size, isize align, isize min_cap) {
  SliceInternal tmp;
  memcpy(&tmp, slice, sizeof(tmp));

  Assert(min_cap >= 0 && "reserve size must be non-negative");
  Assert(size > 0 && "element size must be positive");
  Assert(align > 0 && IsPow2(align) && "element alignment must be a power of two");
  Assert(tmp.len >= 0 && tmp.len <= tmp.cap && "invalid slice state");
  Assert((tmp.data != NULL || tmp.len == 0) && "empty slices only may leave data NULL");
  Assert((tmp.data == NULL || ((uintptr_t)tmp.data & ((uintptr_t)align - 1)) == 0) &&
         "slice data must be aligned for its element type");

  if (tmp.cap >= min_cap)
    return;

  if (tmp.cap > 0 && ARENA_LIKELY(arena_at_tip(arena, tmp.data, size, tmp.cap))) {
    // At the arena tip: extend in place. align 1 keeps the extension adjacent.
    arena_alloc(arena, size, 1, min_cap - tmp.cap, NO_INIT);
  } else {
    // Move off-tip storage into a fresh block.
    void* ptr = arena_alloc(arena, size, align, min_cap, NO_INIT);
    tmp.data = tmp.len == 0 ? ptr : memmove(ptr, tmp.data, size * tmp.len);
  }
  tmp.cap = min_cap;

  memcpy(slice, &tmp, sizeof(tmp));
}

/**
 * @brief Grow a slice's capacity by one step of the growth policy.
 * @param arena Arena to allocate from.
 * @param slice Pointer to slice descriptor.
 * @param size Size per element.
 * @param align Alignment requirement.
 *
 * +16 at the tip, else `cap + max(cap/2, 16)`.
 */
ARENA_INLINE void arena_slice_grow(Arena* arena, void* slice, isize size, isize align) {
  SliceInternal tmp;
  memcpy(&tmp, slice, sizeof(tmp));

  enum { GROW = 16 };

  isize min_cap;
  if (tmp.cap > 0 && ARENA_LIKELY(arena_at_tip(arena, tmp.data, size, tmp.cap)))
    min_cap = tmp.cap + GROW;
  else
    min_cap = tmp.cap + Max(tmp.cap / 2, GROW);

  arena_slice_reserve(arena, slice, size, align, min_cap);
}

/**
 * @brief `malloc`-compatible allocation from arena.
 * @param size Bytes to allocate.
 * @param arena Arena to allocate from.
 * @return Pointer to allocated memory.
 */
ARENA_INLINE void* arena_malloc(size_t size, Arena* arena) {
  Assert(arena != NULL && "arena cannot be NULL");
  return arena_alloc(arena, size, alignof(max_align_t), 1, NO_INIT);
}

/**
 * @brief `free`-compatible deallocation.
 * @param ptr Pointer to free.
 * @param size Size of allocation.
 * @param arena Arena that allocated it.
 *
 * Only reclaims when `ptr` is the most recent allocation (at the tip).
 */
ARENA_INLINE void arena_free(void* ptr, size_t size, Arena* arena) {
  Assert(arena != NULL && "arena cannot be NULL");
  if (!ptr)
    return;

  // Can only free most recent allocation
  if ((uintptr_t)ptr == (uintptr_t)arena->cur - size) {
    ASAN_POISON_MEMORY_REGION(ptr, size);
    arena->cur = ptr;
  }
}

/** Length-prefixed string view. Not null-terminated; see `astr_to_cstr`. */
typedef struct astr {
  char* data;  // String data (may not be null-terminated)
  isize len;   // Length in bytes
} astr;

/** Make an `astr` from a literal: `astr("hello")`. */
#define astr(s) ((astr){(s), sizeof(s) - 1})

/** Printf args for an `astr`: `printf("%.*s\n", S(s))`. */
#define S(s) (int)(s).len, (s).data

// True when s's bytes end exactly at the arena's bump pointer (see arena_at_tip).
ARENA_INLINE bool _astr_at_tip(const Arena* arena, astr s) {
  return arena_at_tip(arena, s.data, 1, s.len);
}

/**
 * @brief Clone string into arena memory.
 * @param arena Arena to allocate in.
 * @param s String to clone.
 * @return New astr with copied data; no-op when empty or already at the tip.
 */
ARENA_INLINE astr astr_clone(Arena* arena, astr s) {
  if (s.len == 0 || _astr_at_tip(arena, s))
    return s;

  astr s2 = s;
  s2.data = New(arena, char, s.len, NO_INIT);
  memmove(s2.data, s.data, s.len);
  return s2;
}

/**
 * @brief Concatenate two strings in arena.
 * @param arena Arena to allocate in.
 * @param head First string.
 * @param tail Second string.
 * @return New astr with concatenated data.
 *
 * Extends in place when `tail` already sits right after `head` at the tip,
 * otherwise clones as needed.
 */
ARENA_INLINE astr astr_concat(Arena* arena, astr head, astr tail) {
  if (head.len == 0) {
    return tail.len > 0 && _astr_at_tip(arena, tail) ? tail : astr_clone(arena, tail);
  }

  if (tail.len > 0 && _astr_at_tip(arena, tail) && tail.data == head.data + head.len) {
    // head || tail already sits contiguously at the arena tip — extend in
    // place: no copies, no allocations, and the result stays at the tip.
    head.len += tail.len;
    return head;
  }

  astr result = astr_clone(arena, head);
  if (tail.len > 0 && _astr_at_tip(arena, tail)) {
    // tail overlaps head at arena tip — force copy to avoid no-op clone
    char* p = New(arena, char, tail.len, NO_INIT);
    memmove(p, tail.data, tail.len);
    result.len += tail.len;
  } else if (tail.len > 0) {
    result.len += astr_clone(arena, tail).len;
  }
  return result;
}

/**
 * @brief Create astr from raw bytes.
 * @param arena Arena to allocate in
 * @param bytes Pointer to bytes
 * @param nbytes Number of bytes
 * @return Arena-allocated astr
 */
ARENA_INLINE astr astr_from_bytes(Arena* arena, const void* bytes, size_t nbytes) {
  return astr_clone(arena, (astr){(char*)bytes, nbytes});
}

/**
 * @brief Create astr from null-terminated C string.
 * @param arena Arena to allocate in
 * @param str Null-terminated string
 * @return Arena-allocated astr
 */
ARENA_INLINE astr astr_from_cstr(Arena* arena, const char* str) {
  return astr_from_bytes(arena, str, strlen(str));
}

/**
 * @brief Concatenate string with raw bytes.
 * @param arena Arena to allocate in
 * @param head String prefix
 * @param bytes Bytes to append
 * @param nbytes Number of bytes
 * @return Concatenated astr
 */
ARENA_INLINE astr astr_cat_bytes(Arena* arena, astr head, const void* bytes, size_t nbytes) {
  return astr_concat(arena, head, (astr){(char*)bytes, nbytes});
}

/**
 * @brief Concatenate string with C string.
 * @param arena Arena to allocate in
 * @param head String prefix
 * @param str C string to append
 * @return Concatenated astr
 */
ARENA_INLINE astr astr_cat_cstr(Arena* arena, astr head, const char* str) {
  return astr_cat_bytes(arena, head, str, strlen(str));
}

/**
 * Format string using printf-style format.
 *
 * Returns arena-allocated formatted string. Results up to 255 bytes are
 * formatted once into a stack buffer and copied into the arena; longer
 * results take a second formatting pass into the arena.
 *
 * Usage:
 *   astr s1 = astr_format(arena, "%.10f, $%d, %.*s", 3.1415926, 42, S(s));
 */
static astr astr_format(Arena* arena, const char* format, ...) {
  enum { STACK_BUF = 256 };
  char buf[STACK_BUF];

  va_list args;
  va_start(args, format);
  int nbytes = vsnprintf(buf, sizeof buf, format, args);
  va_end(args);
  Assert(nbytes >= 0);

  char* data;
  if (nbytes < STACK_BUF) {
    data = New(arena, char, nbytes + 1, NO_INIT);
    memcpy(data, buf, nbytes);
    data[nbytes] = '\0';
  } else {
    data = New(arena, char, nbytes + 1, NO_INIT);
    va_start(args, format);
    int nbytes2 = vsnprintf(data, nbytes + 1, format, args);
    va_end(args);
    Assert(nbytes2 == nbytes);
  }
  arena->cur--;  // Drop null terminator so astr_concat can extend it in place.

  return (astr){.data = data, .len = nbytes};
}

/**
 * Convert astr to null-terminated C string.
 *
 * For temporary use - avoids the malloc/strdup + free dance. The arena is
 * passed by value so the caller's cur is not advanced: the result is only
 * valid until the next allocation from that arena, and two calls in one
 * expression land on the same address (only the last one survives).
 *
 * Usage:
 *   printf("test: %s\n", astr_to_cstr(*arena, s));
 */
ARENA_INLINE const char* astr_to_cstr(Arena arena, astr s) {
  return astr_concat(&arena, s, astr("\0")).data;
}

/**
 * Duplicate astr as malloc'd C string.
 *
 * Caller must free the returned pointer.
 *
 * Usage:
 *   __autofree char *cs = astr_cstrdup(s);
 *   for (char *p = cs; *p; ++p) {
 *     // modify cs...
 *   }
 */
ARENA_INLINE char* astr_cstrdup(astr s) {
  return strndup(s.data, s.len);
}

// Build a direct byte-membership table (nonzero = separator). Always treats
// \0 as separator.
//
// Returns the charset's byte when a single-byte memchr scan alone can find
// the token end: exactly one charset byte and no embedded \0 in s (which is a
// separator too). 0 otherwise, keeping the caller on the table path.
ARENA_INLINE unsigned char _astr_charset_table(const char* charset, astr s, unsigned char table[static 256]) {
  memset(table, 0, 256);
  for (const char* c = charset; *c; c++)
    table[(unsigned char)*c] = 1;
  table[0] = 1;  // \0 can never be in charset; always treat as separator

  if (charset[0] == '\0' || charset[1] != '\0')
    return 0;
  if (s.len > 0 && memchr(s.data, '\0', (size_t)s.len) != NULL)
    return 0;
  return (unsigned char)charset[0];
}

// Internal helper for astr_split_by_char (table must be pre-built).
//
// Leaves pos at the separator run following the token, with the next call's
// leading-separator skip consuming it: each byte is tested exactly once
// (previously the run after a token was rescanned as the next token's leading
// run). A one-byte, NUL-free input (single != 0) scans the token with a byte
// loop for its first 16 bytes and memchr for the rest: memchr's call overhead
// dominates under ~16 bytes (measured on comma-separated fields, ns/token:
// 1-12-byte fields 3.7 vs 7.4 always-memchr), while it wins from there on
// (40-80-byte fields 12.0 vs 7.8).
ARENA_INLINE astr _astr_split_by_char(astr s, const unsigned char table[static 256], unsigned char single,
                                      isize* pos) {
  isize i = *pos;

  if (single != 0) {
    while (i < s.len && (unsigned char)s.data[i] == single)
      i++;

    // Short tokens are the common case: test a prefix by hand, then let
    // memchr (vectorized, but with call overhead) do the long tail.
    isize start = i;
    isize stop = start + 16;
    if (stop > s.len)
      stop = s.len;
    while (i < stop && (unsigned char)s.data[i] != single)
      i++;
    if (i == stop && i < s.len) {
      const char* sep = memchr(s.data + i, single, (size_t)(s.len - i));
      i = sep ? (isize)(sep - s.data) : s.len;
    }

    *pos = i;
    return (astr){s.data + start, i - start};
  }

  // Skip the separator run left by the previous token (or leading the string)
  while (i < s.len && table[(unsigned char)s.data[i]])
    i++;

  isize start = i;

  // Scan for next separator
  while (i < s.len && !table[(unsigned char)s.data[i]])
    i++;

  *pos = i;
  return (astr){s.data + start, i - start};
}

/**
 * Split string by any character in charset.
 *
 * The membership table is built once, on the first iteration (table[0] is a
 * natural sentinel: always non-zero after build because \0 is always a
 * separator). A one-byte charset over NUL-free input scans each token with a
 * short byte loop then memchr, everything else walks the table.
 *
 * Usage:
 *   int i = 0;
 *   for (astr_split_by_char(it, ",| $", s3)) {
 *     printf("'%.*s'\n", S(it.token));
 *     i++;
 *   }
 */
#define astr_split_by_char(it, charset, str)                                            \
  struct {                                                                              \
    astr input, token;                                                                  \
    isize pos;                                                                          \
    unsigned char table[256];                                                           \
    unsigned char single;                                                               \
  } it = {.input = str};                                                                \
  (it.table[0] || (it.single = _astr_charset_table(charset, it.input, it.table), 1)) && \
      it.pos < it.input.len &&                                                          \
      (it.token = _astr_split_by_char(it.input, it.table, it.single, &it.pos)).len > 0;

// Search for a short separator: memchr up to the first-byte candidates and
// compare the rest. sep.len must be >= 2 and <= hay.len.
ARENA_INLINE const char* _astr_search(astr hay, astr sep) {
  Assert(sep.len >= 2 && hay.len >= sep.len);
  const char* p = hay.data;
  const char* last = hay.data + hay.len - sep.len;  // last possible start
  while (p <= last) {
    p = memchr(p, (unsigned char)sep.data[0], (size_t)(last - p) + 1);
    if (!p)
      return NULL;
    if (memcmp(p + 1, sep.data + 1, (size_t)(sep.len - 1)) == 0)
      return p;
    p++;
  }
  return NULL;
}

// Internal helper for astr_split
ARENA_INLINE astr _astr_split(astr s, astr sep, isize* pos) {
  astr slice = {s.data + *pos, s.len - *pos};
  if (sep.len == 0) {
    *pos = s.len;
    return slice;
  }
  const char* res;
  if (sep.len == 1)
    res = memchr(slice.data, (unsigned char)sep.data[0], (size_t)slice.len);
  else if (sep.len <= 4 && slice.len >= sep.len)
    res = _astr_search(slice, sep);
  else
    res = memmem(slice.data, (size_t)slice.len, sep.data, (size_t)sep.len);
  astr token = {slice.data, res ? (res - slice.data) : slice.len};
  *pos += token.len + sep.len;
  return token;
}

/**
 * Split string by multi-character separator.
 *
 * One-byte separators go straight to memchr; separators up to four bytes use
 * _astr_search (memchr to first-byte candidates, compare the rest); longer
 * ones fall back to memmem.
 *
 * Usage:
 *   for (astr_split(it, ",", s3)) {
 *     printf("|%.*s|\n", S(astr_trim(it.token)));
 *   }
 */
#define astr_split(it, strsep, str)                             \
  struct {                                                      \
    astr input, token, sep;                                     \
    isize pos;                                                  \
  } it = {.input = str, .sep = (astr){strsep, strlen(strsep)}}; \
  it.pos < it.input.len && (it.token = _astr_split(it.input, it.sep, &it.pos)).data;

/**
 * @brief Compare two strings for equality.
 * @param a First string
 * @param b Second string
 * @return true if equal, false otherwise
 */
ARENA_INLINE bool astr_equals(astr a, astr b) {
  if (a.len != b.len)
    return false;

  return !a.len || !memcmp(a.data, b.data, a.len);
}

/**
 * @brief Compare two strings lexicographically.
 * @param a First string
 * @param b Second string
 * @return <0 if a<b, 0 if equal, >0 if a>b
 */
ARENA_INLINE int astr_compare(astr a, astr b) {
  isize n = a.len < b.len ? a.len : b.len;
  int c = n ? memcmp(a.data, b.data, n) : 0;
  return c ? c : (a.len > b.len) - (a.len < b.len);
}

/**
 * @brief Check if string starts with prefix.
 * @param s String to check
 * @param prefix Prefix to test
 * @return true if s starts with prefix
 */
ARENA_INLINE bool astr_starts_with(astr s, astr prefix) {
  isize n = prefix.len;
  return n <= s.len && !memcmp(s.data, prefix.data, n);
}

/**
 * @brief Check if string ends with suffix.
 * @param s String to check
 * @param suffix Suffix to test
 * @return true if s ends with suffix
 */
ARENA_INLINE bool astr_ends_with(astr s, astr suffix) {
  isize n = suffix.len;
  return n <= s.len && !memcmp(s.data + s.len - n, suffix.data, n);
}

/**
 * @brief Check if string contains a substring.
 * @param s String to search
 * @param needle Substring to find
 * @return true if needle is found in s
 */
ARENA_INLINE bool astr_contains(astr s, astr needle) {
  return needle.len == 0 || memmem(s.data, s.len, needle.data, needle.len) != NULL;
}

/**
 * @brief Find first occurrence of substring.
 * @param s String to search
 * @param needle Substring to find
 * @return Position of first match, or -1 if not found
 */
ARENA_INLINE isize astr_find(astr s, astr needle) {
  if (needle.len == 0)
    return 0;
  const char* p = memmem(s.data, s.len, needle.data, needle.len);
  return p ? (isize)(p - s.data) : -1;
}

/**
 * @brief Extract substring starting at pos with length len.
 * @param s Source string
 * @param pos Start position
 * @param len Length (clamped to string bounds)
 * @return Substring view (no allocation)
 */
ARENA_INLINE astr astr_substr(astr s, isize pos, isize len) {
  Assert(((size_t)pos <= (size_t)s.len) & (len >= 0));
  if (len > s.len - pos)
    len = s.len - pos;
  s.data += pos, s.len = len;
  return s;
}

/**
 * @brief Extract substring from position p1 to p2 (exclusive).
 * @param s Source string
 * @param p1 Start position (inclusive)
 * @param p2 End position (exclusive, clamped to string length)
 * @return Substring view (no allocation)
 */
ARENA_INLINE astr astr_slice(astr s, isize p1, isize p2) {
  Assert(((size_t)p1 <= (size_t)p2) & ((size_t)p1 <= (size_t)s.len));
  if (p2 > s.len)
    p2 = s.len;
  s.data += p1, s.len = p2 - p1;
  return s;
}

/**
 * @brief Remove leading whitespace (ASCII <= ' ').
 * @param s String to trim
 * @return View of string without leading whitespace
 */
ARENA_INLINE astr astr_trim_left(astr s) {
  while (s.len && (unsigned char)*s.data <= ' ')
    ++s.data, --s.len;
  return s;
}

/**
 * @brief Remove trailing whitespace (ASCII <= ' ').
 * @param s String to trim
 * @return View of string without trailing whitespace
 */
ARENA_INLINE astr astr_trim_right(astr s) {
  while (s.len && (unsigned char)s.data[s.len - 1] <= ' ')
    --s.len;
  return s;
}

/**
 * @brief Remove leading and trailing whitespace.
 * @param sv String to trim
 * @return View of trimmed string
 */
ARENA_INLINE astr astr_trim(astr sv) {
  return astr_trim_right(astr_trim_left(sv));
}

/**
 * @brief Compute a 64-bit hash of a string.
 * @param key String to hash
 * @return 64-bit hash value
 *
 * Suitable for hash tables. A wyhash-style block hash: 8 bytes per step,
 * multiply-mixed, then a splitmix64-style finalizer. The tail (1-7 bytes) is
 * read with overlapping in-bounds loads, so there is no byte loop and no read
 * past the string.
 *
 * Why not FNV-1a: verstable takes the bucket from the low bits and the stored
 * fragment from the TOP FOUR bits (hash >> 48), so entropy must be uniform in
 * both extremes. FNV-1a's high bits are badly non-uniform for structured keys
 * (measured chi^2 over 2^16 cells for "key-<n>", n < 1e6: ~645000 for FNV vs
 * ~65500 ideal), which degrades the fragment's discriminative power.
 *
 * The result is endian-dependent (native 8-byte loads), as is verstable's own
 * default Wyhash. Not keyed, so not DoS-resistant against adversarial keys.
 */
ARENA_INLINE uint64_t astr_hash(astr key) {
  const byte* p = (const byte*)key.data;
  size_t n = (size_t)key.len;
  uint64_t h = 0x9E3779B97F4A7C15ull ^ ((uint64_t)n * 0x9E3779B97F4A7C15ull);

  while (n >= 8) {
    uint64_t k;
    memcpy(&k, p, 8);
    k *= 0xBF58476D1CE4E5B9ull;
    k ^= k >> 27;
    k *= 0x94D049BB133111EBull;
    h ^= k;
    h *= 0xBF58476D1CE4E5B9ull;
    p += 8;
    n -= 8;
  }

  if (n) {
    uint64_t tail;
    if (n >= 4) {
      uint32_t a, b;
      memcpy(&a, p, 4);
      memcpy(&b, p + n - 4, 4);  // overlaps, stays inside [p, p+n)
      tail = (uint64_t)a | ((uint64_t)b << 32);
    } else {
      byte a = p[0], b = p[n - 1], c = p[n >> 1];
      tail = (uint64_t)a | ((uint64_t)b << 8) | ((uint64_t)c << 16);
    }
    h ^= tail;
    h *= 0xD6E8FEB86659FD93ull;
  }

  h ^= h >> 33;
  h *= 0xFF51AFD7ED558CCDull;
  h ^= h >> 33;
  return h;
}

/**
 * Hash table integration example:
 *
 * @code
 * static inline void *vt_arena_malloc(size_t size, Arena **ctx) {
 *   return arena_malloc(size, *ctx);
 * }
 *
 * static inline void vt_arena_free(void *ptr, size_t size, Arena **ctx) {
 *   arena_free(ptr, size, *ctx);
 * }
 *
 * #define NAME      Map_astr_astr
 * #define KEY_TY    astr
 * #define VAL_TY    astr
 * #define CTX_TY    Arena *
 * #define CMPR_FN   astr_equals
 * #define HASH_FN   astr_hash
 * #define MALLOC_FN vt_arena_malloc
 * #define FREE_FN   vt_arena_free
 * #include "verstable.h"
 * @endcode
 */

#endif  // ARENA_H_
