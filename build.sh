#!/bin/sh
# Build the model.
#
#   ./build.sh            everything, then run both test binaries
#   ./build.sh static     libvirtualsx1262.a       - for C and C++ hosts
#   ./build.sh shared     libvirtualsx1262.{so,dylib,dll} - for hosts loading at runtime
#   ./build.sh test       build and run the tests
#   ./build.sh sanitize   the tests under AddressSanitizer and UBSan
#   ./build.sh pedantic   the conversion and cast audit still owed on the model
#
# STRICT=1 turns warnings into errors, which is what CI uses. It is off by
# default so a host bisecting an old compiler is not blocked by a new warning.
set -e

OUT=${OUT:-build}
CXX=${CXX:-c++}
CC=${CC:-cc}

# The warnings that find bugs. These are errors in CI.
WARN="-Wall -Wextra -Wpedantic -Wshadow"
[ -n "$STRICT" ] && WARN="$WARN -Werror"

# -Wconversion, -Wsign-conversion and -Wold-style-cast are deliberately NOT in
# that set. The model is register work: packing datasheet fields into bytes is
# narrowing on purpose, and a C-style cast reads the way the datasheet does.
# Turning them on means auditing every narrowing in the model to tell the
# deliberate ones from the accidental ones, which is real work and owed, not a
# flag flip. `./build.sh pedantic` runs them so the backlog is visible instead
# of hidden, and it is not wired into CI until that audit is done.
PEDANTIC_WARN="-Wconversion -Wsign-conversion -Wold-style-cast"

CXXFLAGS="-std=c++17 -O2 -Iinclude -Isrc $WARN ${CXXFLAGS}"
# The C test exists to prove the header is valid C. Held to the same standard the
# hosts compile at: QEMU is C11, and -Wpedantic catches a C++ism slipping in.
CFLAGS_C="-std=c11 -O2 -Iinclude -Wall -Wextra -Wpedantic ${CFLAGS}"
[ -n "$STRICT" ] && CFLAGS_C="$CFLAGS_C -Werror"

SRC="src/VirtualSX1262.cpp src/spi.cpp src/abi.cpp"

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) SHARED_EXT=dll ;;
  Darwin)               SHARED_EXT=dylib ;;
  *)                    SHARED_EXT=so ;;
esac

mkdir -p "$OUT"

# -fno-exceptions is deliberate: the ABI reports failure by return value, and an
# exception unwinding into C or across a P/Invoke boundary is undefined.
build_static() {
  for f in $SRC; do
    $CXX $CXXFLAGS -fno-exceptions -c "$f" -o "$OUT/$(basename "${f%.cpp}").o"
  done
  ar rcs "$OUT/libvirtualsx1262.a" "$OUT"/*.o
  echo "built $OUT/libvirtualsx1262.a"
}

build_shared() {
  $CXX $CXXFLAGS -fPIC -fno-exceptions -shared $SRC -o "$OUT/libvirtualsx1262.$SHARED_EXT"
  echo "built $OUT/libvirtualsx1262.$SHARED_EXT"
}

build_tests() {
  $CXX $CXXFLAGS $SRC test/test_model.cpp -o "$OUT/test_model"
  # Compiled by the C compiler against the static library, which is the whole
  # point: a header that only works in C++ fails QEMU and Renode, not this.
  build_static >/dev/null
  # -lm and -lstdc++ by hand: the C driver links neither implicitly, and the
  # model uses fmax while the ABI is C++ underneath.
  $CC $CFLAGS_C test/test_c_abi.c "$OUT/libvirtualsx1262.a" -lstdc++ -lm -o "$OUT/test_c_abi"
  echo "built $OUT/test_model $OUT/test_c_abi"
}

run_tests() {
  "$OUT/test_model"
  "$OUT/test_c_abi"
}

# Sequential, never `a && b`: set -e is suspended inside the left operand of an
# && list, so a failing compile there printed an error and carried on to run a
# binary that was never built.
case "${1:-all}" in
  static) build_static ;;
  shared) build_shared ;;
  test)   build_tests; run_tests ;;
  pedantic)
    # Expected to fail today. It exists to measure the audit, not to gate it.
    $CXX $CXXFLAGS $PEDANTIC_WARN -fsyntax-only $SRC test/test_model.cpp
    ;;
  sanitize)
    SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
    $CXX $CXXFLAGS $SAN $SRC test/test_model.cpp -o "$OUT/test_model_san"
    # The C test gets the same treatment. Its own stack overflow is what proved
    # this needed to cover both binaries rather than just the C++ one.
    $CC $CFLAGS_C $SAN -Iinclude test/test_c_abi.c $SRC -lstdc++ -lm -o "$OUT/test_c_abi_san" 2>/dev/null \
      || $CXX $CXXFLAGS $SAN -x c++ test/test_c_abi.c $SRC -o "$OUT/test_c_abi_san"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$OUT/test_model_san"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$OUT/test_c_abi_san"
    ;;
  all)    build_static; build_shared; build_tests; run_tests ;;
  *)      echo "build.sh: unknown target $1" >&2; exit 2 ;;
esac
