<#
.SYNOPSIS
  Shares the Windows vcpkg binary cache (Open CASCADE, Qt and the other ports)
  between machines through a GitHub release.

.DESCRIPTION
  publish (on the desktop, after a build): packs the vcpkg archives that the
  build trees under build\windows-msvc-*\vcpkg_installed use and uploads them,
  split under the 2 GiB asset limit, to the prerelease on the fixed tag
  "vcpkg-cache-x64-windows". Needs gh signed in with push access.

  fetch (on a new machine, before the first build): downloads the parts that
  match this machine's toolchain into vcpkg's binary cache, where the next
  configure finds them. Works without a token while the repository is public;
  uses GH_TOKEN, GITHUB_TOKEN or gh's token when there is one.

  vcpkg names each archive by its package ABI hash, which covers the exact
  cl.exe, the vcpkg commit (builtin-baseline in vcpkg.json), the triplet and
  the tools vcpkg downloads. A machine with another compiler build simply
  misses and builds from source. To make a miss visible before the hour-long
  build instead of during it, assets are named by a key:
    vcpkg-cache-x64-windows-msvc<toolset>-vcpkg<baseline[0:12]>.tar.partNN
  plus a .manifest.txt listing what is inside. fetch takes only the parts whose
  key matches this machine and lists the published keys when none does.
  scripts\windows-toolchain.json pins the toolset both machines install.
  See docs\decisions\0019-windows-dev-vm-and-shared-cache.md.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\vcpkg-cache.ps1 publish
  powershell -ExecutionPolicy Bypass -File scripts\vcpkg-cache.ps1 fetch
  powershell -ExecutionPolicy Bypass -File scripts\vcpkg-cache.ps1 key
#>
[CmdletBinding()]
param(
  [Parameter(Position = 0, Mandatory = $true)]
  [ValidateSet("publish", "fetch", "key")]
  [string] $Command,
  # vcpkg's files cache. Default: VCPKG_DEFAULT_BINARY_CACHE, else
  # %LOCALAPPDATA%\vcpkg\archives (vcpkg's own default).
  [string] $CacheDir,
  # fetch: take this key instead of this machine's (see "key").
  [string] $Key,
  # owner/name. Default: VS_CACHE_REPO, else the origin remote.
  [string] $Repo,
  # publish even though the local toolset differs from windows-toolchain.json.
  [switch] $Force
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest is 10x slower with it
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Tag = if ($env:VS_CACHE_TAG) { $env:VS_CACHE_TAG } else { "vcpkg-cache-x64-windows" }
$Prefix = "vcpkg-cache-x64-windows-"
$PartSize = 1900MB
$Tar = Join-Path $env:SystemRoot "System32\tar.exe"   # bsdtar, in Windows since 1803

function Get-Toolchain {
  Get-Content (Join-Path $PSScriptRoot "windows-toolchain.json") -Raw | ConvertFrom-Json
}

function Get-Baseline {
  (Get-Content (Join-Path $RepoRoot "vcpkg.json") -Raw | ConvertFrom-Json).'builtin-baseline'
}

# The toolset vcpkg compiles with: the newest MSVC in the newest Visual Studio
# that has the C++ tools (the same rule scripts\build.ps1 follows).
function Get-LocalMsvc {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) { return $null }
  $vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $vsPath) { return $null }
  $toolsets = Get-ChildItem (Join-Path $vsPath "VC\Tools\MSVC") -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending
  if ($toolsets) { return $toolsets[0].Name }
  return $null
}

function Get-CacheKey([string] $Msvc) {
  "msvc$Msvc-vcpkg$((Get-Baseline).Substring(0, 12))"
}

function Get-DefaultCacheDir {
  if ($CacheDir) { return $CacheDir }
  if ($env:VCPKG_DEFAULT_BINARY_CACHE) { return $env:VCPKG_DEFAULT_BINARY_CACHE }
  Join-Path $env:LOCALAPPDATA "vcpkg\archives"
}

