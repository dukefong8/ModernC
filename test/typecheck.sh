#!/usr/bin/env bash
#
# Compile-time type-safety checks for include/arena.h slices. Runtime tests
# cannot prove a program fails to compile, so these are fixtures:
#
#   pass/*.c.check  must compile under -Werror
#   fail/*.c.check  must NOT compile under -Werror, and the diagnostic must
#                   match the fixture's first line, "// EXPECT: <regex>", so
#                   a fixture that breaks for an unrelated reason still fails
#                   the check.
#
# Fixtures use a non-.c extension on purpose: the Makefile builds every .c file
# found anywhere in the tree (SRC := find . -name '*.c'), and these must never
# be part of the normal build. Run with `make typecheck`.

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

CC=${CC:-cc}
read -r -a CC_CMD <<<"$CC"   # CC may be multi-word ("ccache clang"), as in make
CFLAGS=(-x c -fsyntax-only -Werror -Wall -Wextra -Wnull-dereference -Wvla -Wformat=2
        -Wno-format-nonliteral -Wno-unused-parameter -Wno-unused-function
        -Iinclude -D_GNU_SOURCE -DOOM_COMMIT -DDEFAULT_ARENA_SIZE=4000000000)

npass=0
nreject=0
status=0

check_pass() {
  local file=$1 out
  if out=$("${CC_CMD[@]}" "${CFLAGS[@]}" "$file" 2>&1); then
    printf '  ok    %s\n' "$file"
    npass=$((npass + 1))
  else
    printf '  FAIL  %s (expected to compile)\n%s\n' "$file" "$out"
    status=1
  fi
}

check_fail() {
  local file=$1 expect out
  expect=$(sed -n '1s|^// EXPECT: ||p' "$file")
  if [[ -z $expect ]]; then
    printf '  FAIL  %s (first line must be "// EXPECT: <regex>")\n' "$file"
    status=1
    return
  fi
  if out=$("${CC_CMD[@]}" "${CFLAGS[@]}" "$file" 2>&1); then
    printf '  FAIL  %s (compiled, but a diagnostic was expected)\n' "$file"
    status=1
  elif ! grep -Eq -- "$expect" <<<"$out"; then
    printf '  FAIL  %s (rejected for the wrong reason; wanted /%s/)\n%s\n' \
      "$file" "$expect" "$out"
    status=1
  else
    printf '  ok    %s (rejected: /%s/)\n' "$file" "$expect"
    nreject=$((nreject + 1))
  fi
}

echo "typecheck: must compile"
for f in test/typecheck/pass/*.c.check; do check_pass "$f"; done

echo "typecheck: must be rejected"
for f in test/typecheck/fail/*.c.check; do check_fail "$f"; done

printf 'typecheck: %d compiled, %d rejected\n' "$npass" "$nreject"
exit $status
