<#
.SYNOPSIS
  Creates a Windows Server 2022 dev VM with an NVIDIA RTX Virtual Workstation
  GPU on Google Compute Engine, trying GPUs and zones until one has capacity.

.DESCRIPTION
  Run from any machine with the gcloud CLI signed in (gcloud auth login) and a
  project selected (gcloud config set project <id>).

  1. Checks the project's GPU quota (GPUS_ALL_REGIONS and the per-region
     virtual workstation GPU quota) and skips regions with none free.
  2. Restricts RDP to this machine's public IP with the firewall rule
     "vstudio-allow-rdp" on the target tag "vstudio-dev".
  3. Tries each GPU in -Gpus (L4 on a G2, then T4 on an N1 by default) in each
     zone that offers it, preferred regions first, until a create succeeds.
     ZONE_RESOURCE_POOL_EXHAUSTED (L4s are often scarce) moves on to the next.
  4. Attaches scripts\gcp\bootstrap-windows.ps1 as the Windows startup script.
     On first boot it installs the GRID driver, VS 2022 Build Tools with the
     pinned toolset, git, gh and vcpkg, clones the repository, fetches the
     desktop's vcpkg binary cache and builds the app (see that script).

  What this can't do for you (docs\dev-vm.md has the steps):
  - Raise GPU quota or upgrade a free-trial billing account. Free trials get
    no GPUs at all; GPUS_ALL_REGIONS must be at least 1.
  - The interactive sign-ins on the VM: gh auth login and the Claude app.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\gcp\New-DevVm.ps1
  powershell -ExecutionPolicy Bypass -File scripts\gcp\New-DevVm.ps1 -Gpus T4 -Regions us-east1
  powershell -ExecutionPolicy Bypass -File scripts\gcp\New-DevVm.ps1 -WindowsUser dallas
#>
[CmdletBinding()]
param(
  [string] $Name = "vstudio-dev",
  # gcloud's configured project when empty.
  [string] $Project,
  # In order of preference. L4: g2-standard-*, T4: n1-standard-*.
  [ValidateSet("L4", "T4")]
  [string[]] $Gpus = @("L4", "T4"),
  # Regions tried first, in this order; other regions offering the GPU follow.
  [string[]] $Regions = @("us-central1", "us-east4", "us-east1", "us-west1", "us-west4", "us-east5", "us-south1"),
  # Only the preferred regions, never the rest.
  [switch] $OnlyPreferredRegions,
  [string] $L4MachineType = "g2-standard-8",
  [string] $T4MachineType = "n1-standard-8",
  [int] $DiskGb = 200,
  [string] $RepoUrl = "https://github.com/dgcrensh/Volumetric-Studio.git",
  [string] $Branch = "main",
  # The bootstrap builds the windows-msvc-app preset after fetching the cache.
  [switch] $NoBuild,
  # Source range for RDP. Default: this machine's public IP.
  [string] $AllowRdpFrom,
  # When given, creates this Windows user once the VM is up and prints its
  # password (gcloud compute reset-windows-password).
  [string] $WindowsUser
)

$ErrorActionPreference = "Stop"
$Tag = "vstudio-dev"
$Bootstrap = Join-Path $PSScriptRoot "bootstrap-windows.ps1"

$GpuTypes = @{
  "L4" = @{ Accelerator = "nvidia-l4-vws"; Quota = "NVIDIA_L4_VWS_GPUS" }
  "T4" = @{ Accelerator = "nvidia-tesla-t4-vws"; Quota = "NVIDIA_T4_VWS_GPUS" }
}

# gcloud writes progress and errors to stderr. Windows PowerShell 5.1 turns
# redirected native stderr into errors, so run it with errors non-terminating.
function Invoke-Gcloud {
  param([string[]] $Arguments, [switch] $Quiet)
  $old = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    $all = @("--quiet")
    if ($Project) { $all += "--project=$Project" }
    $out = & gcloud @all @Arguments 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
  } finally { $ErrorActionPreference = $old }
  [pscustomobject]@{ Code = $code; Out = ($out -join "`n") }
}

function Get-Json([string[]] $Arguments) {
  $r = Invoke-Gcloud ($Arguments + @("--format=json"))
  if ($r.Code -ne 0) { throw "gcloud $($Arguments -join ' ') failed:`n$($r.Out)" }
  # Drop any stderr lines gcloud mixed in before the JSON.
  $start = $r.Out.IndexOfAny([char[]]"[{")
  if ($start -lt 0) { return $null }
  $r.Out.Substring($start) | ConvertFrom-Json
}

