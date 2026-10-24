#include "arena.h"
#include "utest.h"

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
typedef FixedSlice(int) fints;

/* Once-only evaluation probes: each helper bumps a shared counter and returns
 * its argument, so a macro that evaluates an argument more than once shows up
 * as an extra increment. */
static int slice_eval_calls;
static Arena* slice_eval_arena(Arena* a) {
  slice_eval_calls++;
  return a;
}
static ints* slice_eval_slice(ints* s) {
  slice_eval_calls++;
  return s;
}
static fints* slice_eval_fints(fints* b) {
  slice_eval_calls++;
  return b;
}
static int slice_eval_int(int v) {
  slice_eval_calls++;
  return v;
}
static isize slice_eval_cap(isize v) {
  slice_eval_calls++;
  return v;
}
static int* slice_eval_out(int* p) {
  slice_eval_calls++;
  return p;
}

/* --- Slice: initialization from a C fixed-size array --- */

UTEST(slice, from_array_copy_owns_storage) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  int data[] = {10, 20, 30};
  ints* s = New(arena, ints);
  SliceFromArray(arena, s, data);

  ASSERT_TRUE(s->data != data);
  ASSERT_EQ(s->len, 3);
  ASSERT_EQ(s->cap, 3);
  ASSERT_EQ(s->data[1], 20);

  // The copy is independent of the source array.
  data[1] = 99;
  ASSERT_EQ(s->data[1], 20);

  *Push(arena, s) = 40;  // ordinary movable growth
  ASSERT_EQ(s->len, 4);
  ASSERT_EQ(s->data[3], 40);
}

/* The copy form owns arena storage, so the source array's scope is irrelevant. */
static void slice_copy_from_stack(Arena* arena, ints* out) {
  int data[] = {5, 6, 7};
  SliceFromArray(arena, out, data);
}

UTEST(slice, from_array_copy_survives_source_scope) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  slice_copy_from_stack(arena, s);  // source array is gone
  ASSERT_EQ(s->len, 3);
  ASSERT_EQ(s->data[0], 5);
  ASSERT_EQ(s->data[2], 7);
}

UTEST(slice, from_array_copy_to_destination) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  int data[] = {7, 8, 9};
  ints* s = New(arena, ints);
  SliceFromArray(arena, s, data);

  ASSERT_TRUE(s->data != data);
  ASSERT_EQ(s->len, 3);
  ASSERT_EQ(s->cap, 3);
  ASSERT_EQ(s->data[2], 9);

  int top = 0;
  ASSERT_TRUE(Pop(s, &top));  // destination form binds s
  ASSERT_EQ(top, 9);
}

/* --- Slice: arena-owned descriptors --- */

struct SliceHolder {
  ints items;   // inline descriptor: a plain struct field
  ints* extra;  // pointer to a separately allocated descriptor
};

static void slice_append(Arena* arena, ints* s, int v) {
  *Push(arena, s) = v;
}

UTEST(slice, descriptor_is_arena_owned) {
  enum { size = KB(8) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  // New zero-initializes the descriptor into a valid empty slice; no stack
  // descriptor or `{}` initializer is needed.
  ints* s = New(arena, ints);
  ASSERT_TRUE(s->data == NULL);
  ASSERT_EQ(s->len, 0);
  ASSERT_EQ(s->cap, 0);

  // The descriptor pointer is what travels between functions. Mutations
  // through it are visible to the caller.
  slice_append(arena, s, 1);
  ASSERT_EQ(s->len, 1);
  ASSERT_EQ(s->data[0], 1);

  // A New-allocated record can hold a slice inline or by pointer. Both are
  // arena-owned and are passed to slice operations by address.
  struct SliceHolder* h = New(arena, struct SliceHolder);
  *Push(arena, &h->items) = 2;
  h->extra = New(arena, ints);
  *Push(arena, h->extra) = 3;
  ASSERT_EQ(h->items.len, 1);
  ASSERT_EQ(h->items.data[0], 2);
  ASSERT_EQ(h->extra->data[0], 3);

  // Growing the backing block relocates only `data`; the descriptor address
  // stays fixed because the descriptor itself lives in the arena.
  ints* descriptor = s;
  for (int i = 0; i < 100; i++)
    *Push(arena, s) = i;
  ASSERT_TRUE(s == descriptor);
  ASSERT_EQ(s->len, 101);
  ASSERT_EQ(s->data[1], 0);
  ASSERT_EQ(s->data[100], 99);
}

/* --- Slice: fixed (T[..]) via Push --- */

UTEST(slice, fixed_init_and_push) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  fints s = {0};
  SliceFixed(arena, &s, 8);
  ASSERT_EQ(s.len, 0);
  ASSERT_EQ(s.cap, 8);
  ASSERT_TRUE(s.data != NULL);

  int* base = s.data;
  byte* cur = arena->cur;
  for (int i = 0; i < 8; i++) {
    int* slot = TryPush(&s);
    ASSERT_TRUE(slot != NULL);
    *slot = i * 3;
    ASSERT_EQ(slot - s.data, i);
  }

  ASSERT_EQ(s.len, 8);
  ASSERT_TRUE(s.data == base);  // fixed pushes never move
  ASSERT_EQ(arena->cur, cur);   // and never allocate
  ASSERT_EQ(s.data[7], 21);

  ints* c = New(arena, ints);
  Clone(arena, c, &s);
  ASSERT_EQ(c->len, 8);
  ASSERT_EQ(c->data[7], 21);

  // The copy is an ordinary movable Slice, so later Push takes the growable
  // path.
  *Push(arena, c) = 22;  // ordinary growable Slice
  ASSERT_EQ(c->len, 9);
  ASSERT_EQ(c->data[8], 22);
}

/* --- Slice classes and their legal conversions --- */

UTEST(slice, movable_reserve_at_tip_extends_in_place) {
  // Growth at the arena tip extends in place without relocating; the
  // cheapest way to observe it is Reserve past cap while at the tip.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* t = New(arena, ints);
  Reserve(arena, t, 8);
  ASSERT_EQ(t->len, 0);
  int* base = t->data;

  for (int i = 0; i < 8; i++)
    *Push(arena, t) = i * 10;
  ASSERT_TRUE(t->data == base);  // in-capacity pushes never move
  ASSERT_EQ(t->data[0], 0);

  Reserve(arena, t, t->cap + 8);
  ASSERT_TRUE(t->data == base);  // at-tip growth: no relocation
  ASSERT_EQ(t->data[0], 0);

  isize idx = t->len;
  *Push(arena, t) = 99;
  ASSERT_EQ(idx, t->len - 1);
  ASSERT_EQ(t->data[idx], 99);
  ASSERT_EQ(t->data[0], 0);
}

