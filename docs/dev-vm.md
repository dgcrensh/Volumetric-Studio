# A Windows GPU dev VM on Google Compute Engine

A second Windows machine with an NVIDIA GPU, for running the app and the
`[gui]` and GPU tests away from the desktop. With the scripts below, standing
one up is one command and about an hour of unattended work, most of it the
Visual Studio Build Tools install. Open CASCADE and Qt come prebuilt from the
desktop instead of taking hours to compile (decision 0019).

## Once per Google Cloud project (manual)

These can't be scripted:

1. **Billing.** A free-trial account gets no GPUs. Upgrade it to a paid
   account in the console (Billing > Overview > Upgrade).
2. **GPU quota.** In IAM & Admin > Quotas, request at least 1 for
   *GPUs (all regions)* (`GPUS_ALL_REGIONS`), and at least 1 for
   *NVIDIA L4 Virtual Workstation GPUs* (`NVIDIA_L4_VWS_GPUS`) or
   *NVIDIA T4 Virtual Workstation GPUs* (`NVIDIA_T4_VWS_GPUS`) in the regions
   you'd use. Requests are usually granted within minutes to a day. The
   create script checks these and names the one that's missing.
3. **gcloud** on the machine you create the VM from:
   [install it](https://cloud.google.com/sdk/docs/install), then
   `gcloud auth login` and `gcloud config set project <project id>`.

## Create the VM

```powershell
powershell -ExecutionPolicy Bypass -File scripts\gcp\New-DevVm.ps1 -WindowsUser dallas
```

It restricts RDP to your current public IP (firewall rule
`vstudio-allow-rdp`, re-pointed on every run, so run it again or pass
`-AllowRdpFrom` when your IP changes), then tries an L4 on a `g2-standard-8`
and falls back to a T4 on an `n1-standard-8`, zone by zone, preferred US
regions first, skipping regions without quota and moving on when a zone is
out of GPUs. The `-vws` GPU types carry the RTX Virtual Workstation (GRID)
licence. Useful options: `-Gpus T4`, `-Regions us-east1,us-west1`,
`-OnlyPreferredRegions`, `-NoBuild`, `-Branch <branch>`, `-Name <vm>`.
`-WindowsUser` creates that user and prints its password.

The boot disk is Windows Server 2022 Datacenter (Desktop Experience), 200 GB
balanced. The VM's first boot runs `scripts\gcp\bootstrap-windows.ps1`:

| Step | What | Time |
| --- | --- | --- |
| GRID driver | Newest Server 2022 driver from Google's `nvidia-drivers-us-public` bucket | 5 min |
| git, gh | Latest Git for Windows and GitHub CLI | 2 min |
| Clone | `C:\src\Volumetric-Studio` | 1 min |
| Build Tools | VS 2022 Build Tools with the toolset pinned in `scripts\windows-toolchain.json` | 20-30 min |
| vcpkg | `C:\vcpkg` at the baseline; cache in `C:\vcpkg-cache` | 2 min |
| Cache | `scripts\vcpkg-cache.ps1 fetch` (2.8 GB) | 2-5 min |
| Build | `windows-msvc-app`, dependencies restored, not compiled | 10-15 min |

Follow it with `gcloud compute instances get-serial-port-output vstudio-dev
--zone <zone>`, or after signing in in `C:\vstudio-bootstrap\bootstrap.log`.
It finishes by writing `vstudio-next-steps.txt` on the desktop, which says
whether each step worked (in particular whether the cache hit) and reboots
once if the driver asked for it.

## After the bootstrap (manual)

RDP in as the user `-WindowsUser` created, then:

1. `gh auth login` (browser sign-in), then `gh auth setup-git` so `git push`
   works.
2. Install the Claude desktop app from <https://claude.ai/download> and sign
   in.
3. Run the tests once:
   `cd C:\src\Volumetric-Studio; powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Preset windows-msvc-app`.
   `[gui]` tests need the RDP session to be open.

`nvidia-smi` should list the L4 or T4. The app's About box says whether it
samples fields on the GPU.

Stop the VM when idle (`gcloud compute instances stop vstudio-dev --zone
<zone>`); a stopped VM bills only its disk. Delete it with
`gcloud compute instances delete`.

## Keeping the cache current (desktop)

The VM only skips the long builds if the desktop has published archives for
the same toolset and vcpkg baseline. After anything that changes them (a new
dependency, a vcpkg baseline bump, a Visual Studio update), build the app
preset on the desktop and publish:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Preset windows-msvc-app -SkipTests
powershell -ExecutionPolicy Bypass -File scripts\vcpkg-cache.ps1 publish
```

`scripts\vcpkg-cache.ps1 key` prints this machine's key. The release
`vcpkg-cache-x64-windows` lists every published key.

### Toolset drift

vcpkg keys every archive by the exact `cl.exe`. The VM installs the newest
VS 2022 (17.14) Build Tools. If Microsoft ships a 17.14 servicing update with
a new compiler build, the VM gets MSVC `14.44.x` other than the pinned one,
`vstudio-next-steps.txt` says so and the build compiles everything. Fix it
on the desktop: update Visual Studio, rebuild the app preset (hours, once),
set `msvc` in `scripts\windows-toolchain.json` to the new version and
publish. Don't install a second toolset on either machine: `build.ps1` and
vcpkg use the newest.
