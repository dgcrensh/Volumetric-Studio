# 0004: Share the Linux vcpkg binary cache outside Actions

Nov 19, 2026 · between M2 and M3

## Context

Development is moving partly to hosted Linux cloud sessions. Each one starts
from a fresh container, and building Open CASCADE through vcpkg takes over an
hour. CI already keeps vcpkg's binary cache in `actions/cache`, but that cache
can only be downloaded from inside a GitHub Actions job: the REST API lists and
deletes entries, it doesn't serve their contents.

A NuGet feed on GitHub Packages was the first idea. The cloud sessions'
network policy blocks `nuget.pkg.github.com`, and the feed would also need a
classic PAT with `read:packages` stored in the cloud environment.

## Decision

- The `linux-gcc-release` CI job, on pushes to `main` and manual runs, runs
  `scripts/vcpkg-cache.sh publish` after saving `actions/cache`. It tars the
  vcpkg "files" cache, keeping only the archives the build installed, and
  uploads it (split under the 2 GiB asset limit) to the prerelease on the fixed
  tag `vcpkg-cache-x64-linux`. Pull requests never publish.
- Machines outside Actions run `scripts/vcpkg-cache.sh fetch` (or
  `scripts/cloud-setup.sh`), which downloads the assets through the GitHub API
  with the session's existing GitHub token and unpacks them into vcpkg's
  default files cache (`~/.cache/vcpkg/archives`). No vcpkg configuration
  changes; `actions/cache` stays CI's own cache.
- Archives are keyed by vcpkg's ABI hash, which covers the vcpkg commit, the
  triplet, the port's dependencies, the compiler binary and the cmake version
  vcpkg downloads. Measured on Nov 19, 2026: a cloud session (Ubuntu 24.04,
  GCC 13.3.0) computes the same ABIs as the `ubuntu-24.04` runner, so it hits.
  A machine with another compiler misses and builds from source as before.

## Consequences

- The repository gains a `vcpkg-cache-x64-linux` tag and prerelease that
  isn't a product release. CI overwrites its assets on every `main` build.
- Windows is unchanged: its cache stays in `actions/cache` only.
- If the runner image and the cloud image drift apart (a GCC security update
  on one side), cloud sessions silently start building OCCT again. Compare the
  `package ABI` lines in the CI log with a local `vcpkg install --dry-run --debug`
  to diagnose.