UTEST(slice, movable_within_cap_is_off_tip_safe) {
  // Writes and reserves inside the existing cap never move, so they are
  // in place even when another allocation has moved the slice off-tip.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* t = New(arena, ints);
  Reserve(arena, t, 4);
  int* base = t->data;
  *Push(arena, t) = 1;

  New(arena, char);  // off-tip, but the handle still has spare capacity

  isize idx = t->len;
  *Push(arena, t) = 2;  // no growth: still in place
  ASSERT_TRUE(t->data == base);
  ASSERT_EQ(idx, 1);

  Reserve(arena, t, 4);  // at cap: no-op
  ASSERT_TRUE(t->data == base);
  ASSERT_EQ(t->data[0], 1);
  ASSERT_EQ(t->data[1], 2);
}

UTEST(slice, class_fixed_to_movable) {
  // Fixed -> movable: FixedSlice is fixed-capacity; Clone copies it out
  // into an ordinary movable Slice.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  fints b = {0};
  SliceFixed(arena, &b, 4);
  int* slot = TryPush(&b);
  ASSERT_EQ(slot, &b.data[0]);
  *slot = 1;
  slot = TryPush(&b);
  ASSERT_EQ(slot, &b.data[1]);
  *slot = 2;

  ints* copy = New(arena, ints);
  Clone(arena, copy, &b);
  ASSERT_EQ(copy->len, 2);
  ASSERT_TRUE(copy->data != b.data);
  ASSERT_EQ(copy->data[1], 2);
  *Push(arena, copy) = 3;  // ordinary movable growth
  ASSERT_EQ(copy->len, 3);
  *Push(arena, copy) = 4;
  ASSERT_EQ(copy->len, 4);
}

UTEST(slice, class_movable_to_fixed_rebuild) {
  // Movable -> fixed has no descriptor conversion. The legal path is to build a
  // FixedSlice and copy the elements; direct assignment is a compile error.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* g = New(arena, ints);
  *Push(arena, g) = 1;
  *Push(arena, g) = 2;
  *Push(arena, g) = 3;

  fints b = {0};
  SliceFixed(arena, &b, g->len);
  for (isize i = 0; i < g->len; i++) {
    int* slot = TryPush(&b);
    ASSERT_EQ(slot, &b.data[i]);
    *slot = g->data[i];
  }

  ASSERT_EQ(b.len, g->len);
  ASSERT_EQ(b.cap, g->len);
  ASSERT_EQ(b.data[2], 3);
}

UTEST(slice, empty_slice_grows_from_zero) {
  // A zero-initialized empty slice has no storage until it grows.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  ASSERT_TRUE(s->data == NULL);
  ASSERT_EQ(s->len, 0);
  ASSERT_EQ(s->cap, 0);

  ints* empty = New(arena, ints);
  Clone(arena, empty, s);
  ASSERT_TRUE(empty->data == NULL);
  ASSERT_EQ(empty->len, 0);
  ASSERT_EQ(empty->cap, 0);

  // Growing the empty slice allocates fresh arena storage.
  byte* cur = arena->cur;
  *Push(arena, s) = 7;
  ASSERT_EQ(s->len, 1);
  ASSERT_TRUE((void*)s->data == (void*)cur);
  ASSERT_EQ(s->data[0], 7);
}

UTEST(slice, empty_arena_fixed) {
  // Fixed empty: FixedSlice with capacity 0 is a valid fixed store.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  fints b = {0};
  SliceFixed(arena, &b, 0);
  ASSERT_TRUE(b.data == NULL);
  ASSERT_EQ(b.len, 0);
  ASSERT_EQ(b.cap, 0);

  ints* empty = New(arena, ints);
  Clone(arena, empty, &b);
  ASSERT_TRUE(empty->data == NULL);
  ASSERT_EQ(empty->len, 0);
  ASSERT_EQ(empty->cap, 0);
}

UTEST(slice, empty_movable_grows_at_tip) {
  // Empty movable slice grows by allocating at cur.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* t = New(arena, ints);
  ASSERT_TRUE(t->data == NULL);
  ASSERT_EQ(t->len, 0);
  ASSERT_EQ(t->cap, 0);

  byte* cur = arena->cur;
  *Push(arena, t) = 5;
  ASSERT_EQ(t->len, 1);
  ASSERT_TRUE(t->cap >= 1);
  ASSERT_TRUE((void*)t->data == (void*)cur);
  ASSERT_EQ(t->data[0], 5);

  // Another empty slice at the new tip: Reserve may allocate in place.
  ints* r = New(arena, ints);
  byte* r_cur = arena->cur;
  Reserve(arena, r, 4);
  ASSERT_TRUE(r->cap >= 4);
  ASSERT_TRUE((void*)r->data == (void*)r_cur);
}

UTEST(slice, empty_off_tip_can_grow) {
  // Empty slices have no elements to move, so growing them is safe even when
  // they are not at the arena tip: they simply allocate at the current tip.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* movable = New(arena, ints);
  New(arena, char);  // off-tip before the empty movable slice grows
  byte* cur = arena->cur;
  *Push(arena, movable) = 1;
  ASSERT_TRUE((byte*)movable->data >= cur);
  ASSERT_EQ((uintptr_t)movable->data % (uintptr_t)alignof(int), (uintptr_t)0);
  ASSERT_EQ(movable->data[0], 1);

  ints* t = New(arena, ints);
  New(arena, char);  // off-tip before the second empty slice grows
  byte* cur2 = arena->cur;
  *Push(arena, t) = 2;
  ASSERT_TRUE((byte*)t->data >= cur2);
  ASSERT_EQ((uintptr_t)t->data % (uintptr_t)alignof(int), (uintptr_t)0);
  ASSERT_EQ(t->data[0], 2);
}

