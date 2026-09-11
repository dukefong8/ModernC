#if defined(LOGGING) && defined(__linux__)
#define UPRINTF_IMPLEMENTATION
#include "uprintf.h"
#endif

#include <stdio.h>
#include <sys/mman.h>
#include <sys/types.h>
#include "arena.h"
#include "debug.h"
#include "utest.h"
UTEST_STATE();

/* --- Default Thread-Local Arena --- */

#ifndef DEFAULT_ARENA_SIZE
#define DEFAULT_ARENA_SIZE GB(64)
#endif

// Thread-local default arena
__thread Arena* default_arena = NULL;

#ifndef OOM_COMMIT
__thread void* default_arena_mem = NULL;
#endif

/**
 * Get the default arena, initializing it on first use.
 *
 * Usage:
 *   Arena *a = arena_default();
 *   char *s = New(a, char, 100);
 */
static inline Arena* arena_default(void) {
  if (default_arena) {
    return default_arena;
  }

#ifdef OOM_COMMIT
  static __thread Arena storage = {0};
  storage = arena_init(NULL, DEFAULT_ARENA_SIZE);
  default_arena = &storage;
#else
  default_arena_mem = malloc(DEFAULT_ARENA_SIZE);
  if (!default_arena_mem) {
    perror("arena_default malloc");
    abort();
  }

  static __thread Arena storage = {0};
  storage = arena_init(default_arena_mem, DEFAULT_ARENA_SIZE);
  default_arena = &storage;
#endif

  return default_arena;
}

/**
 * Reset the default arena to reclaim all memory.
 * Call this when you're done with temporary allocations.
 *
 * Usage:
 *   Arena *a = arena_default();
 *   // ... do work ...
 *   arena_default_reset();
 */
static inline void arena_default_reset(void) {
  if (default_arena) {
    arena_reset(default_arena);
  }
}

/**
 * Save the current state of the default arena.
 * Returns a snapshot that can be restored later.
 *
 * Usage:
 *   Arena snapshot = arena_default_snapshot();
 *   // ... temporary allocations ...
 *   arena_default_restore(snapshot);
 */
static inline Arena arena_default_snapshot(void) {
  Arena* a = arena_default();
  return *a;
}

/**
 * Restore the default arena to a previous snapshot.
 * Frees all allocations made after the snapshot.
 *
 * Usage:
 *   Arena snap = arena_default_snapshot();
 *   char *temp = New(arena_default(), char, 100);
 *   arena_default_restore(snap);  // temp is now invalid
 */
static inline void arena_default_restore(Arena snapshot) {
  if (default_arena) {
    ASAN_POISON_MEMORY_REGION(snapshot.cur, default_arena->end - snapshot.cur);
    default_arena->cur = snapshot.cur;
  }
}

astr test_astr(Arena arena[static 1]) {
  {
    // Scratch(arena);

    ALOG(arena);
    astr s3 = {0};

    ALOG(arena);

    astr s = {0};
    s = astr_clone(arena, s);
    s = astr_clone(arena, astr(""));
    s = astr_cat_cstr(arena, s, "hello");
    astr s1 = astr_format(arena, "%.10f, $%d, %.*s", 3.1415926, 42, S(s));
    printf("test_astr: %.*s\n", S(s1));
    char buf[] = ", world, \0!!!";
    s3 = astr_cat_bytes(arena, s1, buf, sizeof(buf));
    printf("test_astr: %.*s\n", S(s3));

    printf("test_astr: %s\n", astr_to_cstr(*arena, s3));
    int i = 0;
    for (astr_split(it, ",", s3)) {
      printf("|%.*s|\n", S(astr_trim(it.token)));
      i++;
    }
    printf("num of token=%d\n", i);

    i = 0;
    for (astr_split_by_char(it, ",| $", s3)) {
      printf("'%s'\n", astr_to_cstr(*arena, astr_slice(it.token, 0, 10)));
      i++;
    }
    printf("num of token=%d\n", i);

    ALOG(arena);
    return s3;
  }
}

typedef slice(int64_t) i64s;

i64s test_slice(Arena* arena) {
  ALOG(arena);
  {
    Scratch(arena);
    ALOG(arena);
    while (1) {
      // NO_INIT: p is never read, and fresh anonymous pages are already zero
      char* p = New(arena, char, GB(1), (ArenaFlag){_NO_INIT | _OOM_NULL});
      if (!p) {
        puts("!!! OOM break !!!");
        break;
      }
    }
    ALOG(arena);
  }
  ALOG(arena);

  int64_t data[] = {2, 3, 42};
  i64s fibs = {.data = data, .len = Countof(data)};
  fibs = Clone(arena, fibs, 0, 2);
  for (int i = 2; i < 9; ++i) {
    *Push(arena, &fibs) = fibs.data[i - 2] + fibs.data[i - 1];
  }
  ALOG(arena);
  {
    Scratch(arena);
    ALOG(arena);
    for (int i = 9; i < 11; ++i) {
      *Push(arena, &fibs) = fibs.data[i - 2] + fibs.data[i - 1];
    }
    ALOG(arena);
  }
  ALOG(arena);
  for (int i = 11; i < 29; ++i) {
    *Push(arena, &fibs) = fibs.data[i - 2] + fibs.data[i - 1];
  }
  ALOG(arena);

  return fibs;
}

