#!/usr/bin/env bash
# Prepares a fresh Linux machine (for example a hosted cloud dev session on
# Ubuntu 24.04) to build Volumetric Studio without building Open CASCADE:
#
#   1. installs the build tools and the OpenGL/X11 headers if they are missing,
#   2. fetches the vcpkg binary cache that CI publishes (scripts/vcpkg-cache.sh),
#   3. unless --no-build is given, builds linux-gcc-release without running the
#      tests (build.sh clones and bootstraps vcpkg at the pinned baseline).
#
#   scripts/cloud-setup.sh              # set up, then configure and build linux-gcc-release
#   scripts/cloud-setup.sh --no-build   # set up only
#
# Safe to run again; each step skips work that is already done.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build=1
for arg in "$@"; do
  case "$arg" in
    --no-build) build=0 ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) echo "cloud-setup.sh: unknown argument '$arg'" >&2; exit 2 ;;
  esac
done

packages=(git cmake ninja-build g++ clang pkg-config curl zip unzip tar python3
  libgl1-mesa-dev libglu1-mesa-dev libegl1-mesa-dev
  libx11-dev libxext-dev libxi-dev libxmu-dev)
missing=()
for p in "${packages[@]}"; do
  dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "install ok installed" || missing+=("$p")
done
if (( ${#missing[@]} )); then
  echo "Installing: ${missing[*]}"
  sudo_cmd=()
  [[ $(id -u) -ne 0 ]] && sudo_cmd=(sudo)
  "${sudo_cmd[@]}" apt-get update -q
  DEBIAN_FRONTEND=noninteractive "${sudo_cmd[@]}" apt-get install -y -q "${missing[@]}"
fi

if ! "$repo_root/scripts/vcpkg-cache.sh" fetch; then
  echo "cloud-setup.sh: no prebuilt cache; the first build will compile Open CASCADE (over an hour)." >&2
fi

if (( build )); then
  "$repo_root/scripts/build.sh" --skip-tests linux-gcc-release
fi