UTEST(slice, overlapping_slices_nested_scratch) {
  enum { size = KB(8) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* outer = New(arena, ints);
  *Push(arena, outer) = 10;
  *Push(arena, outer) = 20;
  int* outer_data = outer->data;
  byte* outer_cur = arena->cur;

  {
    Scratch(arena);

    ints* inner = New(arena, ints);
    for (int i = 1; i <= 4; i++)
      *Push(arena, inner) = i;

    // Overlapping structural descriptors over the same arena elements.
    ints* alias = New(arena, ints);
    alias->data = inner->data;
    alias->len = inner->len;
    alias->cap = inner->cap;

    ints* head = New(arena, ints);
    head->data = inner->data;
    head->len = 2;
    head->cap = 2;

    ints* tail = New(arena, ints);
    tail->data = inner->data + 1;
    tail->len = inner->len - 1;
    tail->cap = inner->cap - 1;

    alias->data[0] = 99;
    ASSERT_EQ(inner->data[0], 99);
    ASSERT_EQ(tail->data[0], 2);

    // Cloning overlapping views copies from their own ranges; no corruption.
    ints* tail_copy = New(arena, ints);
    ints* head_copy = New(arena, ints);
    Clone(arena, head_copy, head);
    Clone(arena, tail_copy, tail);
    ASSERT_EQ(head_copy->len, 2);
    ASSERT_EQ(head_copy->data[0], 99);
    ASSERT_EQ(head_copy->data[1], 2);
    ASSERT_EQ(tail_copy->len, 3);
    ASSERT_EQ(tail_copy->data[0], 2);
    ASSERT_EQ(tail_copy->data[2], 4);

    {
      Scratch(arena);
      ints* deeper = New(arena, ints);
      *Push(arena, deeper) = 7;
      ASSERT_GT(arena->cur, (byte*)inner->data);
    }

    // Inner survives the nested scratch and remains readable.
    ASSERT_EQ(inner->data[0], 99);
    ASSERT_EQ(inner->data[3], 4);
  }

  // The outer arena pointer is restored; outer data is unaffected.
  ASSERT_EQ(arena->cur, outer_cur);
  ASSERT_TRUE(outer->data == outer_data);
  ASSERT_EQ(outer->data[0], 10);
  *Push(arena, outer) = 30;  // spare capacity, no growth needed
  ASSERT_TRUE(outer->data == outer_data);

  // Outer is at the tip again after Scratch: fill it, then extend in place.
  while (outer->len < outer->cap)
    *Push(arena, outer) = 0;
  *Push(arena, outer) = 31;
  ASSERT_TRUE(outer->data == outer_data);
  ASSERT_EQ(outer->data[outer->len - 1], 31);
}

UTEST(slice, class_movable_moves) {
  // Movable: an ordinary Slice may relocate when growth happens off-tip.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  *Push(arena, s) = 1;
  int* old = s->data;

  New(arena, char);  // the slice is no longer at the arena tip

  while (s->len < s->cap)
    *Push(arena, s) = 0;

  *Push(arena, s) = 99;  // full and off-tip: grow by moving
  ASSERT_TRUE(s->data != old);
  ASSERT_EQ(s->data[0], 1);
  ASSERT_EQ(s->data[s->len - 1], 99);
}

UTEST(slice, movable_push_at_tip_grows_in_place) {
  // Movable Push extends capacity in place while the block ends at the tip.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* t = New(arena, ints);
  Reserve(arena, t, 2);
  int* base = t->data;
  byte* cur = arena->cur;

  *Push(arena, t) = 10;
  *Push(arena, t) = 20;
  ASSERT_TRUE(t->data == base);
  ASSERT_EQ(arena->cur, cur);  // spare capacity: no arena activity

  isize idx = t->len;
  *Push(arena, t) = 30;  // full, at tip: grow in place
  ASSERT_TRUE(t->data == base);
  ASSERT_TRUE(t->cap > 2);
  ASSERT_TRUE(arena->cur > cur);
  ASSERT_EQ(t->len, 3);
  ASSERT_EQ(idx, 2);
  ASSERT_EQ(t->data[idx], 30);
}

UTEST(slice, push_arena_form_grows_at_tip) {
  // TryPush(arena, s) extends at the arena tip and keeps the backing in place.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  Reserve(arena, s, 4);
  for (int i = 0; i < 4; i++)
    *TryPush(arena, s) = i;
  ASSERT_EQ(s->len, 4);
  ASSERT_EQ(s->cap, 4);

  int* base = s->data;
  byte* cur = arena->cur;
  int* slot = TryPush(arena, s);
  ASSERT_TRUE(slot != NULL);
  *slot = 42;

  ASSERT_TRUE(s->data == base);  // no relocation
  ASSERT_TRUE(s->cap > 4);       // extended in place
  ASSERT_EQ(s->len, 5);
  ASSERT_EQ(s->data[4], 42);
  ASSERT_TRUE(arena->cur > cur);
}

UTEST(slice, push_arena_form_allocates_empty_slice) {
  // An empty slice has nothing to move, so TryPush(arena, s) may allocate it.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  int* slot = TryPush(arena, s);
  ASSERT_TRUE(slot != NULL);
  *slot = 7;

  ASSERT_EQ(s->len, 1);
  ASSERT_TRUE(s->cap >= 1);
  ASSERT_TRUE(s->data != NULL);
  ASSERT_EQ(s->data[0], 7);
}

UTEST(slice, push_arena_form_off_tip_full_returns_null) {
  // Off the tip, TryPush(arena, s) refuses to move: full means NULL.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  Reserve(arena, s, 4);
  for (int i = 0; i < 4; i++)
    *TryPush(arena, s) = i;

  New(arena, char);  // move the slice off the arena tip
  int* base = s->data;
  isize cap = s->cap;
  byte* cur = arena->cur;

  ASSERT_TRUE(TryPush(arena, s) == NULL);
  ASSERT_TRUE(s->data == base);
  ASSERT_EQ(s->cap, cap);
  ASSERT_EQ(arena->cur, cur);
  ASSERT_EQ(s->len, 4);
}

UTEST(slice, push_arena_form_uses_spare_capacity_off_tip) {
  // With spare capacity, TryPush(arena, s) writes in place and never allocates,
  // even when the slice is off the arena tip.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  Reserve(arena, s, 4);
  *TryPush(arena, s) = 1;
  New(arena, char);  // off-tip, but the slice has spare capacity

  int* base = s->data;
  byte* cur = arena->cur;
  int* slot = TryPush(arena, s);
  ASSERT_TRUE(slot != NULL);
  *slot = 2;

  ASSERT_TRUE(s->data == base);
  ASSERT_EQ(arena->cur, cur);
  ASSERT_EQ(s->len, 2);
  ASSERT_EQ(s->data[1], 2);
}

UTEST(slice, movable_reserve_at_tip_extends) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* t = New(arena, ints);
  Reserve(arena, t, 2);
  int* base = t->data;

  Reserve(arena, t, 4);  // at tip: extends, no move
  ASSERT_TRUE(t->data == base);
  ASSERT_TRUE(t->cap >= 4);
}

UTEST(slice, movable_reserve_off_tip_within_cap) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* t = New(arena, ints);
  Reserve(arena, t, 4);
  *Push(arena, t) = 1;

  New(arena, char);  // t is off-tip
  int* base = t->data;

  // Reserve within the existing cap is a no-op, even off-tip.
  Reserve(arena, t, 4);
  ASSERT_TRUE(t->data == base);
  ASSERT_EQ(t->cap, 4);

  // Push within spare capacity is still an in-place write off-tip.
  *Push(arena, t) = 2;
  ASSERT_TRUE(t->data == base);
  ASSERT_EQ(t->len, 2);
  ASSERT_EQ(t->data[1], 2);
}

UTEST(slice, movable_address_stable_after_at_tip_growth) {
  // At-tip growth extends in place, so addresses read before the growth
  // stay valid — but only at the tip. Off-tip growth relocates instead,
  // which is why clients must use indices (see fill_survives_grow).
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* t = New(arena, ints);
  Reserve(arena, t, 2);
  *Push(arena, t) = 10;
  int* first = &t->data[0];

  *Push(arena, t) = 20;  // still within cap
  *Push(arena, t) = 30;  // full, at tip: grow in place

  ASSERT_TRUE(&t->data[0] == first);
  ASSERT_EQ(*first, 10);
  ASSERT_EQ(t->data[2], 30);
}

UTEST(slice, fixed_push_unchecked) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  fints b = {0};
  SliceFixed(arena, &b, 4);

  int* pslot = TryPush(&b);
  Assert(pslot != NULL);
  *pslot = 7;
  ASSERT_EQ(b.data[0], 7);
  ASSERT_EQ(b.len, 1);
  ASSERT_EQ(b.data[0], 7);
}