function Get-FreeQuota($Quotas, [string] $Metric) {
  $q = $Quotas | Where-Object { $_.metric -eq $Metric } | Select-Object -First 1
  if (-not $q) { return 0 }
  [int]($q.limit - $q.usage)
}

if (-not (Get-Command gcloud -ErrorAction SilentlyContinue)) {
  throw "gcloud not found. Install the Google Cloud CLI (https://cloud.google.com/sdk/docs/install), then run gcloud auth login."
}
if (-not $Project) {
  $Project = (Invoke-Gcloud @("config", "get-value", "project")).Out.Trim().Split("`n")[-1].Trim()
  if (-not $Project -or $Project -eq "(unset)") { throw "No project: pass -Project or run gcloud config set project <id>." }
}
Write-Host "Project $Project"

$r = Invoke-Gcloud @("services", "enable", "compute.googleapis.com")
if ($r.Code -ne 0) { throw "Could not enable the Compute Engine API:`n$($r.Out)" }

$existing = Invoke-Gcloud @("compute", "instances", "list", "--filter=name=$Name", "--format=value(zone)")
if ($existing.Code -eq 0 -and $existing.Out.Trim()) {
  throw "A VM named $Name already exists in $($existing.Out.Trim()). Delete it or pass -Name."
}

# --- quota ----------------------------------------------------------------------

$projectInfo = Get-Json @("compute", "project-info", "describe")
$allRegions = Get-FreeQuota $projectInfo.quotas "GPUS_ALL_REGIONS"
if ($allRegions -lt 1) {
  throw ("GPUS_ALL_REGIONS quota is $allRegions. Request 1 or more at " +
    "https://console.cloud.google.com/iam-admin/quotas?project=$Project (filter: GPUs (all regions)). " +
    "Free-trial accounts must upgrade billing first. See docs\dev-vm.md.")
}

# --- firewall -------------------------------------------------------------------

if (-not $AllowRdpFrom) {
  $ip = (Invoke-RestMethod -UseBasicParsing "https://api.ipify.org").ToString().Trim()
  $AllowRdpFrom = "$ip/32"
}
$rule = "vstudio-allow-rdp"
$r = Invoke-Gcloud @("compute", "firewall-rules", "describe", $rule, "--format=value(name)")
if ($r.Code -eq 0) {
  $r = Invoke-Gcloud @("compute", "firewall-rules", "update", $rule, "--source-ranges=$AllowRdpFrom")
} else {
  $r = Invoke-Gcloud @("compute", "firewall-rules", "create", $rule, "--network=default",
    "--direction=INGRESS", "--allow=tcp:3389", "--source-ranges=$AllowRdpFrom", "--target-tags=$Tag",
    "--description=RDP to Volumetric Studio dev VMs from the developer's IP")
}
if ($r.Code -ne 0) { throw "Firewall rule $rule failed:`n$($r.Out)" }
Write-Host "RDP allowed from $AllowRdpFrom (rule $rule)"
$open = Invoke-Gcloud @("compute", "firewall-rules", "list",
  "--filter=sourceRanges:0.0.0.0/0 AND allowed[].ports:3389", "--format=value(name)")