function Get-RepoSlug {
  if ($Repo) { return $Repo }
  if ($env:VS_CACHE_REPO) { return $env:VS_CACHE_REPO }
  $url = Invoke-Native git @("-C", $RepoRoot, "remote", "get-url", "origin")
  if (-not $url) { throw "Pass -Repo owner/name (no origin remote)." }
  $url = $url.Trim() -replace '\.git$', ''
  $parts = ($url -split '[/:]') | Where-Object { $_ }
  "$($parts[-2])/$($parts[-1])"
}

function Find-Gh {
  $cmd = Get-Command gh -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  $default = Join-Path $env:ProgramFiles "GitHub CLI\gh.exe"
  if (Test-Path $default) { return $default }
  return $null
}

# Runs a native command for its stdout, ignoring stderr. Windows PowerShell 5.1
# turns native stderr into terminating errors under ErrorActionPreference Stop,
# even when it is redirected, so relax it for the call.
function Invoke-Native {
  param([string] $Exe, [string[]] $Arguments)
  $old = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    $out = & $Exe @Arguments 2>$null
    $script:NativeExitCode = $LASTEXITCODE
  } finally { $ErrorActionPreference = $old }
  $out
}

function Invoke-Checked {
  param([string] $Exe, [string[]] $Arguments)
  & $Exe @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "'$Exe $($Arguments -join ' ')' failed with exit code $LASTEXITCODE"
  }
}

function Split-File([string] $Path, [string] $OutPrefix) {
  $buffer = New-Object byte[] (8MB)
  $in = [IO.File]::OpenRead($Path)
  $parts = @()
  try {
    $index = 0
    while ($in.Position -lt $in.Length) {
      $name = "{0}{1:D2}" -f $OutPrefix, $index
      $out = [IO.File]::Create($name)
      try {
        $left = [Math]::Min([long]$PartSize, $in.Length - $in.Position)
        while ($left -gt 0) {
          $n = $in.Read($buffer, 0, [int][Math]::Min([long]$buffer.Length, $left))
          $out.Write($buffer, 0, $n)
          $left -= $n
        }
      } finally { $out.Dispose() }
      $parts += $name
      $index++
    }
  } finally { $in.Dispose() }
  $parts
}

function Join-Files([string[]] $Paths, [string] $OutPath) {
  $out = [IO.File]::Create($OutPath)
  try {
    foreach ($p in $Paths) {
      $in = [IO.File]::OpenRead($p)
      try { $in.CopyTo($out, 8MB) } finally { $in.Dispose() }
    }
  } finally { $out.Dispose() }
}