UTEST(slice, fixed_struct_clone) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  typedef Slice(struct Point) points;
  typedef FixedSlice(struct Point) fpoints;

  fpoints b = {0};
  SliceFixed(arena, &b, 2);
  struct Point* slot = TryPush(&b);
  ASSERT_EQ(slot, &b.data[0]);
  *slot = (struct Point){1, 2};
  slot = TryPush(&b);
  ASSERT_EQ(slot, &b.data[1]);
  *slot = (struct Point){3, 4};

  points* copy = New(arena, points);
  Clone(arena, copy, &b);
  ASSERT_EQ(copy->len, 2);
  ASSERT_EQ(copy->data[1].x, 3);
  ASSERT_EQ(copy->data[1].y, 4);
  ASSERT_EQ(copy->data[0].x, 1);
}

/* --- Slice: pass/store rubric (movable by pointer, fixed by value) --- */

struct FixedRecord {
  fints values;
};

static fints fixed_build(Arena* arena, isize cap, isize len) {
  fints b = {0};
  SliceFixed(arena, &b, cap);
  for (isize i = 0; i < len; i++) {
    int* slot = TryPush(&b);
    Assert(slot != NULL);
    *slot = (int)(i * 10);
  }
  return b;
}

static int fixed_sum(fints view) {
  int sum = 0;
  for (isize i = 0; i < view.len; i++)
    sum += view.data[i];
  return sum;
}

static isize fixed_push_copy(fints view, int v) {
  int* slot = TryPush(&view);
  if (!slot)
    return -1;
  *slot = v;
  return slot - view.data;
}

UTEST(slice, fixed_by_value_aliasing_and_mutation) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  fints b = fixed_build(arena, 6, 4);
  ASSERT_EQ(b.len, 4);
  ASSERT_EQ(b.cap, 6);
  ASSERT_EQ(fixed_sum(b), 60);

  // Storing the fixed descriptor by value is the supported model: a local
  // copy and a struct field both alias the same fixed backing block.
  fints copy = b;
  struct FixedRecord rec = {.values = b};
  ASSERT_TRUE(copy.data == b.data);
  ASSERT_TRUE(rec.values.data == b.data);
  ASSERT_EQ(copy.len, b.len);

  // Data writes are shared because the backing block is shared.
  copy.data[0] = 99;
  ASSERT_EQ(b.data[0], 99);

  // Length/capacity are descriptor-local: appending through one copy does
  // not advance the others, but writes the same backing block.
  int* slot = TryPush(&copy);
  ASSERT_EQ(slot, &copy.data[4]);
  *slot = 40;
  ASSERT_EQ(copy.len, 5);
  ASSERT_EQ(b.len, 4);
  ASSERT_EQ(rec.values.len, 4);
  ASSERT_EQ(b.data[4], 40);

  // A by-value parameter is a read view plus a local cursor. It can read and
  // write shared data, but its append does not update the caller's len.
  ASSERT_EQ(fixed_push_copy(b, 50), 4);
  ASSERT_EQ(b.len, 4);
  ASSERT_EQ(b.data[4], 50);
}

UTEST(slice, fixed_by_value_lifetime) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  // The descriptor is returned by value; its backing block is arena-owned.
  fints b = fixed_build(arena, 4, 3);
  ASSERT_EQ(b.len, 3);
  ASSERT_EQ(b.data[2], 20);

  New(arena, char, KB(1));  // unrelated allocation does not move fixed data
  ASSERT_EQ(fixed_sum(b), 30);

  // The value descriptor itself survives reset (it is on the stack), while
  // the arena backing does not. Do not dereference b.data after reset.
  arena_reset(arena);
  ASSERT_TRUE(b.data != NULL);
  ASSERT_EQ(b.len, 3);
  ASSERT_EQ(b.cap, 4);
}

