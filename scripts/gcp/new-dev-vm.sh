#!/usr/bin/env bash
# Creates a Windows Server 2022 dev VM with an NVIDIA RTX Virtual Workstation
# GPU on Google Compute Engine, trying GPUs and zones until one has capacity.
# The bash twin of New-DevVm.ps1, for macOS and Linux: it needs only the
# gcloud CLI (signed in, a project selected) and curl, and runs on macOS's
# bash 3.2 with its BSD tools.
#
#   1. Checks the project's GPU quota (GPUS_ALL_REGIONS and the per-region
#      virtual workstation GPU quota) and skips regions with none free.
#   2. Restricts RDP to this machine's public IP with the firewall rule
#      vstudio-allow-rdp on the target tag vstudio-dev.
#   3. Tries each GPU in --gpus (L4 on a G2, then T4 on an N1 by default) in
#      each zone that offers it, preferred regions first, until a create
#      succeeds. ZONE_RESOURCE_POOL_EXHAUSTED moves on to the next zone.
#   4. Attaches scripts/gcp/bootstrap-windows.ps1 as the Windows startup
#      script, which sets the VM up on first boot (docs/dev-vm.md).
#
#   scripts/gcp/new-dev-vm.sh --windows-user dallas
#   scripts/gcp/new-dev-vm.sh --gpus T4 --regions us-east1,us-west1
#   scripts/gcp/new-dev-vm.sh --dry-run      # read-only checks, prints the rest
#
# Options (defaults in brackets):
#   --name NAME               VM name [vstudio-dev]
#   --project ID              [gcloud's configured project]
#   --gpus L4,T4              GPUs in order of preference [L4,T4]
#   --regions R1,R2,...       regions tried first, in order; other regions
#                             offering the GPU follow
#                             [us-central1,us-east4,us-east1,us-west1,us-west4,us-east5,us-south1]
#   --only-preferred-regions  never try regions outside --regions
#   --l4-machine-type TYPE    [g2-standard-8]
#   --t4-machine-type TYPE    [n1-standard-8]
#   --disk-gb N               boot disk size [200]
#   --repo-url URL            [https://github.com/dgcrensh/Volumetric-Studio.git]
#   --branch BRANCH           branch the bootstrap clones and builds [main]
#   --no-build                bootstrap stops after fetching the vcpkg cache
#   --allow-rdp-from CIDR     RDP source range [this machine's public IP/32]
#   --windows-user NAME       create this Windows user once the VM is up and
#                             print its password
#   --dry-run                 run only the read-only gcloud calls (quota,
#                             zones, firewall) and print the changes it would make
#
# What this can't do for you (docs/dev-vm.md has the steps): raise GPU quota
# or upgrade a free-trial billing account, and the sign-ins on the VM.

set -u
# No pipefail: grep -q closing a pipe early must not fail the pipeline.

script_dir="$(cd "$(dirname "$0")" && pwd)"
bootstrap="$script_dir/bootstrap-windows.ps1"
tag="vstudio-dev"
rule="vstudio-allow-rdp"

name="vstudio-dev"
project=""
gpus="L4,T4"
regions="us-central1,us-east4,us-east1,us-west1,us-west4,us-east5,us-south1"
only_preferred=0
l4_machine="g2-standard-8"
t4_machine="n1-standard-8"
disk_gb=200
repo_url="https://github.com/dgcrensh/Volumetric-Studio.git"
branch="main"
build=1
allow_rdp_from=""
windows_user=""
dry_run=0

die() { printf 'new-dev-vm.sh: %s\n' "$*" >&2; exit 1; }
need_value() { [ $# -ge 2 ] && [ -n "$2" ] || die "$1 needs a value"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --name) need_value "$@"; name="$2"; shift ;;
    --project) need_value "$@"; project="$2"; shift ;;
    --gpus) need_value "$@"; gpus="$2"; shift ;;
    --regions) need_value "$@"; regions="$2"; shift ;;
    --only-preferred-regions) only_preferred=1 ;;
    --l4-machine-type) need_value "$@"; l4_machine="$2"; shift ;;
    --t4-machine-type) need_value "$@"; t4_machine="$2"; shift ;;
    --disk-gb) need_value "$@"; disk_gb="$2"; shift ;;
    --repo-url) need_value "$@"; repo_url="$2"; shift ;;
    --branch) need_value "$@"; branch="$2"; shift ;;
    --no-build) build=0 ;;
    --allow-rdp-from) need_value "$@"; allow_rdp_from="$2"; shift ;;
    --windows-user) need_value "$@"; windows_user="$2"; shift ;;
    --dry-run) dry_run=1 ;;
    -h|--help) sed -n '2,43p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown argument '$1' (see --help)" ;;
  esac
  shift
done

gpus="$(printf '%s' "$gpus" | tr 'a-z,' 'A-Z ')"
regions="$(printf '%s' "$regions" | tr ',' ' ')"
for gpu in $gpus; do
  case "$gpu" in L4|T4) ;; *) die "--gpus takes L4 and T4, not '$gpu'" ;; esac
done
case "$disk_gb" in ''|*[!0-9]*) die "--disk-gb must be a whole number" ;; esac

