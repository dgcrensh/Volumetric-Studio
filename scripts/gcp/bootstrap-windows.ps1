<#
.SYNOPSIS
  Turns a fresh Windows Server 2022 VM into a Volumetric Studio build machine.

.DESCRIPTION
  New-DevVm.ps1 attaches this as the VM's windows-startup-script-ps1, so it
  runs as SYSTEM on every boot; each step checks whether it is already done,
  and after the last one a marker makes later boots exit at once. It can also
  be run by hand in an elevated Windows PowerShell on any Windows machine.

    1. NVIDIA GRID (RTX Virtual Workstation) driver, newest for Server 2022
       from Google's public nvidia-drivers-us-public bucket, when an NVIDIA
       GPU is present and nvidia-smi isn't.
    2. Git for Windows and the GitHub CLI (latest releases).
    3. Clone of the repository into C:\src\Volumetric-Studio (no spaces,
       which Qt's host build needs).
    4. Visual Studio 2022 Build Tools with the components and the MSVC
       toolset pinned in scripts\windows-toolchain.json, so vcpkg's archives
       built on the desktop hit here.
    5. vcpkg in C:\vcpkg at the repository's builtin-baseline, its binary
       cache in C:\vcpkg-cache (machine-wide VCPKG_ROOT and
       VCPKG_DEFAULT_BINARY_CACHE, writable by every user).
    6. scripts\vcpkg-cache.ps1 fetch: the desktop's Open CASCADE, Qt, ...
    7. Unless vstudio-build is 0, scripts\build.ps1 -Preset windows-msvc-app
       -SkipTests, which proves the cache hit (no OCCT or Qt compile).
    8. C:\Users\Public\Desktop\vstudio-next-steps.txt: what happened and the
       manual steps (gh auth login, the Claude app).
  It reboots once at the end when the driver or Build Tools asked for it.

  Log: C:\vstudio-bootstrap\bootstrap.log (also on the serial console).
  Settings come from instance metadata on GCE (vstudio-repo, vstudio-branch,
  vstudio-build, vstudio-grid-driver-url) or from the parameters.
#>
[CmdletBinding()]
param(
  [string] $RepoUrl,
  [string] $Branch,
  [string] $SourceDir = "C:\src\Volumetric-Studio",
  [string] $VcpkgRoot = "C:\vcpkg",
  [string] $CacheDir = "C:\vcpkg-cache",
  # Overrides the newest GRID driver in Google's bucket.
  [string] $GridDriverUrl,
  [switch] $NoBuild
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$StateDir = "C:\vstudio-bootstrap"
$DoneMarker = Join-Path $StateDir "done"
New-Item -ItemType Directory -Force $StateDir | Out-Null
if (Test-Path $DoneMarker) {
  Write-Host "Volumetric Studio bootstrap already done ($(Get-Content $DoneMarker)). Delete $DoneMarker to run it again."
  return
}
Start-Transcript -Append -Path (Join-Path $StateDir "bootstrap.log") | Out-Null
$script:RebootNeeded = $false
$script:Notes = @()

function Step([string] $Text) { Write-Host "`n=== $(Get-Date -Format 'HH:mm:ss') $Text" }
function Note([string] $Text) { Write-Host $Text; $script:Notes += $Text }

function Get-Metadata([string] $Key) {
  try {
    $value = Invoke-RestMethod -UseBasicParsing -TimeoutSec 3 -Headers @{ "Metadata-Flavor" = "Google" } `
      "http://metadata.google.internal/computeMetadata/v1/instance/attributes/$Key"
    return "$value".Trim()
  } catch { return $null }
}

function Invoke-Checked {
  param([string] $Exe, [string[]] $Arguments)
  $old = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try { & $Exe @Arguments 2>&1 | ForEach-Object { "$_" } | Out-Host; $code = $LASTEXITCODE }
  finally { $ErrorActionPreference = $old }
  if ($code -ne 0) { throw "'$Exe $($Arguments -join ' ')' failed with exit code $code" }
}

function Invoke-Installer {
  param([string] $Path, [string[]] $Arguments, [int[]] $Ok = @(0))
  $p = Start-Process -FilePath $Path -ArgumentList $Arguments -Wait -PassThru
  if ($p.ExitCode -eq 3010 -or $p.ExitCode -eq 1641) { $script:RebootNeeded = $true; return }
  if ($Ok -notcontains $p.ExitCode) { throw "$Path exited with $($p.ExitCode)" }
}

function Save-Download([string] $Url, [string] $Name) {
  $dir = Join-Path $StateDir "downloads"
  New-Item -ItemType Directory -Force $dir | Out-Null
  $out = Join-Path $dir $Name
  if (-not (Test-Path $out)) {
    Write-Host "Downloading $Url"
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile "$out.partial"
    Move-Item "$out.partial" $out
  }
  $out
}

function Get-LatestReleaseAsset([string] $Repo, [string] $Pattern) {
  $release = Invoke-RestMethod -UseBasicParsing -Headers @{ "User-Agent" = "vstudio-bootstrap" } `
    "https://api.github.com/repos/$Repo/releases/latest"
  $asset = $release.assets | Where-Object { $_.name -match $Pattern } | Select-Object -First 1
  if (-not $asset) { throw "No asset matching $Pattern in the latest $Repo release" }
  $asset
}

# The newest vGPU release's Windows Server 2022 driver in Google's bucket of
# GRID drivers (the files Google's driver table links to). The bucket's
# objects download anonymously but listing it needs a signed-in caller, so
# this lists with the VM's service account token from the metadata server
# (any VM with the default storage read scope).
function Find-GridDriver {
  $bucket = "nvidia-drivers-us-public"
  $token = (Invoke-RestMethod -UseBasicParsing -TimeoutSec 5 -Headers @{ "Metadata-Flavor" = "Google" } `
    "http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/token").access_token
  $names = @()
  $pageToken = $null
  do {
    $url = "https://storage.googleapis.com/storage/v1/b/$bucket/o?prefix=GRID/&fields=items(name),nextPageToken"
    if ($pageToken) { $url += "&pageToken=$pageToken" }
    $page = Invoke-RestMethod -UseBasicParsing -Headers @{ "Authorization" = "Bearer $token" } $url
    $names += @($page.items | ForEach-Object { $_.name })
    $pageToken = $page.nextPageToken
  } while ($pageToken)
  $drivers = $names | Where-Object { $_ -match '^GRID/vGPU[\d.]+/[^/]*server2022[^/]*\.exe$' } |
    Sort-Object { [version](($_ -replace '^GRID/vGPU([\d.]+)/.*$', '$1') + ".0") }, { $_ }
  if (-not $drivers) { throw "no Server 2022 driver listed in gs://$bucket/GRID/" }
  "https://storage.googleapis.com/$bucket/$($drivers[-1])"
}

function Update-Path {
  $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
    [Environment]::GetEnvironmentVariable("Path", "User")
}

function Grant-UsersModify([string] $Path) {
  & icacls $Path /grant "*S-1-5-32-545:(OI)(CI)M" /T /Q | Out-Null   # BUILTIN\Users
}

try {
  if (-not $RepoUrl) { $RepoUrl = Get-Metadata "vstudio-repo" }
  if (-not $RepoUrl) { $RepoUrl = "https://github.com/dgcrensh/Volumetric-Studio.git" }
  if (-not $Branch) { $Branch = Get-Metadata "vstudio-branch" }
  if (-not $Branch) { $Branch = "main" }
  if (-not $GridDriverUrl) { $GridDriverUrl = Get-Metadata "vstudio-grid-driver-url" }
  if (-not $NoBuild -and (Get-Metadata "vstudio-build") -eq "0") { $NoBuild = $true }

  # --- 1. GRID driver --------------------------------------------------------
  Step "NVIDIA GRID driver"
  $smi = Join-Path $env:SystemRoot "System32\nvidia-smi.exe"
  $nvidia = Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match "NVIDIA" -or $_.DeviceID -match "VEN_10DE" }
  if (Test-Path $smi) {
    Note "GPU driver: already installed. $((& $smi --query-gpu=name,driver_version --format=csv,noheader) -join '; ')"
  } elseif (-not $nvidia) {
    Note "GPU driver: skipped, no NVIDIA device on this machine."
  } else {
    try {
      if (-not $GridDriverUrl) { $GridDriverUrl = Find-GridDriver }
      $exe = Save-Download $GridDriverUrl (Split-Path -Leaf $GridDriverUrl)
      Write-Host "Installing $exe (silent)"
      Invoke-Installer $exe @("-s", "-noreboot") @(0, 1)
      $script:RebootNeeded = $true
      Note "GPU driver: installed $(Split-Path -Leaf $GridDriverUrl). The GRID licence comes with the -vws GPU type."
    } catch {
      # Everything else still works without the driver; the GPU tests skip.
      Note ("GPU driver: NOT installed ($($_.Exception.Message)). Find the Server 2022 driver in " +
        "https://cloud.google.com/compute/docs/gpus/grid-drivers-table and run it, or set the instance " +
        "metadata vstudio-grid-driver-url to its URL, delete $DoneMarker and reboot.")
    }
  }

  # --- 2. git and gh -----------------------------------------------------------
  Step "Git and GitHub CLI"
  Update-Path
  if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    $asset = Get-LatestReleaseAsset "git-for-windows/git" '^Git-[\d.]+(\.windows\.\d+)?-64-bit\.exe$'
    $exe = Save-Download $asset.browser_download_url $asset.name
    Invoke-Installer $exe @("/VERYSILENT", "/NORESTART", "/NOCANCEL", "/SP-", "/SUPPRESSMSGBOXES")
    Update-Path
  }
  if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    $asset = Get-LatestReleaseAsset "cli/cli" '_windows_amd64\.msi$'
    $msi = Save-Download $asset.browser_download_url $asset.name
    Invoke-Installer "msiexec.exe" @("/i", "`"$msi`"", "/qn", "/norestart")
    Update-Path
  }
  # Everything below is cloned by SYSTEM; let users work in those clones.
  Invoke-Checked git @("config", "--system", "--replace-all", "safe.directory", "*")
  Note "git $((& git --version) -replace 'git version ', ''), gh installed."

  # --- 3. clone -----------------------------------------------------------------
  Step "Clone $RepoUrl ($Branch)"
  if (-not (Test-Path (Join-Path $SourceDir ".git"))) {
    New-Item -ItemType Directory -Force (Split-Path $SourceDir) | Out-Null
    Invoke-Checked git @("clone", "--branch", $Branch, $RepoUrl, $SourceDir)
  }
  Grant-UsersModify (Split-Path $SourceDir)
  $toolchain = Get-Content (Join-Path $SourceDir "scripts\windows-toolchain.json") -Raw | ConvertFrom-Json
  $baseline = (Get-Content (Join-Path $SourceDir "vcpkg.json") -Raw | ConvertFrom-Json).'builtin-baseline'

  # --- 4. Visual Studio Build Tools -------------------------------------------------
  Step "Visual Studio Build Tools $($toolchain.visualStudio), MSVC $($toolchain.msvc)"
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  $vsPath = $null
  if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  }
  if (-not $vsPath -or -not (Test-Path (Join-Path $vsPath "VC\Tools\MSVC\$($toolchain.msvc)"))) {
    $exe = Save-Download $toolchain.buildToolsUrl "vs_buildtools.exe"
    $vsArgs = @("--quiet", "--wait", "--norestart", "--nocache")
    foreach ($c in $toolchain.components) { $vsArgs += @("--add", $c) }
    Write-Host "Running the Build Tools installer (20-30 minutes)"
    Invoke-Installer $exe $vsArgs
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  }
  $toolsets = Get-ChildItem (Join-Path $vsPath "VC\Tools\MSVC") -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending
  $msvc = $toolsets[0].Name
  if ($msvc -eq $toolchain.msvc) {
    Note "MSVC: $msvc, as pinned in scripts\windows-toolchain.json."
  } else {
    Note ("MSVC: WARNING, $msvc installed but $($toolchain.msvc) is pinned. vcpkg's shared cache will miss and " +
      "Open CASCADE and Qt will build from source. See docs\dev-vm.md, 'Toolset drift'.")
  }

  # --- 5. vcpkg ----------------------------------------------------------------------
  Step "vcpkg at $baseline"
  if (-not (Test-Path (Join-Path $VcpkgRoot ".git"))) {
    Invoke-Checked git @("clone", "https://github.com/microsoft/vcpkg", $VcpkgRoot)
  }
  if ((& git -C $VcpkgRoot rev-parse HEAD).Trim() -ne $baseline) {
    Invoke-Checked git @("-C", $VcpkgRoot, "fetch", "origin")
    Invoke-Checked git @("-C", $VcpkgRoot, "checkout", "--quiet", $baseline)
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $VcpkgRoot "vcpkg.exe")
  }
  if (-not (Test-Path (Join-Path $VcpkgRoot "vcpkg.exe"))) {
    Invoke-Checked (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") @("-disableMetrics")
  }
  New-Item -ItemType Directory -Force $CacheDir | Out-Null
  Grant-UsersModify $VcpkgRoot
  Grant-UsersModify $CacheDir
  [Environment]::SetEnvironmentVariable("VCPKG_ROOT", $VcpkgRoot, "Machine")
  [Environment]::SetEnvironmentVariable("VCPKG_DEFAULT_BINARY_CACHE", $CacheDir, "Machine")
  $env:VCPKG_ROOT = $VcpkgRoot
  $env:VCPKG_DEFAULT_BINARY_CACHE = $CacheDir

  # --- 6. the desktop's binary cache ----------------------------------------------------
  Step "Fetch the vcpkg binary cache"
  try {
    Invoke-Checked "powershell.exe" @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
      (Join-Path $SourceDir "scripts\vcpkg-cache.ps1"), "fetch", "-CacheDir", $CacheDir)
    Note "vcpkg cache: fetched key msvc$msvc-vcpkg$($baseline.Substring(0, 12)) into $CacheDir."
  } catch {
    Note "vcpkg cache: NOT fetched ($($_.Exception.Message)). The first build compiles every dependency."
  }

  # --- 7. warm build ---------------------------------------------------------------------
  if ($NoBuild) {
    Note "Build: skipped (vstudio-build=0)."
  } elseif (Test-Path (Join-Path $SourceDir "build\windows-msvc-app\apps\vs_app\vs_app.exe")) {
    Note "Build: already built."
  } else {
    Step "Build windows-msvc-app"
    $log = Join-Path $StateDir "build.log"
    $started = Get-Date
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $SourceDir "scripts\build.ps1") `
      -Preset windows-msvc-app -SkipTests *> $log
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    Grant-UsersModify (Join-Path $SourceDir "build")
    $minutes = [int]((Get-Date) - $started).TotalMinutes
    $restored = Select-String -Path $log -Pattern "Restored \d+ package" | Select-Object -First 1
    $built = @(Select-String -Path $log -Pattern "^Building [^ ]+:x64-windows").Count
    if ($code -eq 0) {
      Note "Build: windows-msvc-app built in $minutes min. $(if ($restored) { $restored.Line.Trim() }) $built port(s) compiled from source."
    } else {
      Note "Build: FAILED after $minutes min (exit $code). See $log."
    }
  }

  # --- 8. next steps -----------------------------------------------------------------------
  Step "Done"
  $nextSteps = @"
Volumetric Studio dev VM, bootstrapped $(Get-Date -Format 'yyyy-MM-dd HH:mm')

$($script:Notes -join "`r`n")

Repository: $SourceDir   (vcpkg: $VcpkgRoot, cache: $CacheDir)
Log: $StateDir\bootstrap.log

Still to do by hand (each needs you to sign in):
  1. gh auth login            (then: gh auth setup-git)
  2. Install and sign in to the Claude desktop app: https://claude.ai/download
  3. Run the tests once:      cd $SourceDir; powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Preset windows-msvc-app

Stop the VM when idle; it bills by the hour while running.
"@
  [IO.File]::WriteAllText("C:\Users\Public\Desktop\vstudio-next-steps.txt", $nextSteps)
  Write-Host $nextSteps
  Remove-Item -Force -ErrorAction SilentlyContinue "C:\Users\Public\Desktop\vstudio-bootstrap-FAILED.txt"
  Set-Content -Path $DoneMarker -Value (Get-Date -Format o)
} catch {
  Write-Host "BOOTSTRAP FAILED: $($_.Exception.Message)`n$($_.ScriptStackTrace)"
  [IO.File]::WriteAllText("C:\Users\Public\Desktop\vstudio-bootstrap-FAILED.txt",
    "The bootstrap failed: $($_.Exception.Message)`r`nSee $StateDir\bootstrap.log. It runs again on the next boot, or run it by hand:`r`n" +
    "  powershell -ExecutionPolicy Bypass -File $SourceDir\scripts\gcp\bootstrap-windows.ps1")
} finally {
  Stop-Transcript | Out-Null
}

if ($script:RebootNeeded) {
  Write-Host "Rebooting to finish the driver and Build Tools installs."
  Restart-Computer -Force
}