UTEST(slice, movable_descriptor_lifetime) {
  enum { size = KB(8) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  *Push(arena, s) = 1;
  int* data = s->data;

  {
    Scratch(arena);
    New(arena, char, KB(1));
  }

  // The descriptor and its data both live in the outer arena range, so a
  // nested Scratch does not invalidate either.
  ASSERT_TRUE(s->data == data);
  ASSERT_EQ(s->data[0], 1);

  // The descriptor is arena-owned, not a stack temporary: pointer aliases
  // stay valid for the arena's lifetime. After reset both descriptor and
  // data are invalid and must not be touched.
  ints* alias = s;
  ASSERT_TRUE(alias == s);
  arena_reset(arena);
}

UTEST(slice, movable_by_pointer_aliasing_and_relocation) {
  enum { size = KB(8) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  // The supported movable model is one arena-owned descriptor, passed by
  // pointer. Pointer aliases observe every mutation, including relocation.
  ints* s = New(arena, ints);
  *Push(arena, s) = 1;
  *Push(arena, s) = 2;
  ints* alias = s;
  ASSERT_TRUE(alias == s);
  ASSERT_EQ(alias->len, 2);

  // A value copy compiles, but it is not the storage model: it pins the old
  // backing pointer and does not follow later growth.
  ints copy = *s;
  ASSERT_TRUE(copy.data == s->data);
  ASSERT_EQ(copy.len, 2);

  New(arena, char);  // take the movable slice off the arena tip
  while (s->len < s->cap)
    *Push(arena, s) = 0;
  int* old_data = s->data;
  *Push(arena, s) = 99;  // full and off-tip: relocate

  ASSERT_TRUE(s->data != old_data);
  ASSERT_TRUE(copy.data == old_data);   // value copy still points at old block
  ASSERT_TRUE(alias->data == s->data);  // pointer alias follows the descriptor
  ASSERT_EQ(alias->len, s->len);
}

UTEST(slice, fixed_never_relocates_movable_does) {
  enum { size = KB(8) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  fints f = {0};
  SliceFixed(arena, &f, 8);
  int* fixed_base = f.data;

  ints* m = New(arena, ints);
  *Push(arena, m) = 1;
  int* movable_base = m->data;

  New(arena, char);  // both are off the arena tip
  byte* cur = arena->cur;

  // Fixed appends never allocate or relocate, even off-tip.
  for (int i = 0; i < 8; i++) {
    int* slot = TryPush(&f);
    ASSERT_EQ(slot, &f.data[i]);
    *slot = i;
  }
  ASSERT_TRUE(f.data == fixed_base);
  ASSERT_EQ(arena->cur, cur);

  // Movable growth off-tip relocates its backing block.
  while (m->len < m->cap)
    *Push(arena, m) = 0;
  *Push(arena, m) = 99;
  ASSERT_TRUE(m->data != movable_base);
}

UTEST(slice, at_tip_query) {
  enum { size = KB(8) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* m = New(arena, ints);
  ASSERT_TRUE(SliceAtTip(arena, m));  // empty: no backing block, nothing to move

  *Push(arena, m) = 1;
  ASSERT_TRUE(SliceAtTip(arena, m));

  fints f = {0};
  SliceFixed(arena, &f, 4);
  ASSERT_TRUE(SliceAtTip(arena, &f));  // fixed block now ends at the tip
  ASSERT_FALSE(SliceAtTip(arena, m));  // movable block is now off-tip

  // Off-tip reserve relocates the movable slice to the tip.
  int* old = m->data;
  Reserve(arena, m, 64);
  ASSERT_TRUE(m->data != old);
  ASSERT_TRUE(SliceAtTip(arena, m));
  ASSERT_FALSE(SliceAtTip(arena, &f));

  // At-tip reserve extends in place and stays at the tip.
  old = m->data;
  Reserve(arena, m, 128);
  ASSERT_TRUE(m->data == old);
  ASSERT_TRUE(SliceAtTip(arena, m));

  New(arena, char);  // both blocks are now behind the bump pointer
  ASSERT_FALSE(SliceAtTip(arena, m));
  ASSERT_FALSE(SliceAtTip(arena, &f));
}

UTEST(slice, at_tip_empty_reserve_pattern) {
  enum { size = KB(8) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  // The documented pattern must work on an empty slice: the query reports
  // true (nothing to move), Reserve allocates fresh backing, and a direct
  // fill into the reserved capacity is valid.
  ints* m = New(arena, ints);
  if (SliceAtTip(arena, m))
    Reserve(arena, m, 10);
  ASSERT_EQ(m->cap, 10);
  for (isize i = 0; i < 10; i++)
    m->data[m->len + i] = (int)(i * 2);
  m->len += 10;
  ASSERT_EQ(m->len, 10);
  ASSERT_EQ(At(m, 9), 18);
}

/* --- Slice: Pop (structural shrink) --- */

UTEST(slice, pop_returns_last_and_decrements) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  *Push(arena, s) = 10;
  *Push(arena, s) = 20;
  *Push(arena, s) = 30;
  int* base = s->data;
  isize cap = s->cap;

  int top = 0;
  ASSERT_TRUE(Pop(s, &top));
  ASSERT_EQ(top, 30);
  ASSERT_EQ(s->len, 2);
  ASSERT_EQ(s->cap, cap);  // shrinking does not release capacity
  ASSERT_TRUE(s->data == base);

  ASSERT_TRUE(Pop(s, &top));
  ASSERT_EQ(top, 20);
  ASSERT_TRUE(Pop(s, &top));
  ASSERT_EQ(top, 10);
  ASSERT_EQ(s->len, 0);

  top = -1;
  ASSERT_FALSE(Pop(s, &top));  // empty: false, and *out is untouched
  ASSERT_EQ(top, -1);

  // The popped slots are still owned by the slice; Push reuses them in place.
  *Push(arena, s) = 40;
  ASSERT_TRUE(s->data == base);
  ASSERT_EQ(s->len, 1);
  ASSERT_EQ(s->data[0], 40);
}

UTEST(slice, pop_fixed) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  fints b = {0};
  SliceFixed(arena, &b, 4);
  int* base = b.data;
  int* slot = TryPush(&b);
  ASSERT_EQ(slot, &b.data[0]);
  *slot = 1;
  slot = TryPush(&b);
  ASSERT_EQ(slot, &b.data[1]);
  *slot = 2;

  New(arena, char);  // off-tip: Pop must still shrink in place
  int top = 0;
  ASSERT_TRUE(Pop(&b, &top));
  ASSERT_EQ(top, 2);
  ASSERT_EQ(b.len, 1);
  ASSERT_TRUE(b.data == base);

  slot = TryPush(&b);
  ASSERT_EQ(slot, &b.data[1]);  // fixed push after pop reuses the slot
  *slot = 3;
  ASSERT_EQ(b.len, 2);
  ASSERT_EQ(b.data[1], 3);
  ASSERT_TRUE(b.data == base);
}

UTEST(slice, pop_struct_value) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  points* s = New(arena, points);
  *Push(arena, s) = ((struct Point){1, 2});

  struct Point top;
  ASSERT_TRUE(Pop(s, &top));
  ASSERT_EQ(top.x, 1);
  ASSERT_EQ(top.y, 2);
  ASSERT_EQ(s->len, 0);
  ASSERT_FALSE(Pop(s, &top));
}

UTEST(slice, push_then_pop_returns_same_value) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  *Push(arena, s) = 7;  // a value that must survive the round trips
  for (int v = -3; v <= 3; v++) {
    isize idx = s->len;
    *Push(arena, s) = v;
    ASSERT_EQ(idx, s->len - 1);
    ASSERT_EQ(s->data[idx], v);
    int popped = 0;
    ASSERT_TRUE(Pop(s, &popped));
    ASSERT_EQ(popped, v);
    ASSERT_EQ(s->len, 1);
    ASSERT_EQ(s->data[0], 7);
  }

  points* ps = New(arena, points);
  *Push(arena, ps) = ((struct Point){3, 4});
  struct Point p;
  ASSERT_TRUE(Pop(ps, &p));
  ASSERT_EQ(p.x, 3);
  ASSERT_EQ(p.y, 4);
  ASSERT_EQ(ps->len, 0);
}

UTEST(slice, pop_then_push_is_noop) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  for (int i = 0; i < 4; i++)
    *Push(arena, s) = i * 10;
  int* data = s->data;
  isize len = s->len;
  isize cap = s->cap;
  int last = s->data[len - 1];

  int popped = 0;
  ASSERT_TRUE(Pop(s, &popped));
  ASSERT_EQ(popped, last);
  *Push(arena, s) = popped;

  ASSERT_EQ(s->len, len);
  ASSERT_EQ(s->cap, cap);
  ASSERT_TRUE(s->data == data);
  for (isize i = 0; i < len; i++)
    ASSERT_EQ(s->data[i], (int)(i * 10));

  // The same round trip is a no-op for a fixed slice within its cap.
  fints b = {0};
  SliceFixed(arena, &b, 4);
  for (int i = 0; i < 3; i++) {
    int* slot = TryPush(&b);
    ASSERT_EQ(slot, &b.data[i]);
    *slot = i + 1;
  }
  int* bdata = b.data;
  isize blen = b.len;
  isize bcap = b.cap;
  int blast = b.data[blen - 1];

  int bpopped = 0;
  ASSERT_TRUE(Pop(&b, &bpopped));
  ASSERT_EQ(bpopped, blast);
  int* slot = TryPush(&b);
  ASSERT_EQ(slot, &b.data[blen - 1]);
  *slot = bpopped;

  ASSERT_EQ(b.len, blen);
  ASSERT_EQ(b.cap, bcap);
  ASSERT_TRUE(b.data == bdata);
  for (isize i = 0; i < blen; i++)
    ASSERT_EQ(b.data[i], (int)(i + 1));
}

