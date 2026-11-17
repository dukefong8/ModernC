// clang-format off
#pragma once

#include "arena.h"
#include "utest.h"
#include <math.h>

#include "datatype99.h"

#define EXPR(expr)       New(arena, Expr, 1, (Expr[]){expr})
#define OP(lhs, op, rhs) EXPR(op(EXPR(lhs), EXPR(rhs)))

datatype(
    Expr,
    (Const, double),
    (Add, Expr *, Expr *),
    (Sub, Expr *, Expr *),
    (Mul, Expr *, Expr *),
    (Div, Expr *, Expr *),
    (Mod, Expr *, Expr *)
);

static double eval(const Expr *expr) {
  match(*expr) {
    of(Const, number) return *number;
    of(Add, lhs, rhs) return eval(*lhs) + eval(*rhs);
    of(Sub, lhs, rhs) return eval(*lhs) - eval(*rhs);
    of(Mul, lhs, rhs) return eval(*lhs) * eval(*rhs);
    of(Div, lhs, rhs) return eval(*lhs) / eval(*rhs);
    of(Mod, lhs, rhs) return fmod(eval(*lhs), eval(*rhs));
  }

  Assert("Invalid expr");
  return -1;
}

static Expr *expr(Arena *arena) {
  return OP(*OP(*OP(Const(53),
                    Add,
                    Const(5)),
                Sub,
                Const(10)),
            Mod,
            *OP(Const(3),
                Add,
                Const(5)));
}

UTEST(datatype99, adt) {
  enum { size = KB(1) };
  byte mem[size] = {0};
  Arena arena = arena_init(mem, size);
  ASSERT_EQ(0.0, eval(expr(&arena)));
}