if ($open.Code -eq 0 -and $open.Out.Trim()) {
  Write-Warning "These rules open RDP to the whole internet and apply to every VM on their network: $($open.Out.Trim() -replace "`n", ', '). Consider deleting them."
}

# --- candidates -----------------------------------------------------------------

$regionQuota = @{}
function Get-RegionFree([string] $Region, [string] $Metric) {
  if (-not $regionQuota.ContainsKey($Region)) {
    $regionQuota[$Region] = (Get-Json @("compute", "regions", "describe", $Region)).quotas
  }
  Get-FreeQuota $regionQuota[$Region] $Metric
}

$attempts = @()
foreach ($gpu in $Gpus) {
  $type = $GpuTypes[$gpu]
  $zones = (Invoke-Gcloud @("compute", "accelerator-types", "list",
    "--filter=name=$($type.Accelerator)", "--format=value(zone)")).Out -split "`n" |
    ForEach-Object { ($_.Trim() -split '/')[-1] } | Where-Object { $_ -match '^[a-z]+-[a-z]+\d+-[a-z]$' }
  $byRegion = $zones | Group-Object { $_ -replace '-[a-z]$', '' }
  $ordered = @()
  foreach ($region in $Regions) { $ordered += @($byRegion | Where-Object Name -eq $region) }
  if (-not $OnlyPreferredRegions) {
    $ordered += @($byRegion | Where-Object { $Regions -notcontains $_.Name } | Sort-Object Name)
  }
  foreach ($group in $ordered) {
    $free = Get-RegionFree $group.Name $type.Quota
    if ($free -lt 1) {
      Write-Host "  skip $($group.Name): $($type.Quota) quota has $free free"
      continue
    }
    foreach ($zone in ($group.Group | Sort-Object)) {
      $attempts += [pscustomobject]@{ Gpu = $gpu; Zone = $zone; Accelerator = $type.Accelerator }
    }
  }
}
if (-not $attempts) {
  throw ("No zone has free quota for $($Gpus -join ' or ') virtual workstation GPUs. Request " +
    "NVIDIA_L4_VWS_GPUS or NVIDIA_T4_VWS_GPUS in a region at https://console.cloud.google.com/iam-admin/quotas?project=$Project.")
}

# --- create -----------------------------------------------------------------------

$metadata = "vstudio-repo=$RepoUrl,vstudio-branch=$Branch,vstudio-build=$(if ($NoBuild) { 0 } else { 1 }),enable-windows-ssh=FALSE"
$created = $null
foreach ($a in $attempts) {
  $machine = if ($a.Gpu -eq "L4") { $L4MachineType } else { $T4MachineType }
  Write-Host "Trying $($a.Gpu) ($machine) in $($a.Zone)..."
  $r = Invoke-Gcloud @("compute", "instances", "create", $Name,
    "--zone=$($a.Zone)", "--machine-type=$machine",
    "--accelerator=type=$($a.Accelerator),count=1", "--maintenance-policy=TERMINATE",
    "--image-family=windows-2022", "--image-project=windows-cloud",
    "--boot-disk-size=$($DiskGb)GB", "--boot-disk-type=pd-balanced",
    "--tags=$Tag", "--metadata=$metadata",
    "--metadata-from-file=windows-startup-script-ps1=$Bootstrap")
  if ($r.Code -eq 0) { $created = $a; break }
  $reason = ($r.Out -split "`n" | Where-Object { $_ -match 'ERROR|code|message' } | Select-Object -First 2) -join ' '
  Write-Host "  failed: $reason"
  if ($r.Out -notmatch 'RESOURCE_POOL_EXHAUSTED|does not have enough resources|QUOTA|quota|not supported|unavailable|STOCKOUT') {
    throw "Create failed for a reason another zone won't fix:`n$($r.Out)"
  }
}
if (-not $created) { throw "No zone had capacity. Try again later, or pass -Gpus T4 / more -Regions." }

$ip = (Invoke-Gcloud @("compute", "instances", "describe", $Name, "--zone=$($created.Zone)",
  "--format=value(networkInterfaces[0].accessConfigs[0].natIP)")).Out.Trim()
Write-Host ""
Write-Host "Created ${Name}: $($created.Gpu) in $($created.Zone), external IP $ip" -ForegroundColor Green

if ($WindowsUser) {
  Write-Host "Waiting for Windows to come up to set a password for $WindowsUser (a few minutes)..."
  $r = Invoke-Gcloud @("compute", "reset-windows-password", $Name, "--zone=$($created.Zone)", "--user=$WindowsUser")
  $r.Out
}

Write-Host @"

Next:
  - The bootstrap runs on first boot and takes about an hour (driver, Build
    Tools, cache download, app build). Follow it with
      gcloud compute instances get-serial-port-output $Name --zone=$($created.Zone)
    or, once signed in, C:\vstudio-bootstrap\bootstrap.log. It ends by writing
    C:\Users\Public\Desktop\vstudio-next-steps.txt.
$(if (-not $WindowsUser) { "  - Set a password: gcloud compute reset-windows-password $Name --zone=$($created.Zone) --user=<name>`n" })  - RDP to $ip, then do the manual steps in docs\dev-vm.md (gh auth login, Claude app).
  - Stop it when idle (GPU VMs bill by the hour):
      gcloud compute instances stop $Name --zone=$($created.Zone)
"@
