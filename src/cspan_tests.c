#include "arena.h"
#include "cspan.h"
#include "utest.h"

static bool int_eq(const int* a, const int* b) {
  return *a == *b;
}

use_cspan3_with_eq(Span, int, int_eq);

typedef Slice(int) cspan_ints;
typedef Slice(Span3) cspan_tiles;

/* --- Rank reduction --- */

UTEST(cspan, subdim) {
  int array[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  Span3 m = cspan_md(array, 2, 2, 3);

  for (int i = 0; i < m.shape[0]; i++) {
    Span2 sub_i = cspan_submd3(&m, i);
    for (int j = 0; j < m.shape[1]; j++) {
      Span sub_i_j = cspan_submd2(&sub_i, j);
      for (int k = 0; k < m.shape[2]; k++) {
        ASSERT_EQ(*cspan_at(&sub_i_j, k), *cspan_at(&m, i, j, k));
      }
    }
  }
}

/* --- Slicing --- */

UTEST(cspan, slice) {
  int array[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  Span2 m1 = cspan_md(array, 3, 4);

  int sum1 = 0;
  for (int i = 0; i < m1.shape[0]; i++) {
    for (int j = 0; j < m1.shape[1]; j++) {
      sum1 += *cspan_at(&m1, i, j);
    }
  }

  Span2 m2 = cspan_slice(&m1, Span2, {c_ALL}, {2, 4});

  int sum2 = 0;
  for (int i = 0; i < m2.shape[0]; i++) {
    for (int j = 0; j < m2.shape[1]; j++) {
      sum2 += *cspan_at(&m2, i, j);
    }
  }
  ASSERT_EQ(78, sum1);
  ASSERT_EQ(45, sum2);
}

UTEST(cspan, slice2) {
  enum { size = KB(64) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  cspan_ints* values = New(arena, cspan_ints);
  for (int i = 0; i < 10 * 20 * 30; i++) {
    Push(arena, values, i);
  }

  Span3 ms3 = cspan_md(values->data, 10, 20, 30);
  ms3 = cspan_slice(&ms3, Span3, {1, 4}, {3, 7}, {20, 24});

  int sum = 0;
  for (int i = 0; i < ms3.shape[0]; i++) {
    for (int j = 0; j < ms3.shape[1]; j++) {
      for (int k = 0; k < ms3.shape[2]; k++) {
        sum += *cspan_at(&ms3, i, j, k);
      }
    }
  }
  ASSERT_EQ(65112, sum);

  sum = 0;
  for (Span3_iter it = Span3_begin(&ms3); it.ref; Span3_next(&it)) {
    sum += *it.ref;
  }
  ASSERT_EQ(65112, sum);
}

/* --- Equality and tile slicing --- */

UTEST(cspan, equality) {
  int base_data[] = {
      3, 1, 2, 3, 1, 2, 4, 5, 6, 2, 1, 7, 8, 9, 3, 1, 3, 1, 2, 3, 2, 2, 4, 5, 6, 3, 1, 7, 8, 9,
  };
  Span base = cspan_from_array(base_data);
  Span2 base2 = cspan_md(base.data, 6, 5);

  int test_data[] = {
      1, 2, 3, 4, 5, 6, 7, 8, 9,
  };
  Span test = cspan_from_array(test_data);
  Span2 test2 = cspan_md(test.data, 3, 3);

  // Test every 3x3 subtile in base2 against the test2 tile.
  for (int y = 0; y < base2.shape[0] - 3 + 1; y++) {
    for (int x = 0; x < base2.shape[1] - 3 + 1; x++) {
      bool expect_eq = (y == 0 && x == 1) || (y == 3 && x == 2);
      ASSERT_EQ(expect_eq, Span2_equals(test2, cspan_slice(&base2, Span2, {y, y + 3}, {x, x + 3})));
    }
  }

  // Check that the two 3x4 tiles are equal.
  ASSERT_TRUE(
      Span2_equals(cspan_slice(&base2, Span2, {0, 3}, {0, 4}), cspan_slice(&base2, Span2, {3, 6}, {1, 5})));
}

/* --- Cube slicing with Slice(Span3) --- */

struct cspan_cube {
  Arena arena;
  byte* mem;
  cspan_ints* values;
  cspan_tiles* tiles;
};

UTEST_F_SETUP(cspan_cube) {
  enum {
    TSIZE = 4,
    CUBE = 64,
    N = CUBE * CUBE * CUBE,
    TILES = (CUBE / TSIZE) * (CUBE / TSIZE) * (CUBE / TSIZE)
  };

  utest_fixture->mem = malloc(MB(4));
  ASSERT_TRUE(utest_fixture->mem != NULL);
  utest_fixture->arena = arena_init(utest_fixture->mem, MB(4));

  utest_fixture->values = New(&utest_fixture->arena, cspan_ints);
  Reserve(&utest_fixture->arena, utest_fixture->values, N);
  for (int i = 0; i < N; i++) {
    PushUnchecked(utest_fixture->values, i + 1);
  }

  Span3 ms3 = cspan_md(utest_fixture->values->data, CUBE, CUBE, CUBE);

  utest_fixture->tiles = New(&utest_fixture->arena, cspan_tiles);
  Reserve(&utest_fixture->arena, utest_fixture->tiles, TILES);
  for (int i = 0; i < CUBE; i += TSIZE) {
    for (int j = 0; j < CUBE; j += TSIZE) {
      for (int k = 0; k < CUBE; k += TSIZE) {
        Span3 tile = cspan_slice(&ms3, Span3, {i, i + TSIZE}, {j, j + TSIZE}, {k, k + TSIZE});
        PushUnchecked(utest_fixture->tiles, tile);
      }
    }
  }
}

UTEST_F_TEARDOWN(cspan_cube) {
  arena_release(&utest_fixture->arena);
  free(utest_fixture->mem);
}

UTEST_F(cspan_cube, slice3) {
  int64_t n = utest_fixture->values->len;
  int64_t sum = 0;

  // Iterate each 3d tile in sequence.
  for (isize t = 0; t < utest_fixture->tiles->len; t++) {
    Span3 tile = utest_fixture->tiles->data[t];
    for (Span3_iter it = Span3_begin(&tile); it.ref; Span3_next(&it)) {
      sum += *it.ref;
    }
  }

  ASSERT_EQ(utest_fixture->tiles->len, 4096);
  ASSERT_EQ(sum, n * (n + 1) / 2);
}