function New-WorkDir {
  $dir = Join-Path ([IO.Path]::GetTempPath()) ("vs-vcpkg-cache-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
  New-Item -ItemType Directory -Force $dir | Out-Null
  $dir
}

# --- publish -----------------------------------------------------------------

function Invoke-Publish {
  $gh = Find-Gh
  if (-not $gh) { throw "publish needs the GitHub CLI (gh), signed in with push access." }
  $toolchain = Get-Toolchain
  $msvc = Get-LocalMsvc
  if (-not $msvc) { throw "No Visual Studio with the C++ tools found." }
  if ($msvc -ne $toolchain.msvc) {
    $msg = "This machine compiles with MSVC $msvc, but scripts\windows-toolchain.json pins $($toolchain.msvc)."
    if (-not $Force) { throw "$msg Machines installing the pinned toolset would miss. Update the pin (and publish) or pass -Force." }
    Write-Warning $msg
  }
  $key = Get-CacheKey $msvc
  $cache = Get-DefaultCacheDir
  if (-not (Test-Path $cache)) { throw "No vcpkg binary cache at $cache" }
  $repoSlug = Get-RepoSlug

  # Every package installed in any Windows build tree, by ABI hash
  # (vcpkg names an archive by the sha256 of its vcpkg_abi_info.txt).
  $infos = Get-ChildItem (Join-Path $RepoRoot "build") -Directory -Filter "windows-msvc-*" -ErrorAction SilentlyContinue |
    ForEach-Object { Get-ChildItem (Join-Path $_.FullName "vcpkg_installed\*\share\*\vcpkg_abi_info.txt") -ErrorAction SilentlyContinue }
  if (-not $infos) { throw "No build\windows-msvc-*\vcpkg_installed tree. Build first (scripts\build.ps1 -Preset windows-msvc-app)." }

  $baseline = Get-Baseline
  $entries = @{}
  $tripletAbis = @{}
  $missing = 0
  foreach ($info in $infos) {
    $abi = (Get-FileHash -Algorithm SHA256 $info.FullName).Hash.ToLowerInvariant()
    $triplet = $info.Directory.Parent.Parent.Name
    $package = $info.Directory.Name
    foreach ($line in (Get-Content $info.FullName)) {
      if ($line -like "triplet_abi *") { $tripletAbis["$triplet $($line.Substring(12))"] = $true }
    }
    $rel = "$($abi.Substring(0, 2))/$abi.zip"
    if (Test-Path (Join-Path $cache $rel)) {
      $entries[$rel] = "$package`:$triplet"
    } else {
      Write-Warning "No archive for $package`:$triplet ($abi) in $cache"
      $missing++
    }
  }
  if ($entries.Count -eq 0) { throw "None of the installed packages has an archive in $cache" }

  $work = New-WorkDir
  try {
    $list = Join-Path $work "files.txt"
    $sorted = $entries.Keys | Sort-Object
    [IO.File]::WriteAllLines($list, [string[]]$sorted)

    $manifest = Join-Path $work "$Prefix$key.manifest.txt"
    $commit = "$(Invoke-Native git @("-C", $RepoRoot, "rev-parse", "HEAD"))".Trim()
    $lines = @(
      "key $key",
      "msvc $msvc",
      "vcpkg-baseline $baseline",
      "published $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')) from $env:COMPUTERNAME at $commit"
    )
    $lines += $tripletAbis.Keys | Sort-Object | ForEach-Object { "triplet_abi $_" }
    $lines += $sorted | ForEach-Object { "$_ $($entries[$_])" }
    [IO.File]::WriteAllLines($manifest, [string[]]$lines)

    $tarPath = Join-Path $work "cache.tar"
    Write-Host "Packing $($entries.Count) archive(s) from $cache (key $key)"
    Invoke-Checked $Tar @("-C", $cache, "-cf", $tarPath, "-T", $list)
    $parts = Split-File $tarPath (Join-Path $work "$Prefix$key.tar.part")
    Remove-Item $tarPath

    Invoke-Native $gh @("release", "view", $Tag, "-R", $repoSlug) | Out-Null
    if ($script:NativeExitCode -ne 0) {
      Invoke-Checked $gh @("release", "create", $Tag, "-R", $repoSlug, "--prerelease", "--target", "main",
        "--title", "vcpkg binary cache (x64-windows)",
        "--notes", "Prebuilt vcpkg ports (Open CASCADE, Qt, ...) for Windows, built on the desktop with the toolset in scripts/windows-toolchain.json. Fetch with scripts/vcpkg-cache.ps1 fetch. Assets are named by toolchain key. Not a product release.")
    }
    Write-Host "Uploading $($parts.Count) part(s) to release $Tag of $repoSlug"
    Invoke-Checked $gh (@("release", "upload", $Tag, "-R", $repoSlug, "--clobber", $manifest) + $parts)

    # Parts of this key left over from an earlier, larger upload.
    $uploaded = @($parts | ForEach-Object { Split-Path -Leaf $_ })
    $names = & $gh release view $Tag -R $repoSlug --json assets --jq ".assets[].name"
    foreach ($name in $names) {
      if ($name -like "$Prefix$key.tar.part*" -and $uploaded -notcontains $name) {
        Invoke-Checked $gh @("release", "delete-asset", $Tag, $name, "-R", $repoSlug, "-y")
      }
    }
    $others = @($names | Where-Object { $_ -like "$Prefix*.manifest.txt" -and $_ -ne "$Prefix$key.manifest.txt" })
    if ($others) {
      Write-Host "Other keys still published (delete with gh release delete-asset once no machine needs them):"
      $others | ForEach-Object { Write-Host "  $_" }
    }
    Write-Host "Published key $key ($($entries.Count) archives, $missing missing)." -ForegroundColor Green
  } finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
  }
}

