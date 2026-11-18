#!/usr/bin/env bash
# Configures, builds and tests Volumetric Studio on Linux in one command.
#
#   scripts/build.sh                      # linux-gcc-release
#   scripts/build.sh linux-clang-debug    # any configure preset
#   scripts/build.sh --clean [preset]     # delete the build directory first
#   scripts/build.sh --skip-tests [preset]
#
# Needs: git, cmake (3.25+), ninja, a C++20 compiler (GCC 12+ or Clang 16+),
# and the usual vcpkg prerequisites (curl, zip, unzip, tar, pkg-config).
#
# Uses $VCPKG_ROOT when it is a git clone of vcpkg. Otherwise clones vcpkg into
# ${XDG_CACHE_HOME:-~/.cache}/vstudio/vcpkg at the commit pinned in vcpkg.json
# ("builtin-baseline") and bootstraps it.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preset="linux-gcc-release"
clean=0
skip_tests=0

for arg in "$@"; do
  case "$arg" in
    --clean) clean=1 ;;
    --skip-tests) skip_tests=1 ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    -*) echo "build.sh: unknown option '$arg'" >&2; exit 2 ;;
    *) preset="$arg" ;;
  esac
done

for tool in git cmake ninja; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "build.sh: '$tool' is required but not on PATH" >&2
    exit 1
  fi
done

baseline="$(sed -n 's/.*"builtin-baseline"[[:space:]]*:[[:space:]]*"\([0-9a-f]*\)".*/\1/p' "$repo_root/vcpkg.json")"
if [[ -z "$baseline" ]]; then
  echo "build.sh: could not read builtin-baseline from vcpkg.json" >&2
  exit 1
fi

managed=0
if [[ -n "${VCPKG_ROOT:-}" && -d "$VCPKG_ROOT/.git" ]]; then
  root="$VCPKG_ROOT"
else
  managed=1
  root="${XDG_CACHE_HOME:-$HOME/.cache}/vstudio/vcpkg"
  if [[ ! -d "$root/.git" ]]; then
    echo "Cloning vcpkg into $root"
    mkdir -p "$(dirname "$root")"
    git clone https://github.com/microsoft/vcpkg "$root"
  fi
fi

if ! git -C "$root" cat-file -e "${baseline}^{commit}" 2>/dev/null; then
  echo "Fetching vcpkg baseline $baseline"
  git -C "$root" fetch origin
fi
if [[ $managed -eq 1 && "$(git -C "$root" rev-parse HEAD)" != "$baseline" ]]; then
  git -C "$root" checkout --quiet "$baseline"
  rm -f "$root/vcpkg"
fi
if [[ ! -x "$root/vcpkg" ]]; then
  "$root/bootstrap-vcpkg.sh" -disableMetrics
fi
export VCPKG_ROOT="$root"
echo "Using vcpkg at $root"

cd "$repo_root"
if [[ $clean -eq 1 ]]; then
  rm -rf "build/$preset"
fi

cmake --preset "$preset"
cmake --build --preset "$preset"
if [[ $skip_tests -eq 0 ]]; then
  ctest --preset "$preset"
fi
echo
echo "Build and tests succeeded ($preset)."
