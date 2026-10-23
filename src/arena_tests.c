#include "arena.h"
#include "cspan.h"
#include "debug.h"
#include "utest.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

/* --- Arena init / alloc / reset / release --- */

UTEST(arena, init_static_buf) {
  enum { size = KB(4) };
  byte mem[size];
  Arena a = arena_init(mem, size);
  ASSERT_EQ(a.beg, mem);
  ASSERT_EQ(a.cur, mem);
  ASSERT_EQ(a.end, mem + size);
}

UTEST(arena, alloc_zero_init) {
  enum { size = KB(4) };
  byte mem[size];
  memset(mem, 0xFF, size);
  Arena arena[] = {arena_init(mem, size)};

  int* p = New(arena, int, 4);
  for (int i = 0; i < 4; i++) {
    ASSERT_EQ(p[i], 0);
  }
}

UTEST(arena, alloc_no_init) {
  enum { size = KB(4) };
  byte mem[size];
  memset(mem, 0xAB, size);
  Arena arena[] = {arena_init(mem, size)};

  // NO_INIT should skip zeroing — memory retains previous content
  byte* p = New(arena, byte, 16, NO_INIT);
  int non_zero = 0;
  for (int i = 0; i < 16; i++) {
    if (p[i] != 0)
      non_zero++;
  }
  ASSERT_TRUE(non_zero > 0);
}

UTEST(arena, alloc_copy_init) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  int src[] = {10, 20, 30};
  int* dst = New(arena, int, 3, src);
  ASSERT_EQ(dst[0], 10);
  ASSERT_EQ(dst[1], 20);
  ASSERT_EQ(dst[2], 30);
  ASSERT_TRUE(dst != src);
}

UTEST(arena, alloc_alignment) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  // Allocate a char to misalign, then allocate aligned types
  New(arena, char);

  double* d = New(arena, double);
  ASSERT_EQ((uintptr_t)d % _Alignof(double), (uintptr_t)0);

  New(arena, char);
  int64_t* q = New(arena, int64_t);
  ASSERT_EQ((uintptr_t)q % _Alignof(int64_t), (uintptr_t)0);
}

UTEST(arena, bump_advances_cur) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  byte* before = arena->cur;
  New(arena, int, 10);
  byte* after = arena->cur;
  ASSERT_TRUE(after >= before + 10 * sizeof(int));
}

UTEST(arena, reset_restores_cur) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  New(arena, char, 100);
  ASSERT_TRUE(arena->cur > arena->beg);

  arena_reset(arena);
  ASSERT_EQ(arena->cur, arena->beg);
}

UTEST(arena, reset_allows_reuse) {
  enum { size = KB(1) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  int* a = New(arena, int, 64);
  a[0] = 42;

  arena_reset(arena);
  int* b = New(arena, int, 64);
  // After reset, allocation starts from the beginning again
  ASSERT_EQ(a, b);
}

/* --- Scratch (scoped temporary allocation) --- */

UTEST(arena, scratch_restores_on_exit) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  New(arena, int, 10);
  byte* saved = arena->cur;
  isize saved_size = arena->end - arena->beg;

  ALOG(arena);
  {
    Scratch(arena);
    New(arena, char, 512);
    // arena->cur advanced inside scratch
    ALOG(arena);
    ASSERT_GT(arena->cur, saved);
  }
  ALOG(arena);
  // After scope exit, original arena's cur and size are unchanged
  ASSERT_EQ(arena->cur, saved);
  ASSERT_EQ(arena->end - arena->beg, saved_size);
}

UTEST(arena, scratch_nested) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  byte* level0 = arena->cur;
  isize size0 = arena->end - arena->beg;
  ALOG(arena);
  {
    Scratch(arena);
    New(arena, char, 100);
    byte* level1 = arena->cur;
    ALOG(arena);
    {
      Scratch(arena);
      New(arena, char, 200);
      ALOG(arena);
      ASSERT_GT(arena->cur, level1);
    }
    ALOG(arena);
    ASSERT_EQ(arena->cur, level1);
  }
  ALOG(arena);
  ASSERT_EQ(arena->cur, level0);
  ASSERT_EQ(arena->end - arena->beg, size0);
}

typedef Slice(int) ints;

