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
   create script stops if `GPUS_ALL_REGIONS` is 0. It only reports the
   regional numbers, since gcloud doesn't always list the vWS quota (or
   reads 0 where the console still creates the VM); a zone that refuses
   for quota is skipped like a full one.
3. **gcloud** on the machine you create the VM from (macOS, Linux or
   Windows): [install it](https://cloud.google.com/sdk/docs/install), then
   `gcloud auth login` and `gcloud config set project <project id>`.

## Create the VM

There are two equivalent scripts; use whichever fits the machine. Neither
needs anything but gcloud (and curl, for the bash one), and both pass the
same Windows bootstrap to the VM.

On macOS or Linux (bash 3.2 or later, so the stock macOS bash works; no
PowerShell needed):

```bash
scripts/gcp/new-dev-vm.sh --windows-user dallas
scripts/gcp/new-dev-vm.sh --dry-run    # check quota, zones and firewall first; creates nothing
```

On Windows:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\gcp\New-DevVm.ps1 -WindowsUser dallas
```

It restricts RDP to your current public IP (firewall rule
`vstudio-allow-rdp`, re-pointed on every run, so run it again or pass
`--allow-rdp-from` / `-AllowRdpFrom` when your IP changes), then tries an L4
on a `g2-standard-8` and falls back to a T4 on an `n1-standard-8`, zone by
zone, preferred US regions first, moving on when a zone is out of GPUs or
refuses for quota. The `-vws` GPU types carry the RTX Virtual
Workstation (GRID) licence. Every L4 region is tried before the first T4,
including ones outside the US; pass `--only-preferred-regions` to stay in
the preferred ones.

| bash | PowerShell | What |
| --- | --- | --- |
| `--gpus T4` | `-Gpus T4` | GPUs to try, in order (default L4 then T4) |
| `--regions us-east1,us-west1` | `-Regions us-east1,us-west1` | regions tried first |
| `--only-preferred-regions` | `-OnlyPreferredRegions` | never try other regions |
| `--no-build` | `-NoBuild` | bootstrap stops after fetching the cache |
| `--branch <branch>` | `-Branch <branch>` | branch the VM clones and builds |
| `--name <vm>` | `-Name <vm>` | VM name (default `vstudio-dev`) |
| `--windows-user <name>` | `-WindowsUser <name>` | create that user and print its password |
| `--dry-run` | | run only the read-only checks and print what it would create |

Both use gcloud's configured project unless you pass `--project <id>` /
`-Project <id>`; the GPU quota is per project, so point it at the project
that has it (or `gcloud config set project <id>` once).

`scripts/gcp/new-dev-vm.sh --help` lists the rest (machine types, disk size,
project, repository URL).

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

RDP in as the user `--windows-user` / `-WindowsUser` created (from a Mac,
with Windows App, formerly Microsoft Remote Desktop, from the App Store), then:

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
