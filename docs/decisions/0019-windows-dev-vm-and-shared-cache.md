# 0019: A scripted Windows dev VM, fed by the desktop's vcpkg cache

Nov 20, 2026 · after L1

## Context

Dallas ran a second Windows machine on Compute Engine (Server 2022, L4 with
the RTX Virtual Workstation driver) for GPU work. Setting it up by hand took
most of a day: finding a zone with L4s (none, then a T4), finding the GRID
driver, installing Visual Studio, Qt, git, gh and the Claude app, and a
multi-hour cold build of Open CASCADE and Qt. The only prebuilt Windows
dependencies were CI's, in `actions/cache` (readable only by Actions) and
built by the runner's Visual Studio, so they wouldn't have hit anyway.

vcpkg names each binary archive by an ABI hash. On this desktop's
`vcpkg_abi_info.txt` that covers the port files, the vcpkg commit's scripts,
the cmake and PowerShell vcpkg downloads (both fixed by the vcpkg commit),
the triplet file and a hash of the compiler binary. Two machines hit each
other's archives when they have the same vcpkg commit and the same `cl.exe`.
The Windows SDK version is not in the hash.

## Decision

- **Pin the toolset.** `scripts/windows-toolchain.json` names the Visual
  Studio release (17.14), MSVC build (14.44.35207, the desktop's), Windows
  SDK and Build Tools components. The VM bootstrap installs them;
  `build.ps1` warns when the newest local toolset isn't the pinned one.
- **Publish from the desktop to a GitHub release.** `scripts/vcpkg-cache.ps1
  publish` packs the archives used by the desktop's `build/windows-msvc-*`
  trees (24 archives, 2.8 GB with OCCT and Qt) and uploads them in parts
  under 2 GiB to the prerelease `vcpkg-cache-x64-windows`, like the Linux
  cache (decision 0004). Assets are named by a key,
  `msvc<toolset>-vcpkg<baseline[0:12]>`, with a manifest listing each
  archive's package and the triplet ABI lines. `fetch` takes only the parts
  matching the local key, and lists the published keys when none matches, so
  a toolset mismatch shows up before the build instead of an hour into it.
  A GitHub release needs nothing new on either machine (gh is signed in on
  the desktop, and a public release downloads without a token); a GCS
  bucket would need gcloud credentials on both.
- **Script the VM.** `scripts/gcp/New-DevVm.ps1` checks GPU quota, limits RDP
  to the caller's IP, and tries L4 then T4 virtual workstation GPUs zone by
  zone. It attaches `scripts/gcp/bootstrap-windows.ps1` as the startup
  script, which installs the GRID driver (listed from Google's driver bucket
  with the VM's service account), git, gh, the pinned Build Tools and vcpkg
  (machine-wide `VCPKG_ROOT=C:\vcpkg`,
  `VCPKG_DEFAULT_BINARY_CACHE=C:\vcpkg-cache`), clones to a path without
  spaces, fetches the cache and builds the app. `docs/dev-vm.md` has the
  steps that stay manual: billing upgrade and GPU quota, `gh auth login`,
  the Claude app.

## Consequences

- The release holds statically linked Qt and OCCT binaries built from
  vcpkg's sources. It isn't a product release.
- When the desktop's Visual Studio updates to a new compiler build, the pin
  and the published key must follow (one long rebuild on the desktop, then
  `publish`). Until then a new VM builds from source; its next-steps file
  says so.
- The Build Tools bootstrapper (`aka.ms/vs/17/release`) installs the newest
  17.14. If a servicing update changes the compiler, VMs drift from the
  desktop until it is updated too; see "Toolset drift" in `docs/dev-vm.md`.
- CI's Windows job is unchanged and still uses `actions/cache`.
