#!/bin/sh
# Build the model: a static library for C/C++ hosts, a shared one for managed
# hosts that load it at runtime, and the tests.
#
#   ./build.sh            static + shared + tests, then run the tests
#   ./build.sh static     just libvirtualsx1262.a
#   ./build.sh shared     just libvirtualsx1262.so (.dll on Windows)
#   ./build.sh test       build and run the tests
set -e

OUT=${OUT:-build}
CXX=${CXX:-c++}
CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc ${CXXFLAGS}"
SRC="src/VirtualSX1262.cpp src/abi.cpp"

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) SHARED_EXT=dll ;;
  Darwin)               SHARED_EXT=dylib ;;
  *)                    SHARED_EXT=so ;;
esac

mkdir -p "$OUT"

build_static() {
  # -fno-exceptions is deliberate: the ABI reports failure by return value, so
  # an exception crossing into C or C# would be undefined either way.
  for f in $SRC; do
    $CXX $CXXFLAGS -fno-exceptions -c "$f" -o "$OUT/$(basename "${f%.cpp}").o"
  done
  ar rcs "$OUT/libvirtualsx1262.a" "$OUT"/*.o
  echo "$OUT/libvirtualsx1262.a"
}

build_shared() {
  $CXX $CXXFLAGS -fPIC -fno-exceptions -shared $SRC -o "$OUT/libvirtualsx1262.$SHARED_EXT"
  echo "$OUT/libvirtualsx1262.$SHARED_EXT"
}

build_test() {
  $CXX $CXXFLAGS $SRC test/test_model.cpp -o "$OUT/test_model"
  echo "$OUT/test_model"
}

case "${1:-all}" in
  static) build_static ;;
  shared) build_shared ;;
  test)   build_test && "$OUT/test_model" ;;
  all)    build_static && build_shared && build_test && "$OUT/test_model" ;;
  *)      echo "build.sh: unknown target $1" >&2; exit 2 ;;
esac