UTEST(slice, movable_pop_then_push_is_noop) {
  enum { size = KB(8) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  for (int i = 0; i < 16; i++)
    *Push(arena, s) = i;
  int* before = s->data;
  isize cap_before = s->cap;

  New(arena, char);      // move the slice off the arena tip
  *Push(arena, s) = 16;  // full and off-tip: class 4 relocates
  ASSERT_TRUE(s->data != before);
  ASSERT_TRUE(s->cap > cap_before);

  // A relocated movable Slice still round-trips values through Push/Pop.
  int roundtrip = 1234;
  *Push(arena, s) = roundtrip;
  int rt = 0;
  ASSERT_TRUE(Pop(s, &rt));
  ASSERT_EQ(rt, roundtrip);

  // Fill the relocated block to capacity: Pop then Push must reuse the last
  // slot instead of triggering another growth/move.
  while (s->len < s->cap)
    *Push(arena, s) = 0;
  int* data = s->data;
  isize len = s->len;
  isize cap = s->cap;
  ASSERT_EQ(len, cap);
  int last = s->data[len - 1];

  int popped = 0;
  ASSERT_TRUE(Pop(s, &popped));
  ASSERT_EQ(popped, last);
  *Push(arena, s) = popped;

  ASSERT_EQ(s->len, len);
  ASSERT_EQ(s->cap, cap);
  ASSERT_TRUE(s->data == data);
  for (isize i = 0; i < len; i++)
    ASSERT_EQ(s->data[i], i < 17 ? (int)i : 0);
}

UTEST(slice, push_grow_move_then_pop) {
  enum { size = KB(8) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  for (int i = 0; i < 16; i++)
    *Push(arena, s) = i;
  int* old_data = s->data;
  isize old_cap = s->cap;

  New(arena, char);  // the full slice is no longer at the arena tip
  int sentinel = 123456;
  isize idx = s->len;
  *Push(arena, s) = sentinel;  // growth: memmove old elements

  ASSERT_TRUE(s->data != old_data);
  ASSERT_TRUE(s->cap > old_cap);
  ASSERT_EQ(idx, s->len - 1);
  ASSERT_EQ(s->data[idx], sentinel);
  for (int i = 0; i < 16; i++)
    ASSERT_EQ(s->data[i], i);  // pre-existing elements survived the move

  int* moved_data = s->data;
  isize moved_cap = s->cap;
  int popped = 0;
  ASSERT_TRUE(Pop(s, &popped));
  ASSERT_EQ(popped, sentinel);
  ASSERT_EQ(s->len, 16);
  ASSERT_TRUE(s->data == moved_data);
  ASSERT_EQ(s->cap, moved_cap);
  for (int i = 0; i < 16; i++)
    ASSERT_EQ(s->data[i], i);
}

UTEST(slice, class_fixed_push_full_returns_null) {
  // Fixed is fixed-capacity: pushing past cap returns NULL without
  // appending and without aborting.
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  fints b = {0};
  SliceFixed(arena, &b, 2);
  int* slot = TryPush(&b);
  ASSERT_EQ(slot, &b.data[0]);
  *slot = 10;
  slot = TryPush(&b);
  ASSERT_EQ(slot, &b.data[1]);
  *slot = 20;
  ASSERT_EQ(b.len, 2);
  ASSERT_TRUE(TryPush(&b) == NULL);
  ASSERT_EQ(b.len, 2);
  ASSERT_EQ(b.data[0], 10);
  ASSERT_EQ(b.data[1], 20);

  // Capacity 0 is a valid empty fixed slice: every Push returns NULL.
  fints e = {0};
  SliceFixed(arena, &e, 0);
  ASSERT_TRUE(TryPush(&e) == NULL);
  ASSERT_EQ(e.len, 0);
}

UTEST(slice, movable_push_arena_form_never_moves) {
  enum { size = KB(8) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  Reserve(arena, s, 4);  // reserve before taking references
  int* base = s->data;

  int* slot = TryPush(arena, s);
  ASSERT_EQ(slot, &s->data[0]);
  *slot = 10;
  slot = TryPush(arena, s);
  ASSERT_EQ(slot, &s->data[1]);
  *slot = 20;

  New(arena, char);  // off-tip; TryPush(arena, s) must still not move it
  byte* cur = arena->cur;
  slot = TryPush(arena, s);
  ASSERT_EQ(slot, &s->data[2]);
  *slot = 30;
  slot = TryPush(arena, s);
  ASSERT_EQ(slot, &s->data[3]);
  *slot = 40;
  ASSERT_TRUE(TryPush(arena, s) == NULL);  // full off-tip: no append, no growth
  ASSERT_TRUE(s->data == base);
  ASSERT_EQ(s->len, 4);
  ASSERT_EQ(arena->cur, cur);
  ASSERT_EQ(base[3], 40);

  // Push remains the growth path and may relocate.
  isize idx = s->len;
  *Push(arena, s) = 50;
  ASSERT_EQ(idx, 4);
  ASSERT_TRUE(s->data != base);
  ASSERT_EQ(s->data[4], 50);
}

UTEST(slice, pop_empty_is_safe) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  // Empty zero-capacity slice: false and *out unchanged.
  ints* s = New(arena, ints);
  int top = 123;
  ASSERT_FALSE(Pop(s, &top));
  ASSERT_EQ(top, 123);
  ASSERT_EQ(s->len, 0);
  ASSERT_TRUE(s->data == NULL);
  ASSERT_EQ(s->cap, 0);

  // Empty after draining a slice with spare capacity: false, not underflow.
  *Push(arena, s) = 7;
  ASSERT_TRUE(Pop(s, &top));
  ASSERT_EQ(top, 7);
  int* data = s->data;
  isize cap = s->cap;
  top = 123;
  ASSERT_FALSE(Pop(s, &top));
  ASSERT_EQ(top, 123);
  ASSERT_EQ(s->len, 0);
  ASSERT_EQ(s->cap, cap);
  ASSERT_TRUE(s->data == data);

  // A fixed slice has the same safe empty behavior.
  fints b = {0};
  SliceFixed(arena, &b, 4);
  int btop = 123;
  ASSERT_FALSE(Pop(&b, &btop));
  ASSERT_EQ(btop, 123);
  int* slot = TryPush(&b);
  ASSERT_EQ(slot, &b.data[0]);
  *slot = 9;
  ASSERT_TRUE(Pop(&b, &btop));
  ASSERT_EQ(btop, 9);
  btop = 123;
  ASSERT_FALSE(Pop(&b, &btop));
  ASSERT_EQ(btop, 123);
  ASSERT_EQ(b.len, 0);
  ASSERT_EQ(b.cap, 4);
}

UTEST(slice, clone_full) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  for (int i = 0; i < 5; i++)
    *Push(arena, s) = i * 10;

  ints* copy = New(arena, ints);
  Clone(arena, copy, s);
  ASSERT_EQ(copy->len, 5);
  for (int i = 0; i < 5; i++)
    ASSERT_EQ(copy->data[i], i * 10);
  ASSERT_TRUE(copy->data != s->data);
}

UTEST(slice, clone_subslice) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  for (int i = 0; i < 10; i++)
    *Push(arena, s) = i;

  ints* mid = New(arena, ints);
  Clone(arena, mid, s, 3, 4);
  ASSERT_EQ(mid->len, 4);
  ASSERT_EQ(mid->data[0], 3);
  ASSERT_EQ(mid->data[1], 4);
  ASSERT_EQ(mid->data[2], 5);
  ASSERT_EQ(mid->data[3], 6);
}

