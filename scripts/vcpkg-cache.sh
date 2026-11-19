#!/usr/bin/env bash
# Shares the Linux vcpkg binary cache (Open CASCADE and the other ports) between
# GitHub Actions and machines outside Actions, such as cloud dev sessions.
#
#   scripts/vcpkg-cache.sh publish <cache-dir> [<vcpkg-installed-dir>]   # CI
#   scripts/vcpkg-cache.sh fetch [<cache-dir>]                           # anywhere
#
# CI keeps the cache in actions/cache, which only an Actions job can read. So
# the linux-gcc-release job also publishes it as assets of a prerelease on the
# fixed tag "vcpkg-cache-x64-linux" in this repository. "fetch" downloads those
# assets into vcpkg's default files cache, where the next configure finds them.
#
# The cache is vcpkg's "files" layout: <cache-dir>/<abi[0:2]>/<abi>.zip, where
# <abi> is the sha256 of the port's vcpkg_abi_info.txt. When the installed tree
# is given, publish keeps only the archives it uses, so stale ones restored
# through actions/cache restore-keys don't pile up in the release.
#
# A hit needs the same vcpkg commit, triplet (x64-linux), compiler binary and
# vcpkg-downloaded cmake as CI: today that is Ubuntu 24.04 with its GCC 13.3.
# On a miss vcpkg just builds from source, so a mismatch costs time, not
# correctness.
#
# publish needs gh and GH_TOKEN with contents: write. fetch uses curl and
# authenticates with GH_TOKEN or GITHUB_TOKEN when set (the repo is private).
# Environment overrides: VS_CACHE_REPO (owner/name, default: from GITHUB_REPOSITORY
# or the origin remote), VS_CACHE_TAG (default vcpkg-cache-x64-linux).

set -euo pipefail

tag="${VS_CACHE_TAG:-vcpkg-cache-x64-linux}"
part_size="1900m"          # release assets are limited to 2 GiB each
asset_prefix="vcpkg-cache-x64-linux.tar.part"

die() { echo "vcpkg-cache.sh: $*" >&2; exit 1; }

repo_slug() {
  if [[ -n "${VS_CACHE_REPO:-}" ]]; then echo "$VS_CACHE_REPO"; return; fi
  if [[ -n "${GITHUB_REPOSITORY:-}" ]]; then echo "$GITHUB_REPOSITORY"; return; fi
  local url
  url="$(git -C "$(dirname "${BASH_SOURCE[0]}")" remote get-url origin 2>/dev/null)" \
    || die "set VS_CACHE_REPO=owner/name"
  url="${url%.git}"
  # https://github.com/o/r, git@github.com:o/r, or a proxied URL ending in /o/r
  url="${url##*github.com[:/]}"
  echo "$url" | awk -F/ '{print $(NF-1) "/" $NF}'
}

publish() {
  local cache_dir="${1:?publish needs <cache-dir>}" installed_dir="${2:-}"
  [[ -d "$cache_dir" ]] || die "no cache directory at $cache_dir"
  command -v gh >/dev/null || die "publish needs gh"
  local repo work list
  repo="$(repo_slug)"
  work="$(mktemp -d)"
  trap 'rm -rf "$work"' RETURN
  list="$work/files.txt"

  if [[ -n "$installed_dir" ]] && compgen -G "$installed_dir/*/share/*/vcpkg_abi_info.txt" >/dev/null; then
    local info abi
    for info in "$installed_dir"/*/share/*/vcpkg_abi_info.txt; do
      abi="$(sha256sum "$info" | cut -d' ' -f1)"
      if [[ -f "$cache_dir/${abi:0:2}/$abi.zip" ]]; then
        echo "${abi:0:2}/$abi.zip" >> "$list"
      else
        echo "warning: no archive for $(basename "$(dirname "$info")") ($abi)" >&2
      fi
    done
  else
    (cd "$cache_dir" && find . -name '*.zip' -printf '%P\n') > "$list"
  fi
  [[ -s "$list" ]] || die "nothing to publish from $cache_dir"
  sort -o "$list" "$list"
  cp "$list" "$work/MANIFEST.txt"

  echo "Publishing $(wc -l < "$list") archive(s) to release $tag of $repo"
  tar -C "$cache_dir" -cf - -T "$list" | split -b "$part_size" -d -a 2 - "$work/$asset_prefix"
  ls -l "$work"

  if ! gh release view "$tag" -R "$repo" >/dev/null 2>&1; then
    gh release create "$tag" -R "$repo" --prerelease --target "${GITHUB_SHA:-main}" \
      --title "vcpkg binary cache (x64-linux)" \
      --notes "Prebuilt vcpkg ports (Open CASCADE etc.) for Linux, published by CI. Fetch with scripts/vcpkg-cache.sh fetch. Not a product release."
  fi
  gh release upload "$tag" -R "$repo" --clobber "$work/MANIFEST.txt" "$work/$asset_prefix"*

  # Remove parts left over from an earlier, larger upload.
  local name
  gh release view "$tag" -R "$repo" --json assets --jq '.assets[].name' | while read -r name; do
    if [[ "$name" == "$asset_prefix"* && ! -f "$work/$name" ]]; then
      gh release delete-asset "$tag" "$name" -R "$repo" -y
    fi
  done
}

fetch() {
  local cache_dir="${1:-${VCPKG_DEFAULT_BINARY_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/vcpkg/archives}}"
  local repo token api work
  repo="$(repo_slug)"
  token="${GH_TOKEN:-${GITHUB_TOKEN:-}}"
  api="https://api.github.com/repos/$repo"
  local -a auth=()
  [[ -n "$token" ]] && auth=(-H "Authorization: Bearer $token")
  work="$(mktemp -d)"
  trap 'rm -rf "$work"' RETURN

  curl -fsSL "${auth[@]}" -H "Accept: application/vnd.github+json" \
    "$api/releases/tags/$tag" -o "$work/release.json" \
    || die "could not read release $tag of $repo (no cache published yet, or no access)"

  # id<TAB>name for each part, in order.
  python3 - "$work/release.json" "$asset_prefix" > "$work/assets.tsv" <<'PY'
import json, sys
release = json.load(open(sys.argv[1]))
parts = sorted((a["name"], a["id"]) for a in release["assets"] if a["name"].startswith(sys.argv[2]))
for name, asset_id in parts:
    print(f"{asset_id}\t{name}")
PY
  [[ -s "$work/assets.tsv" ]] || die "release $tag has no cache parts"

  mkdir -p "$cache_dir"
  echo "Fetching vcpkg binary cache from $repo ($tag) into $cache_dir"
  local id name
  {
    while IFS=$'\t' read -r id name; do
      echo "  $name" >&2
      curl -fsSL "${auth[@]}" -H "Accept: application/octet-stream" "$api/releases/assets/$id"
    done < "$work/assets.tsv"
  } | tar -C "$cache_dir" -xf -
  echo "Cache has $(find "$cache_dir" -name '*.zip' | wc -l) archive(s)."
}

case "${1:-}" in
  publish) shift; publish "$@" ;;
  fetch) shift; fetch "$@" ;;
  *) sed -n '2,25p' "$0"; exit 2 ;;
esac