accelerator_of() { if [ "$1" = L4 ]; then echo nvidia-l4-vws; else echo nvidia-tesla-t4-vws; fi; }
quota_of() { if [ "$1" = L4 ]; then echo NVIDIA_L4_VWS_GPUS; else echo NVIDIA_T4_VWS_GPUS; fi; }
machine_of() { if [ "$1" = L4 ]; then echo "$l4_machine"; else echo "$t4_machine"; fi; }

work="$(mktemp -d "${TMPDIR:-/tmp}/vstudio-dev-vm.XXXXXX")" || die "mktemp failed"
trap 'rm -rf "$work"' EXIT

# Runs gcloud quietly in the project. Stdout lands in $out, stderr in $err;
# returns gcloud's exit code.
out="" err=""
gc() {
  local code
  if [ -n "$project" ]; then
    gcloud --quiet --project="$project" "$@" >"$work/out" 2>"$work/err"
  else
    gcloud --quiet "$@" >"$work/out" 2>"$work/err"
  fi
  code=$?
  out="$(cat "$work/out")"
  err="$(cat "$work/err")"
  return $code
}

# Like gc, for calls that change something: --dry-run prints them instead.
gc_change() {
  if [ $dry_run -eq 1 ]; then
    printf '  [dry run] would run: gcloud'
    printf ' %q' "$@"
    printf '\n'
    out="" err=""
    return 0
  fi
  gc "$@"
}

# Free quota of METRIC in the "metric,limit,usage" lines on stdin (0 if absent).
free_quota() {
  awk -F, -v m="$1" '$1 == m { print int($2 - $3); found = 1; exit } END { if (!found) print 0 }'
}

command -v gcloud >/dev/null 2>&1 ||
  die "gcloud not found. Install the Google Cloud CLI (https://cloud.google.com/sdk/docs/install), then run gcloud auth login."

if [ -z "$project" ]; then
  gc config get-value project
  project="$(printf '%s\n' "$out" | tail -n 1 | tr -d '[:space:]')"
  [ -n "$project" ] && [ "$project" != "(unset)" ] ||
    die "No project: pass --project or run gcloud config set project <id>."
fi
echo "Project $project"
[ $dry_run -eq 1 ] && echo "Dry run: nothing will be created or changed."

gc_change services enable compute.googleapis.com ||
  die "Could not enable the Compute Engine API:"$'\n'"$err"

if gc compute instances list --filter="name=$name" --format="value(zone)" && [ -n "$out" ]; then
  die "A VM named $name already exists in $(basename "$out"). Delete it or pass --name."
fi

# --- quota --------------------------------------------------------------------

gc compute project-info describe --flatten=quotas \
  --format="csv[no-heading](quotas.metric,quotas.limit,quotas.usage)" ||
  die "Could not read the project's quotas:"$'\n'"$err"
all_regions="$(printf '%s\n' "$out" | free_quota GPUS_ALL_REGIONS)"
if [ "$all_regions" -lt 1 ]; then
  die "GPUS_ALL_REGIONS quota is $all_regions. Request 1 or more at" \
    "https://console.cloud.google.com/iam-admin/quotas?project=$project (filter: GPUs (all regions))." \
    "Free-trial accounts must upgrade billing first. See docs/dev-vm.md."
fi

# --- firewall -----------------------------------------------------------------

if [ -z "$allow_rdp_from" ]; then
  ip="$(curl -fsS --max-time 20 https://api.ipify.org | tr -d '[:space:]')" ||
    die "Could not look up this machine's public IP; pass --allow-rdp-from <cidr>."
  [ -n "$ip" ] || die "Could not look up this machine's public IP; pass --allow-rdp-from <cidr>."
  allow_rdp_from="$ip/32"
fi
if gc compute firewall-rules describe "$rule" --format="value(name)"; then
  gc_change compute firewall-rules update "$rule" --source-ranges="$allow_rdp_from"
else
  gc_change compute firewall-rules create "$rule" --network=default \
    --direction=INGRESS --allow=tcp:3389 --source-ranges="$allow_rdp_from" --target-tags="$tag" \
    --description="RDP to Volumetric Studio dev VMs from the developer's IP"
fi || die "Firewall rule $rule failed:"$'\n'"$err"
echo "RDP allowed from $allow_rdp_from (rule $rule)"
if gc compute firewall-rules list \
  --filter="sourceRanges:0.0.0.0/0 AND allowed[].ports:3389" --format="value(name)" && [ -n "$out" ]; then
  echo "WARNING: these rules open RDP to the whole internet and apply to every VM on" \
    "their network: $(printf '%s' "$out" | paste -sd, - | sed 's/,/, /g'). Consider deleting them." >&2
fi

# --- candidates ---------------------------------------------------------------

# Writes REGION's "metric,limit,usage" quota lines to $work/region-REGION,
# describing each region once. Not called in $(...), so die exits the script.
describe_region() {
  [ -f "$work/region-$1" ] && return 0
  gc compute regions describe "$1" --flatten=quotas \
    --format="csv[no-heading](quotas.metric,quotas.limit,quotas.usage)" ||
    die "Could not read quotas of $1:"$'\n'"$err"
  printf '%s\n' "$out" >"$work/region-$1"
}

