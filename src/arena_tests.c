#include "arena.h"
#include "debug.h"
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

typedef Slice(int) ints;

// A second expansion of the same element type: a distinct anonymous type that
// coexists with `ints` in this translation unit.
typedef Slice(int) ints_again;

struct Point {
  int x, y;
};
typedef Slice(unsigned int) uints;
typedef Slice(struct Point) points;
typedef Slice(const char*) strs;

UTEST(slice, clone_full) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints s = {0};
  for (int i = 0; i < 5; i++)
    Push(arena, &s, i * 10);

  ints copy = Clone(arena, s);
  ASSERT_EQ(copy.len, 5);
  for (int i = 0; i < 5; i++)
    ASSERT_EQ(Get(&copy, i), i * 10);
  ASSERT_TRUE(copy.data != s.data);
}

UTEST(slice, clone_subslice) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints s = {0};
  for (int i = 0; i < 10; i++)
    Push(arena, &s, i);

  ints mid = Clone(arena, s, 3, 4);
  ASSERT_EQ(mid.len, 4);
  ASSERT_EQ(Get(&mid, 0), 3);
  ASSERT_EQ(Get(&mid, 1), 4);
  ASSERT_EQ(Get(&mid, 2), 5);
  ASSERT_EQ(Get(&mid, 3), 6);
}

UTEST(slice, push_grow_many) {
  enum { size = KB(16) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints s = {0};
  for (int i = 0; i < 200; i++)
    Push(arena, &s, i);

  ASSERT_EQ(s.len, 200);
  ASSERT_TRUE(s.cap >= 200);
  for (int i = 0; i < 200; i++)
    ASSERT_EQ(Get(&s, i), i);

  // Push returns the new element's address
  int* p = Push(arena, &s, 999);
  ASSERT_EQ(*p, 999);
  ASSERT_TRUE(p == &Get(&s, 200));
  ASSERT_EQ(s.len, 201);
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

UTEST(slice, foreign_buffer_is_valid_slice) {
  // cap == 0 means "backed by calling code, not the arena": len may exceed cap,
  // and both Push and Clone must accept it -- adopting the data into the arena
  // rather than writing through the caller's pointer.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  int stack_data[] = {2, 3, 42};
  ints s = {.data = stack_data, .len = Countof(stack_data)};

  ints copy = Clone(arena, s);
  ASSERT_EQ(copy.len, 3);
  ASSERT_EQ(Get(&copy, 2), 42);

  Push(arena, &s, 4);
  ASSERT_EQ(s.len, 4);
  ASSERT_EQ(Get(&s, 0), 2);
  ASSERT_EQ(Get(&s, 3), 4);
  ASSERT_TRUE(s.data != stack_data);  // adopted into the arena
  ASSERT_EQ(stack_data[2], 42);       // caller's buffer untouched
}

UTEST(slice, clone_evaluates_args_once) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints s = {0};
  for (int i = 0; i < 5; i++)
    Push(arena, &s, i * 10);

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
  ASSERT_EQ(Get(&rest, 0), 20);
}

/* --- Slice: typed data, Push, Get --- */

UTEST(slice, multi_token_types) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  // Multi-token element types: struct, unsigned, pointer. Slice(T) spells
  // them as written, no intermediate typedef needed.
  points ps = {0};
  Push(arena, &ps, ((struct Point){1, 2}));
  Push(arena, &ps, ((struct Point){3, 4}));
  ASSERT_EQ(ps.len, 2);
  ASSERT_EQ(Get(&ps, 0).x, 1);
  ASSERT_EQ(Get(&ps, 0).y, 2);
  ASSERT_EQ(Get(&ps, 1).x, 3);
  ASSERT_EQ(Get(&ps, 1).y, 4);

  uints us = {0};
  Push(arena, &us, 7u);
  ASSERT_EQ(Get(&us, 0), 7u);

  const char* hello = "hello";
  strs ss = {0};
  Push(arena, &ss, hello);
  ASSERT_EQ(ss.len, 1);
  ASSERT_TRUE(strcmp(Get(&ss, 0), "hello") == 0);
}

UTEST(slice, duplicate_expansion) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  // ints and ints_again are the same element type expanded twice at file
  // scope; each expansion is usable on its own.
  ints a = {0};
  ints_again b = {0};
  Push(arena, &a, 1);
  Push(arena, &b, 2);
  ASSERT_EQ(a.len, 1);
  ASSERT_EQ(b.len, 1);
  ASSERT_EQ(Get(&a, 0), 1);
  ASSERT_EQ(Get(&b, 0), 2);
}