# --- fetch ---------------------------------------------------------------------

function Get-GitHubToken {
  if ($env:GH_TOKEN) { return $env:GH_TOKEN }
  if ($env:GITHUB_TOKEN) { return $env:GITHUB_TOKEN }
  $gh = Find-Gh
  if ($gh) {
    $token = Invoke-Native $gh @("auth", "token")
    if ($script:NativeExitCode -eq 0 -and $token) { return $token.Trim() }
  }
  return $null
}

function Invoke-Fetch {
  $wanted = $Key
  if (-not $wanted) {
    $msvc = Get-LocalMsvc
    if (-not $msvc) {
      $msvc = (Get-Toolchain).msvc
      Write-Warning "No Visual Studio with the C++ tools found; assuming the pinned MSVC $msvc."
    }
    $wanted = Get-CacheKey $msvc
  }
  $cache = Get-DefaultCacheDir
  $repoSlug = Get-RepoSlug
  $headers = @{ "Accept" = "application/vnd.github+json"; "User-Agent" = "vstudio-vcpkg-cache" }
  $token = Get-GitHubToken
  if ($token) { $headers["Authorization"] = "Bearer $token" }

  $api = "https://api.github.com/repos/$repoSlug"
  try {
    $release = Invoke-RestMethod -Headers $headers "$api/releases/tags/$Tag"
  } catch {
    throw "Could not read release $Tag of $repoSlug (nothing published yet, or no access): $($_.Exception.Message)"
  }
  $parts = @($release.assets | Where-Object { $_.name -like "$Prefix$wanted.tar.part*" } | Sort-Object name)
  if (-not $parts) {
    $keys = @($release.assets | Where-Object { $_.name -like "$Prefix*.manifest.txt" } |
      ForEach-Object { $_.name.Substring($Prefix.Length) -replace '\.manifest\.txt$', '' })
    throw ("No cache for key $wanted. Published: $($keys -join ', '). " +
      "Install the toolset in scripts\windows-toolchain.json, or publish this key from the desktop.")
  }

  $work = New-WorkDir
  try {
    $files = @()
    $dlHeaders = $headers.Clone()
    $dlHeaders["Accept"] = "application/octet-stream"
    foreach ($part in $parts) {
      $out = Join-Path $work $part.name
      Write-Host ("  {0} ({1:N0} MB)" -f $part.name, ($part.size / 1MB))
      Invoke-WebRequest -UseBasicParsing -Headers $dlHeaders -OutFile $out "$api/releases/assets/$($part.id)"
      $files += $out
    }
    $tarPath = Join-Path $work "cache.tar"
    Join-Files $files $tarPath
    $files | ForEach-Object { Remove-Item $_ }
    New-Item -ItemType Directory -Force $cache | Out-Null
    Invoke-Checked $Tar @("-C", $cache, "-xf", $tarPath)
    $count = @(Get-ChildItem $cache -Recurse -Filter *.zip).Count
    Write-Host "Fetched key $wanted into $cache ($count archive(s) there now)." -ForegroundColor Green
  } finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
  }
}

switch ($Command) {
  "publish" { Invoke-Publish }
  "fetch" { Invoke-Fetch }
  "key" {
    $msvc = Get-LocalMsvc
    if (-not $msvc) { throw "No Visual Studio with the C++ tools found." }
    Get-CacheKey $msvc
  }
}