UTEST(slice, push_grow_many) {
  enum { size = KB(16) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  for (int i = 0; i < 200; i++)
    *Push(arena, s) = i;

  ASSERT_EQ(s->len, 200);
  ASSERT_TRUE(s->cap >= 200);
  for (int i = 0; i < 200; i++)
    ASSERT_EQ(s->data[i], i);

  isize idx = s->len;
  *Push(arena, s) = 999;
  ASSERT_EQ(idx, 200);
  ASSERT_EQ(s->data[idx], 999);
  ASSERT_EQ(s->len, 201);
}

/* --- Slice: reserved fill (caller-vouched capacity) --- */

UTEST(slice, push_unchecked_fills_reserved) {
  enum { size = KB(16) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  Reserve(arena, s, s->len + 200);
  isize cap = s->cap;
  byte* cur = arena->cur;

  for (int i = 0; i < 200; i++) {
    int* pslot = TryPush(arena, s);
    Assert(pslot != NULL);
    *pslot = i * 2;
  }

  ASSERT_EQ(s->len, 200);
  ASSERT_EQ(s->cap, cap);      // no growth stepped it
  ASSERT_EQ(arena->cur, cur);  // and nothing allocated at all
  ASSERT_EQ(s->data[199], 398);
}

UTEST(slice, push_unchecked_shares_state_with_push) {
  enum { size = KB(16) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  Reserve(arena, s, 4);
  *TryPush(arena, s) = 1;
  *TryPush(arena, s) = 2;

  // Checked Push sees the unchecked len and grows past the reserved room.
  *Push(arena, s) = 3;
  *Push(arena, s) = 4;
  *Push(arena, s) = 5;
  ASSERT_EQ(s->len, 5);

  // The slice now has fresh room again: fill it unchecked.
  *TryPush(arena, s) = 6;
  ASSERT_EQ(s->len, 6);
  for (int i = 0; i < 6; i++)
    ASSERT_EQ(s->data[i], i + 1);
}

UTEST(slice, push_unchecked_top_up_partially_filled) {
  enum { size = KB(16) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  for (int i = 0; i < 3; i++)
    *Push(arena, s) = i * 10;

  // s->len + n rooms n more pushes on top of the 3 already held.
  Reserve(arena, s, s->len + 100);
  isize cap = s->cap;
  byte* cur = arena->cur;

  for (int i = 0; i < 100; i++) {
    int* pslot = TryPush(arena, s);
    Assert(pslot != NULL);
    *pslot = i;
  }

  ASSERT_EQ(s->len, 103);
  ASSERT_EQ(s->cap, cap);      // no growth stepped it
  ASSERT_EQ(arena->cur, cur);  // and nothing allocated at all
  ASSERT_EQ(s->data[0], 0);
  ASSERT_EQ(s->data[2], 20);
  ASSERT_EQ(s->data[102], 99);
}

UTEST(slice, reserve_grows_up_front) {
  enum { size = KB(16) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  Reserve(arena, s, 200);
  ASSERT_TRUE(s->cap >= 200);
  ASSERT_EQ(s->len, 0);

  isize cap = s->cap;
  for (int i = 0; i < 200; i++)
    *Push(arena, s) = i;

  ASSERT_EQ(s->cap, cap);  // reserved once, grown never
  ASSERT_EQ(s->len, 200);
  ASSERT_EQ(s->data[199], 199);
}

UTEST(slice, reserve_preserves_elements) {
  enum { size = KB(16) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  for (int i = 0; i < 3; i++)
    *Push(arena, s) = i * 10;

  int* before = s->data;
  New(arena, int, 64);  // off the arena tip: reserving must copy, not extend
  Reserve(arena, s, 100);

  ASSERT_TRUE(s->data != before);
  ASSERT_EQ(s->len, 3);
  ASSERT_TRUE(s->cap >= 100);
  ASSERT_EQ(s->data[0], 0);
  ASSERT_EQ(s->data[2], 20);

  *Push(arena, s) = 30;
  ASSERT_EQ(s->data[3], 30);
}

UTEST(slice, reserve_is_grow_only) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  *Push(arena, s) = 1;

  isize cap = s->cap;
  Reserve(arena, s, 4);  // below current capacity
  ASSERT_EQ(s->cap, cap);
  Reserve(arena, s, 0);
  ASSERT_EQ(s->cap, cap);
  ASSERT_EQ(s->len, 1);
}

UTEST(slice, reserve_at_tip_extends) {
  enum { size = KB(16) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  for (int i = 0; i < 3; i++)
    *Push(arena, s) = i;

  // At the arena tip, reserving extends the existing block instead of copying.
  int* data = s->data;
  Reserve(arena, s, 100);
  ASSERT_TRUE(s->data == data);
  ASSERT_TRUE(s->cap >= 100);
  ASSERT_EQ(s->len, 3);
  ASSERT_EQ(s->data[2], 2);
}

UTEST(slice, clone_empty) {
  enum { size = KB(1) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* copy = New(arena, ints);
  ints* s = New(arena, ints);
  Clone(arena, copy, s);
  ASSERT_EQ(copy->len, 0);
  ASSERT_TRUE(copy->data == NULL);
}

UTEST(slice, clone_evaluates_args_once) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  for (int i = 0; i < 5; i++)
    *Push(arena, s) = i * 10;

  slice_eval_calls = 0;
  ints* full = New(arena, ints);
  Clone(arena, full, slice_eval_slice(s));
  ASSERT_EQ(slice_eval_calls, 1);
  ASSERT_EQ(full->len, 5);

  slice_eval_calls = 0;
  int start = 2;
  ints* rest = New(arena, ints);
  Clone(arena, rest, slice_eval_slice(s), start++);
  ASSERT_EQ(slice_eval_calls, 1);
  ASSERT_EQ(start, 3);
  ASSERT_EQ(rest->len, 3);
  ASSERT_EQ(rest->data[0], 20);

  // The destination and arena are evaluated once too.
  ints* tapped = New(arena, ints);
  slice_eval_calls = 0;
  Clone(slice_eval_arena(arena), slice_eval_slice(tapped), slice_eval_slice(s));
  ASSERT_EQ(slice_eval_calls, 3);  // arena, destination, source
  ASSERT_EQ(tapped->len, 5);
}

UTEST(slice, macro_args_evaluated_once) {
  enum { size = KB(8) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  slice_eval_calls = 0;
  isize idx = s->len;
  *Push(slice_eval_arena(arena), slice_eval_slice(s)) = slice_eval_int(7);
  ASSERT_EQ(slice_eval_calls, 3);  // arena, slice, value
  ASSERT_EQ(idx, 0);
  ASSERT_EQ(s->data[idx], 7);
  ASSERT_EQ(s->len, 1);

  slice_eval_calls = 0;
  Reserve(slice_eval_arena(arena), slice_eval_slice(s), slice_eval_cap(16));
  ASSERT_EQ(slice_eval_calls, 3);  // arena, slice, min_cap

  slice_eval_calls = 0;
  (void)SliceAtTip(slice_eval_arena(arena), slice_eval_slice(s));
  ASSERT_EQ(slice_eval_calls, 2);  // arena, slice

  int out = 0;
  slice_eval_calls = 0;
  bool popped = Pop(slice_eval_slice(s), slice_eval_out(&out));
  ASSERT_TRUE(popped);
  ASSERT_EQ(out, 7);
  ASSERT_EQ(slice_eval_calls, 2);  // slice, out

  fints b = {0};
  slice_eval_calls = 0;
  SliceFixed(slice_eval_arena(arena), slice_eval_fints(&b), slice_eval_cap(4));
  ASSERT_EQ(slice_eval_calls, 3);  // arena, slice, capacity

  int data[] = {1, 2};
  ints* from = New(arena, ints);
  slice_eval_calls = 0;
  SliceFromArray(slice_eval_arena(arena), slice_eval_slice(from), data);
  ASSERT_EQ(slice_eval_calls, 2);  // arena, destination
  ASSERT_EQ(from->len, 2);
  ASSERT_EQ(from->data[1], 2);
}

/* --- Slice: typed data, Push --- */

UTEST(slice, multi_token_types) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  // Multi-token element types: struct, unsigned, pointer. Slice(T) spells
  // them as written, no intermediate typedef needed.
  points* ps = New(arena, points);
  *Push(arena, ps) = ((struct Point){1, 2});
  *Push(arena, ps) = ((struct Point){3, 4});
  ASSERT_EQ(ps->len, 2);
  ASSERT_EQ(ps->data[0].x, 1);
  ASSERT_EQ(ps->data[0].y, 2);
  ASSERT_EQ(ps->data[1].x, 3);
  ASSERT_EQ(ps->data[1].y, 4);

  uints* us = New(arena, uints);
  *Push(arena, us) = 7u;
  ASSERT_EQ(us->data[0], 7u);

  const char* hello = "hello";
  strs* ss = New(arena, strs);
  *Push(arena, ss) = hello;
  ASSERT_EQ(ss->len, 1);
  ASSERT_TRUE(strcmp(ss->data[0], "hello") == 0);
}

UTEST(slice, duplicate_expansion) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  // ints and ints_again are the same element type expanded twice at file
  // scope; each expansion is usable on its own.
  ints* a = New(arena, ints);
  ints_again* b = New(arena, ints_again);
  *Push(arena, a) = 1;
  *Push(arena, b) = 2;
  ASSERT_EQ(a->len, 1);
  ASSERT_EQ(b->len, 1);
  ASSERT_EQ(a->data[0], 1);
  ASSERT_EQ(b->data[0], 2);
}

UTEST(slice, push_value_struct) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  points* ps = New(arena, points);
  struct Point p = {5, 6};
  *Push(arena, ps) = p;

  ASSERT_EQ(ps->len, 1);
  ASSERT_EQ(ps->data[0].x, 5);
  ASSERT_EQ(ps->data[0].y, 6);
}

/* --- Slice: streaming into a new element --- */
//
// Push returns a zeroed slot, so an element whose contents are *produced*
// -- read(2), inflate, decode, a struct filled field by field -- is written
// directly through the returned pointer:
//
//   Chunk* c = Push(arena, cs);
//   c->n = read(fd, c->data, sizeof c->data);
//
// Two rules: finish filling before the next growth, and after any growth reach
// the live element by index (s->data[i]) rather than through a held pointer.

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

  chunks* cs = New(arena, chunks);

  // Placeholder, then stream into it: three successive fills, as a decoder
  // would, each starting where the last stopped. Everything is reached by
  // index, so a later growth cannot strand a held pointer.
  isize ci = cs->len;
  Push(arena, cs);  // zeroed placeholder
  for (int i = 0; i < 3; i++)
    At(cs, ci).n += stream_fill(At(cs, ci).data + At(cs, ci).n, (int)sizeof At(cs, ci).data - At(cs, ci).n);

  ASSERT_EQ(cs->len, 1);
  ASSERT_EQ(cs->data[0].n, 48);
  ASSERT_EQ(cs->data[0].data[0], 'a');
  ASSERT_EQ(cs->data[0].data[15], 'a' + 15);
  ASSERT_EQ(cs->data[0].data[16], 'a');  // second fill started over
  ASSERT_EQ(cs->data[0].data[47], 'a' + 15);
}

UTEST(slice, fill_survives_grow) {
  enum { size = KB(32) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  chunks* cs = New(arena, chunks);

  isize ci = cs->len;
  Push(arena, cs);  // zeroed placeholder
  At(cs, ci).n = 42;
  memcpy(At(cs, ci).data, "filled before growth", 21);
  Chunk* stale = &cs->data[ci];  // raw pointers still go stale on a move

  // Move the slice off the arena tip, so the growth below cannot extend in
  // place and has to memmove the elements to fresh storage.
  New(arena, int, 64);
  for (int i = 0; i < 100; i++)
    *Push(arena, cs) = (Chunk){0};

  ASSERT_EQ(cs->len, 101);
  ASSERT_EQ(At(cs, ci).n, 42);  // the fill rode along with the move
  ASSERT_TRUE(strcmp(At(cs, ci).data, "filled before growth") == 0);
  ASSERT_TRUE(stale != &cs->data[0]);  // ...while raw pointers name the pre-move copy

  // From here on the live element is reached by index.
  At(cs, 0).data[0] = 'F';
  ASSERT_EQ(At(cs, 0).data[0], 'F');
}

UTEST(slice, data_is_typed) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  ints* s = New(arena, ints);
  *Push(arena, s) = 7;

  // data is T*, so elements are reachable without a macro or a cast: s->data[i]
  // is an assignable lvalue and binds to T* directly. Writes to the field are
  // checked too (-Wincompatible-pointer-types) -- see
  // test/typecheck/fail/slice_data_wrong_pointer_type.c.check.
  s->data[0] += 1;
  ASSERT_EQ(s->data[0], 8);

  s->data[0] = 42;
  ASSERT_EQ(s->data[0], 42);

  int* p = s->data;
  ASSERT_TRUE(p == &s->data[0]);
}

UTEST(slice, layout_zero_overhead) {
  // Slice(T) must stay byte-identical to the erased layout arena_slice_reserve
  // memcpys, whatever the element type.
  _Static_assert(sizeof(ints) == sizeof(SliceInternal), "slice size");
  _Static_assert(alignof(ints) == alignof(SliceInternal), "slice align");
  _Static_assert(offsetof(ints, data) == offsetof(SliceInternal, data), "data offset");
  _Static_assert(offsetof(ints, len) == offsetof(SliceInternal, len), "len offset");
  _Static_assert(offsetof(ints, cap) == offsetof(SliceInternal, cap), "cap offset");
  _Static_assert(sizeof(SliceInternal) == sizeof(void*) + 2 * sizeof(isize), "three words");
}