/* Outer and scratch cursors share one region: an outer write through an alias
 * consumes the earliest scratch slots, so the alias is used last in the block
 * while the outer result stays intact. */
UTEST(arena, scratch_outer_alias_clone) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  Arena* out = arena;  // alias to the outer arena, bound before the shadow
  ints* kept = New(out, ints);
  {
    Scratch(arena);  // `arena` is a temporary copy here
    ints* tmp = New(arena, ints);
    Push(arena, tmp, (int)42);

    ints* extra = New(arena, ints);  // more scratch work before the outer write
    Push(arena, extra, (int)7);
    ASSERT_EQ(extra->data[0], 7);

    Clone(out, kept, tmp);  // last use of `out` in the block
  }

  ASSERT_EQ(kept->len, 1);
  ASSERT_EQ(kept->cap, 1);
  ASSERT_EQ(kept->data[0], 42);
  ASSERT_GT(arena->cur, arena->beg);
}

/* The clone destination starts at the scratch base, inside the source data.
 * arena_alloc_init copies with memmove, so the overlap is safe. */
UTEST(arena, scratch_overlapping_clone) {
  enum { size = KB(8) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  Arena* out = arena;
  ints* big = New(out, ints);
  {
    Scratch(arena);
    ints* tmp = New(arena, ints);
    for (int i = 0; i < 64; i++)
      Push(arena, tmp, (int)i);

    int* src = tmp->data;
    Clone(out, big, tmp);
    // Destination lies before the source and overlaps it.
    ASSERT_TRUE((byte*)big->data < (byte*)src);
    ASSERT_TRUE((byte*)big->data + 64 * sizeof(int) > (byte*)src);
  }

  ASSERT_EQ(big->len, 64);
  for (isize i = 0; i < 64; i++)
    ASSERT_EQ(big->data[i], (int)i);
}

/* Documented limit: an outer allocation larger than the scratch's advance puts
 * the outer cursor past the scratch cursor, so later scratch allocations land
 * inside the outer object. */
UTEST(arena, scratch_outer_past_scratch_advance) {
  enum { size = KB(8) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  Arena* out = arena;
  ints* kept = New(out, ints);
  int changed = 0;
  {
    Scratch(arena);
    ints* tmp = New(arena, ints);
    Push(arena, tmp, (int)1);
    Clone(out, kept, tmp);          // outer cursor: base + 4
    int* wide = New(out, int, 64);  // outer cursor: base + 260
    for (int i = 0; i < 64; i++)
      wide[i] = 0xAB;
    ints* next = New(arena, ints);  // scratch cursor: base + 28, inside `wide`
    Push(arena, next, (int)9);
    for (int i = 0; i < 64; i++)
      changed += wide[i] != 0xAB;
  }
  ASSERT_GT(changed, 0);
}

/* The outer write consumes the scratch prefix, including the clone source's
 * descriptor: that object must not be used again (e.g. as a second source). */
UTEST(arena, scratch_source_consumed_by_outer_write) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  Arena* out = arena;
  ints* kept = New(out, ints);
  {
    Scratch(arena);
    ints* tmp = New(arena, ints);
    Push(arena, tmp, (int)5);

    unsigned char before[sizeof(ints)];
    memcpy(before, tmp, sizeof before);
    Clone(out, kept, tmp);
    ASSERT_NE(memcmp(before, tmp, sizeof before), 0);
  }

  ASSERT_EQ(kept->data[0], 5);
}

/* --- OOM handling --- */

UTEST(arena, oom_null_returns_null) {
  enum { size = 64 };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  void* p = New(arena, char, size * 2, OOM_NULL);
  ASSERT_TRUE(p == NULL);
}

UTEST(arena, oom_longjmp) {
  enum { size = 64 };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  jmp_buf jmpbuf;
  int oom_hit = 0;
  if (ArenaOOM(arena, jmpbuf)) {
    oom_hit = 1;
  }

  if (!oom_hit) {
    // This should trigger OOM → longjmp
    New(arena, char, size * 2);
  }
  ASSERT_EQ(oom_hit, 1);
}

UTEST(arena, oom_longjmp_clears_handler) {
  enum { size = 64 };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  jmp_buf jmpbuf;
  if (ArenaOOM(arena, jmpbuf)) {
    ASSERT_TRUE(arena->oom == NULL);
    return;
  }

  New(arena, char, size * 2);
  ASSERT_TRUE(false);
}

UTEST(arena, oom_null_still_works_after_full) {
  enum { size = 128 };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  // Fill most of the arena
  New(arena, char, size - 32);

  // OOM_NULL should return NULL, not crash
  void* p = New(arena, char, 64, OOM_NULL);
  ASSERT_TRUE(p == NULL);

  // Small allocation should still succeed
  void* q = New(arena, char, 1);
  ASSERT_TRUE(q != NULL);
}

/* --- New macro variants --- */

UTEST(arena, new_single) {
  enum { size = KB(1) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  int* p = New(arena, int);
  ASSERT_TRUE(p != NULL);
  ASSERT_EQ(*p, 0);
}

UTEST(arena, new_array) {
  enum { size = KB(1) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  int* arr = New(arena, int, 5);
  for (int i = 0; i < 5; i++) {
    ASSERT_EQ(arr[i], 0);
    arr[i] = i * 10;
  }
  ASSERT_EQ(arr[3], 30);
}

typedef struct {
  double x, y;
} Vec2;

UTEST(arena, new_struct) {
  enum { size = KB(1) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  Vec2* p = New(arena, Vec2);
  ASSERT_EQ(p->x, 0.0);
  ASSERT_EQ(p->y, 0.0);
  p->x = 3.14;
  ASSERT_EQ(p->x, 3.14);
}

UTEST(arena, new_copy_struct) {
  enum { size = KB(1) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  typedef struct {
    int a;
    int b;
  } Pair;
  Pair src = {.a = 42, .b = 99};
  Pair* dst = New(arena, Pair, 1, &src);
  ASSERT_EQ(dst->a, 42);
  ASSERT_EQ(dst->b, 99);
  ASSERT_TRUE(dst != &src);
}

/* --- arena_malloc / arena_free --- */

UTEST(arena, malloc_free_at_tip) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  void* p = arena_malloc(64, arena);
  ASSERT_TRUE(p != NULL);
  byte* cur_after = arena->cur;

  // Free at tip should reclaim space
  arena_free(p, 64, arena);
  ASSERT_EQ(arena->cur, cur_after - 64);
}

UTEST(arena, free_not_at_tip_is_noop) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  void* p = arena_malloc(64, arena);
  arena_malloc(64, arena);  // push p away from tip
  byte* cur_before = arena->cur;

  // Free of non-tip pointer should be a no-op
  arena_free(p, 64, arena);
  ASSERT_EQ(arena->cur, cur_before);
}

/* --- c_countof / size macros --- */

UTEST(arena, size_macros) {
  ASSERT_EQ(KB(1), (size_t)1024);
  ASSERT_EQ(MB(1), (size_t)1024 * 1024);
  ASSERT_EQ(GB(1), (size_t)1024 * 1024 * 1024);
}

UTEST(arena, countof) {
  int arr[7];
  ASSERT_EQ(c_countof(arr), 7);
}

/* --- Multiple allocations are contiguous (bump property) --- */

UTEST(arena, allocations_contiguous) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  // Same-type sequential allocations should be contiguous
  int* a = New(arena, int, 4);
  int* b = New(arena, int, 4);
  ASSERT_EQ(a + 4, b);
}

/* --- Large allocation fills arena --- */

UTEST(arena, fill_to_capacity) {
  enum { size = 256 };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  // Allocate nearly all space
  byte* p = New(arena, byte, size - 8);
  ASSERT_TRUE(p != NULL);

  // Arena should be nearly full
  ASSERT_TRUE(arena->end - arena->cur < 16);
}

/* --- arena_release ownership --- */

UTEST(arena, release_borrowed_buffer) {
  // release must only free what the arena obtained itself; a caller-owned
  // buffer (stack, static, mmap'd) is never freed here.
  enum { size = KB(1) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  New(arena, int, 4);
  arena_release(arena);

  ASSERT_TRUE(arena->beg == NULL);
  ASSERT_TRUE(arena->cur == NULL);
  ASSERT_TRUE(arena->end == NULL);
}
