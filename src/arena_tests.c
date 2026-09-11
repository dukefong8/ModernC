#include "arena.h"
#include "utest.h"

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
    if (p[i] != 0) non_zero++;
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

  {
    Scratch(arena);
    New(arena, char, 512);
    // arena->cur advanced inside scratch
  }
  // After scope exit, original arena's cur is unchanged
  ASSERT_EQ(arena->cur, saved);
}

UTEST(arena, scratch_nested) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  byte* level0 = arena->cur;
  {
    Scratch(arena);
    New(arena, char, 100);
    byte* level1 = arena->cur;
    {
      Scratch(arena);
      New(arena, char, 200);
    }
    ASSERT_EQ(arena->cur, level1);
  }
  ASSERT_EQ(arena->cur, level0);
}

UTEST(arena, scratch_repairs_poison_above_stale_end) {
#if defined(ASAN_ENABLED) && defined(OOM_COMMIT)
  // The Scratch's committed growth dies with the discarded copy, so the outer
  // arena's `end` is stale; the whole range must be poisoned again on exit.
  Arena arena[] = {arena_init(NULL, GB(1))};
  char* p;
  {
    Scratch(arena);
    New(arena, char, arena->end - arena->cur, NO_INIT);  // fill the initial commit
    p = New(arena, char, 1, NO_INIT);                    // growth -> past the stale end
  }
  ASSERT_TRUE(__asan_address_is_poisoned(p));
  arena_release(arena);
#endif
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

typedef struct { double x, y; } Vec2;

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

  typedef struct { int a; int b; } Pair;
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

/* --- Slice: Push / Clone --- */

typedef slice(int) ints;

UTEST(slice, clone_full) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints s = {0};
  for (int i = 0; i < 5; i++)
    *Push(arena, &s) = i * 10;

  ints copy = Clone(arena, s);
  ASSERT_EQ(copy.len, 5);
  for (int i = 0; i < 5; i++)
    ASSERT_EQ(copy.data[i], i * 10);
  ASSERT_TRUE(copy.data != s.data);
}

UTEST(slice, clone_subslice) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints s = {0};
  for (int i = 0; i < 10; i++)
    *Push(arena, &s) = i;

  ints mid = Clone(arena, s, 3, 4);
  ASSERT_EQ(mid.len, 4);
  ASSERT_EQ(mid.data[0], 3);
  ASSERT_EQ(mid.data[1], 4);
  ASSERT_EQ(mid.data[2], 5);
  ASSERT_EQ(mid.data[3], 6);
}

UTEST(slice, push_grow_many) {
  enum { size = KB(16) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints s = {0};
  for (int i = 0; i < 200; i++)
    *Push(arena, &s) = i;

  ASSERT_EQ(s.len, 200);
  ASSERT_TRUE(s.cap >= 200);
  for (int i = 0; i < 200; i++)
    ASSERT_EQ(s.data[i], i);
}

UTEST(slice, clone_empty) {
  enum { size = KB(1) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints s = {0};
  ints copy = Clone(arena, s);
  ASSERT_EQ(copy.len, 0);
  ASSERT_TRUE(copy.data == NULL);
}

UTEST(slice, clone_evaluates_args_once) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints s = {0};
  for (int i = 0; i < 5; i++)
    *Push(arena, &s) = i * 10;

  int calls = 0;
  ints full = Clone(arena, (calls++, s));
  ASSERT_EQ(calls, 1);
  ASSERT_EQ(full.len, 5);

  calls = 0;
  int start = 2;
  ints rest = Clone(arena, (calls++, s), start++);
  ASSERT_EQ(calls, 1);
  ASSERT_EQ(start, 3);
  ASSERT_EQ(rest.len, 3);
  ASSERT_EQ(rest.data[0], 20);
}

/* --- Countof / size macros --- */

UTEST(arena, size_macros) {
  ASSERT_EQ(KB(1), (size_t)1024);
  ASSERT_EQ(MB(1), (size_t)1024 * 1024);
  ASSERT_EQ(GB(1), (size_t)1024 * 1024 * 1024);
}

UTEST(arena, countof) {
  int arr[7];
  ASSERT_EQ(Countof(arr), 7);
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