UTEST(slice, push_value_struct) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  points ps = {0};
  struct Point p = {5, 6};
  Push(arena, &ps, p);

  ASSERT_EQ(ps.len, 1);
  ASSERT_EQ(Get(&ps, 0).x, 5);
  ASSERT_EQ(Get(&ps, 0).y, 6);
}

/* --- Slice: streaming into a new element --- */
//
// Push takes a value, so an element whose contents are *produced* -- read(2),
// inflate, decode, a struct filled field by field -- is pushed as a placeholder
// and then filled through the pointer Push returns:
//
//   Chunk* c = Push(arena, &cs, (Chunk){0});
//   c->n = read(fd, c->data, sizeof c->data);
//
// Two rules: finish filling before the next Push, and after any growth reach
// the live element by index (Get) rather than through a held pointer.

// An element whose contents are produced rather than known up front: a fixed
// inline buffer plus the number of bytes in use.
typedef struct {
  char data[64];
  int n;
} Chunk;
typedef Slice(Chunk) chunks;

// Stands in for a stream producer (read(2), inflate, decode): writes into dst
// and reports how many bytes it produced.
static int stream_fill(char* dst, int cap) {
  int n = Min(cap, 16);
  for (int i = 0; i < n; i++)
    dst[i] = (char)('a' + i);
  return n;
}

UTEST(slice, build_element_in_place) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  chunks cs = {0};

  // Placeholder, then stream into it: three successive fills, as a decoder
  // would, each starting where the last stopped.
  Chunk* c = Push(arena, &cs, (Chunk){0});
  for (int i = 0; i < 3; i++)
    c->n += stream_fill(c->data + c->n, (int)sizeof c->data - c->n);

  ASSERT_EQ(cs.len, 1);
  ASSERT_EQ(Get(&cs, 0).n, 48);
  ASSERT_EQ(Get(&cs, 0).data[0], 'a');
  ASSERT_EQ(Get(&cs, 0).data[15], 'a' + 15);
  ASSERT_EQ(Get(&cs, 0).data[16], 'a');  // second fill started over
  ASSERT_EQ(Get(&cs, 0).data[47], 'a' + 15);
}

UTEST(slice, fill_survives_grow) {
  enum { size = KB(32) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  chunks cs = {0};

  Chunk* c = Push(arena, &cs, (Chunk){0});
  c->n = 42;
  memcpy(c->data, "filled before growth", 21);

  // Push the slice off the arena tip, so the growth below cannot extend in
  // place and has to memmove the elements to fresh storage.
  New(arena, int, 64);
  for (int i = 0; i < 100; i++)
    Push(arena, &cs, (Chunk){0});

  ASSERT_EQ(cs.len, 101);
  ASSERT_EQ(Get(&cs, 0).n, 42);  // the fill rode along with the move
  ASSERT_TRUE(strcmp(Get(&cs, 0).data, "filled before growth") == 0);
  ASSERT_TRUE(c != &Get(&cs, 0));  // ...while c still names the pre-move copy

  // From here on the live element is reached by index.
  Get(&cs, 0).data[0] = 'F';
  ASSERT_EQ(Get(&cs, 0).data[0], 'F');
}

UTEST(slice, get_lvalue) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints s = {0};
  Push(arena, &s, 1);

  Get(&s, 0) = 42;
  ASSERT_EQ(Get(&s, 0), 42);

  Get(&s, 0) += 1;
  ASSERT_EQ(Get(&s, 0), 43);

  int* addr = &Get(&s, 0);
  ASSERT_TRUE(addr == (int*)s.data);
}

UTEST(slice, data_is_typed) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints s = {0};
  Push(arena, &s, 7);

  // data is T*, so elements are reachable without a macro or a cast, and it
  // binds to T* directly. Writes to the field are checked too
  // (-Wincompatible-pointer-types) -- see
  // test/typecheck/fail/slice_data_wrong_pointer_type.c.check.
  s.data[0] += 1;
  ASSERT_EQ(s.data[0], 8);
  int* p = s.data;
  ASSERT_TRUE(p == &Get(&s, 0));
}

UTEST(slice, layout_zero_overhead) {
  // Slice(T) must stay byte-identical to the erased layout arena_slice_grow
  // memcpys, whatever the element type.
  _Static_assert(sizeof(ints) == sizeof(SliceInternal), "slice size");
  _Static_assert(alignof(ints) == alignof(SliceInternal), "slice align");
  _Static_assert(offsetof(ints, data) == offsetof(SliceInternal, data), "data offset");
  _Static_assert(offsetof(ints, len) == offsetof(SliceInternal, len), "len offset");
  _Static_assert(offsetof(ints, cap) == offsetof(SliceInternal, cap), "cap offset");
  _Static_assert(sizeof(ints) == sizeof(void*) + 2 * sizeof(isize), "three words");
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