#include "json.h"
void test_json() {

  char json_str[] = "{\"name\":{\"first\":\"Janet\",\"last\":\"Prichard\"},\"age\":47}";

  // Get the last name by its path.
  // A path is a series of keys separated by a dot.
  struct json value = json_get(json_str, "name.last");
  char last_name[64];
  json_string_copy(value, last_name, sizeof(last_name));

  // Get the age as an integer.
  int64_t age = json_int64(json_get(json_str, "age"));

  printf("%s %lld\n", last_name, (long long)age);
}

struct point {
  double x;
  double y;
};

struct point* test_init(Arena* a, double x, double y) {
  struct point* p = New(a, struct point);
  p->x = x;
  p->y = y;
  return p;
}

#include "adt.h"
#include "object.h"

UTEST(interface99, oop) {
  enum { size = KB(1) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  IShape r = newRectangle(arena, 5, 7);
  int p = VCALL(r, perim);
  ASSERT_EQ(p, 24);
  VCALL(r, scale, 5);
  p = VCALL(r, perim);
  ASSERT_EQ(p, 120);

  IShape t = newTriangle(arena, 5, 7, 3);
  p = VCALL(t, perim);
  ASSERT_EQ(p, 15);
  VCALL(t, scale, 5);
  p = VCALL(t, perim);
  ASSERT_EQ(p, 75);
}

UTEST(slice, push_fresh) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  i64s s = {0};
  *Push(arena, &s) = 10;
  *Push(arena, &s) = 20;
  *Push(arena, &s) = 30;
  ASSERT_EQ(s.len, 3);
  ASSERT_EQ(s.data[0], 10);
  ASSERT_EQ(s.data[1], 20);
  ASSERT_EQ(s.data[2], 30);
}

UTEST(slice, push_after_len_reset) {
  // Bug 4: {data!=NULL, len=0, cap>0} must not assert
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  i64s s = {0};
  *Push(arena, &s) = 1;
  *Push(arena, &s) = 2;
  ASSERT_EQ(s.len, 2);

  s.len = 0;  // reuse as buffer
  *Push(arena, &s) = 99;
  ASSERT_EQ(s.len, 1);
  ASSERT_EQ(s.data[0], 99);
  ASSERT_TRUE(s.cap > 0);
}

UTEST(slice, push_after_cap_reset) {
  // cap=0 detaches from old storage; Push must allocate fresh and copy
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  i64s s = {0};
  *Push(arena, &s) = 1;
  *Push(arena, &s) = 2;
  *Push(arena, &s) = 3;
  ASSERT_EQ(s.len, 3);

  int64_t* old_data = s.data;
  s.cap = 0;
  *Push(arena, &s) = 4;
  ASSERT_EQ(s.len, 4);
  ASSERT_EQ(s.data[0], 1);
  ASSERT_EQ(s.data[1], 2);
  ASSERT_EQ(s.data[2], 3);
  ASSERT_EQ(s.data[3], 4);
  ASSERT_TRUE(s.data != old_data);  // must have new backing storage
  ASSERT_TRUE(s.cap >= s.len);
}

UTEST(slice, clone_and_push) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  i64s s = {0};
  *Push(arena, &s) = 1;
  *Push(arena, &s) = 2;
  *Push(arena, &s) = 3;

  i64s copy = Clone(arena, s, 0, 2);
  ASSERT_EQ(copy.len, 2);
  ASSERT_EQ(copy.data[0], 1);
  ASSERT_EQ(copy.data[1], 2);

  *Push(arena, &copy) = 42;
  ASSERT_EQ(copy.len, 3);
  ASSERT_EQ(copy.data[2], 42);
}

int main(int argc, const char* argv[]) {
#ifdef __COSMOCC__
  ShowCrashReports();
#endif

  Arena* arena = arena_default();

  jmp_buf jmpbuf;
  if (ArenaOOM(arena, jmpbuf)) {
    fputs("!!! OOM exit !!!\n", stderr);
    exit(1);
  }

  ALOG(arena);
  {
    Scratch(arena);
    ALOG(arena);
    New(arena, char, MB(4));
    ALOG(arena);
  }
  ALOG(arena);

  int test_pqueue(Arena arena);
  test_pqueue(*arena);
  ALOG(arena);

  i64s fibs = test_slice(arena);
  fibs.cap = 0;
  *Push(arena, &fibs) = 0;
  puts(">>>fibs");
  for (int i = 0; i < fibs.len; ++i)
    printf("%lld,", (long long)fibs.data[i]);
  puts("<<<fibs");

  astr s = test_astr(arena);
  printf("test_astr: %.*s\n", S(s));

  // char* cs = astr_to_cstr(*arena, s);
  __autofree char* cs = astr_cstrdup(s);
  for (char* p = cs; *p; ++p) {
    New(arena, char, MB(1), OOM_NULL);
    char c = *p;
    if (c >= 'a' && c <= 'z') {
      *p -= 'a' - 'A';
    }
  }
  printf("astr_to_cstr: %s\n", cs);

  ALOG(arena);
  struct point* p = test_init(arena, 1.0, 2.0);
  struct point* p2 = New(arena, struct point, 1, p);
  ULOG(p2);
  p2->x += 10;
  ULOG(p);
  ALOG(arena);

  arena_release(arena);
#ifndef OOM_COMMIT
  free(default_arena_mem);  // arena_release only frees what it reserved itself
#endif

  return utest_main(argc, argv);
}
