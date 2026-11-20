<#
.SYNOPSIS
  Configures, builds and tests Volumetric Studio on Windows in one command.

.DESCRIPTION
  - Finds Visual Studio 2022 (or newer) with the C++ workload and loads its
    x64 developer environment, so cl, cmake and ninja are on PATH.
  - Uses $env:VCPKG_ROOT when it is a git clone of vcpkg. Otherwise clones vcpkg into
    %LOCALAPPDATA%\vstudio\vcpkg at the commit pinned in vcpkg.json
    ("builtin-baseline") and bootstraps it.
  - Runs the CMake configure, build and test presets.

.EXAMPLE
  ./scripts/build.ps1
  ./scripts/build.ps1 -Preset windows-msvc-debug
  ./scripts/build.ps1 -Clean
#>
[CmdletBinding()]
param(
  [string] $Preset = "windows-msvc-release",
  [switch] $Clean,
  [switch] $SkipTests
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

function Invoke-Checked {
  param([string] $Exe, [string[]] $Arguments)
  & $Exe @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "'$Exe $($Arguments -join ' ')' failed with exit code $LASTEXITCODE"
  }
}

# --- Visual Studio developer environment -----------------------------------

function Enter-VsDevEnvironment {
  if ((Get-Command cl.exe -ErrorAction SilentlyContinue) -and
      (Get-Command ninja.exe -ErrorAction SilentlyContinue) -and
      (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    return
  }
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio 2022 with the 'Desktop development with C++' workload."
  }
  $vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
  if (-not $vsPath) {
    throw "No Visual Studio installation with the C++ x64 tools was found."
  }
  # vcpkg builds dependencies with the newest MSVC toolset installed, while the
  # developer shell defaults to whichever toolset Visual Studio marks as
  # default. When several are installed these can differ, and objects built by
  # a newer STL don't link with an older one. Pin the shell to the newest.
  $toolsets = Get-ChildItem (Join-Path $vsPath "VC\Tools\MSVC") -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending
  $devArgs = "-arch=x64 -host_arch=x64"
  if ($toolsets) {
    $devArgs += " -vcvars_ver=$($toolsets[0].Name)"
  }
  $devShell = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
  Import-Module $devShell
  Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
    -DevCmdArguments $devArgs | Out-Null
  Write-Host "Using Visual Studio at $vsPath (MSVC $(if ($toolsets) { $toolsets[0].Name } else { 'default' }))"

  # vcpkg's archives are keyed by the exact compiler, so a toolset other than
  # the pinned one rebuilds Open CASCADE and Qt from source (hours) instead of
  # restoring them from the shared cache (decision 0019).
  $pin = (Get-Content (Join-Path $PSScriptRoot "windows-toolchain.json") -Raw | ConvertFrom-Json).msvc
  if ($toolsets -and $toolsets[0].Name -ne $pin) {
    Write-Warning ("MSVC $($toolsets[0].Name) differs from $pin in scripts\windows-toolchain.json: " +
      "vcpkg's shared binary cache (scripts\vcpkg-cache.ps1) won't hit, so dependencies build from source.")
  }
}

# --- vcpkg --------------------------------------------------------------------

function Initialize-Vcpkg {
  $manifest = Get-Content (Join-Path $RepoRoot "vcpkg.json") -Raw | ConvertFrom-Json
  $baseline = $manifest.'builtin-baseline'

  # Only a git clone of vcpkg will do: the copy bundled with Visual Studio
  # (which its developer shell puts in VCPKG_ROOT) can't check out a baseline.
  $userRoot = $script:OriginalVcpkgRoot
  $managed = -not ($userRoot -and (Test-Path (Join-Path $userRoot ".git")))
  if (-not $managed) {
    $root = $userRoot
  } else {
    $root = Join-Path $env:LOCALAPPDATA "vstudio\vcpkg"
    if (-not (Test-Path (Join-Path $root ".git"))) {
      Write-Host "Cloning vcpkg into $root"
      New-Item -ItemType Directory -Force (Split-Path $root) | Out-Null
      Invoke-Checked git @("clone", "https://github.com/microsoft/vcpkg", $root)
    }
  }

  # The clone must contain the baseline commit; a vcpkg we manage is also
  # checked out at it, so the tool and the port scripts match.
  & git -C $root cat-file -e "$baseline^{commit}" 2>$null
  if ($LASTEXITCODE -ne 0) {
    Write-Host "Fetching vcpkg baseline $baseline"
    Invoke-Checked git @("-C", $root, "fetch", "origin")
  }
  if ($managed) {
    $head = (& git -C $root rev-parse HEAD).Trim()
    if ($head -ne $baseline) {
      Invoke-Checked git @("-C", $root, "checkout", "--quiet", $baseline)
      Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $root "vcpkg.exe")
    }
  }
  if (-not (Test-Path (Join-Path $root "vcpkg.exe"))) {
    Invoke-Checked (Join-Path $root "bootstrap-vcpkg.bat") @("-disableMetrics")
  }
  $env:VCPKG_ROOT = $root
  Write-Host "Using vcpkg at $root"
}

# --- build ----------------------------------------------------------------------

$script:OriginalVcpkgRoot = $env:VCPKG_ROOT

Push-Location $RepoRoot
try {
  Enter-VsDevEnvironment
  Initialize-Vcpkg

  if ($Clean) {
    $buildDir = Join-Path $RepoRoot "build\$Preset"
    if (Test-Path $buildDir) {
      Write-Host "Removing $buildDir"
      Remove-Item -Recurse -Force $buildDir
    }
  }

  Invoke-Checked cmake @("--preset", $Preset)
  Invoke-Checked cmake @("--build", "--preset", $Preset)
  if (-not $SkipTests) {
    Invoke-Checked ctest @("--preset", $Preset)
  }
  Write-Host "`nBuild and tests succeeded ($Preset)." -ForegroundColor Green
} finally {
  Pop-Location
}