attempts=""   # words GPU:ZONE, in the order to try
for gpu in $gpus; do
  accelerator="$(accelerator_of "$gpu")"
  quota="$(quota_of "$gpu")"
  gc compute accelerator-types list --filter="name=$accelerator" --format="value(zone)" ||
    die "Could not list zones offering $accelerator:"$'\n'"$err"
  zones="$(printf '%s\n' "$out" | sed 's#.*/##' | grep -E '^[a-z]+-[a-z]+[0-9]+-[a-z]$' | sort -u)"
  offered="$(printf '%s\n' "$zones" | sed 's/-[a-z]$//' | sort -u)"
  ordered=""
  for region in $regions; do
    printf '%s\n' "$offered" | grep -qx "$region" && ordered="$ordered $region"
  done
  if [ $only_preferred -eq 0 ]; then
    for region in $offered; do
      case " $regions " in *" $region "*) ;; *) ordered="$ordered $region" ;; esac
    done
  fi
  for region in $ordered; do
    describe_region "$region"
    free="$(free_quota "$quota" <"$work/region-$region")"
    if [ "$free" -lt 1 ]; then
      echo "  skip $region: $quota quota has $free free"
      continue
    fi
    for zone in $(printf '%s\n' "$zones" | grep "^$region-[a-z]\$"); do
      attempts="$attempts $gpu:$zone"
    done
  done
done
[ -n "$attempts" ] ||
  die "No zone has free quota for ${gpus// / or } virtual workstation GPUs." \
    "Request NVIDIA_L4_VWS_GPUS or NVIDIA_T4_VWS_GPUS in a region at" \
    "https://console.cloud.google.com/iam-admin/quotas?project=$project."

# --- create -------------------------------------------------------------------

metadata="vstudio-repo=$repo_url,vstudio-branch=$branch,vstudio-build=$build,enable-windows-ssh=FALSE"
if [ $dry_run -eq 1 ]; then
  echo "Would try, in order:$attempts"
fi
created_gpu="" created_zone=""
for attempt in $attempts; do
  gpu="${attempt%%:*}"
  zone="${attempt#*:}"
  machine="$(machine_of "$gpu")"
  echo "Trying $gpu ($machine) in $zone..."
  if gc_change compute instances create "$name" \
      --zone="$zone" --machine-type="$machine" \
      --accelerator="type=$(accelerator_of "$gpu"),count=1" --maintenance-policy=TERMINATE \
      --image-family=windows-2022 --image-project=windows-cloud \
      --boot-disk-size="${disk_gb}GB" --boot-disk-type=pd-balanced \
      --tags="$tag" --metadata="$metadata" \
      --metadata-from-file="windows-startup-script-ps1=$bootstrap"; then
    created_gpu="$gpu" created_zone="$zone"
    break
  fi
  echo "  failed: $(printf '%s\n' "$err" | grep -E 'ERROR|code|message' | head -n 2 | tr '\n' ' ')"
  printf '%s\n' "$err" |
    grep -Eq 'RESOURCE_POOL_EXHAUSTED|does not have enough resources|QUOTA|quota|not supported|unavailable|STOCKOUT' ||
    die "Create failed for a reason another zone won't fix:"$'\n'"$err"
done
[ -n "$created_zone" ] ||
  die "No zone had capacity. Try again later, or pass --gpus T4 / more --regions."

if [ $dry_run -eq 1 ]; then
  echo ""
  echo "Dry run done: the first create above is the one a real run starts with."
  exit 0
fi

gc compute instances describe "$name" --zone="$created_zone" \
  --format="value(networkInterfaces[0].accessConfigs[0].natIP)"
ip="$out"
echo ""
echo "Created $name: $created_gpu in $created_zone, external IP $ip"

if [ -n "$windows_user" ]; then
  echo "Waiting for Windows to come up to set a password for $windows_user (a few minutes)..."
  if gc compute reset-windows-password "$name" --zone="$created_zone" --user="$windows_user"; then
    printf '%s\n' "$out"
  else
    echo "Setting the password failed; retry with:" >&2
    echo "  gcloud compute reset-windows-password $name --zone=$created_zone --user=$windows_user" >&2
    printf '%s\n' "$err" >&2
  fi
fi

cat <<EOF

Next:
  - The bootstrap runs on first boot and takes about an hour (driver, Build
    Tools, cache download, app build). Follow it with
      gcloud compute instances get-serial-port-output $name --zone=$created_zone
    or, once signed in, C:\\vstudio-bootstrap\\bootstrap.log. It ends by writing
    C:\\Users\\Public\\Desktop\\vstudio-next-steps.txt.
EOF
if [ -z "$windows_user" ]; then
  echo "  - Set a password: gcloud compute reset-windows-password $name --zone=$created_zone --user=<name>"
fi
cat <<EOF
  - RDP to $ip, then do the manual steps in docs/dev-vm.md (gh auth login, Claude app).
  - Stop it when idle (GPU VMs bill by the hour):
      gcloud compute instances stop $name --zone=$created_zone
EOF
