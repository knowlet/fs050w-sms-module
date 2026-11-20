#!/bin/sh
# Run through an already provisioned maintenance console on FS050W V2.1.0.
set -eu
umask 077
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
kit=$(CDPATH='' cd -P "$(dirname "$0")" && pwd)
state=/run/fs050w-sms-owner
socket=/tmp/m_tmp/ipc/sms
binary=$kit/bin/fs050w-sms
action=${1:-check}

die() { echo "$*" >&2; exit 1; }
case "$action" in
  check|start|stop|trial|commit|restore-trial|restore-original) ;;
  *) die "Usage: sh apply.sh [check|start|stop|trial|commit|restore-trial|restore-original]" ;;
esac
[ "$#" -le 1 ] || die "Unexpected parameters"
case "$(uname -m)" in armv7*) ;; *) die "Requires the FS050W ARMv7 maintenance environment." ;; esac
(cd "$kit" && sha256sum -c BIN.sha256 >/dev/null) || die "Kit integrity check failed."
(cd / && sha256sum -c "$kit/V2.1.0.sha256" >/dev/null) || die "Unsupported firmware library or modem service."
probe=$kit/bin/fs050w-nas-status
setting() {
  value=$(mgdb_tool get "$1") || die "Could not read $1"
  case "$value" in
    "mgdb_tool get $1="*" success") value=${value#"mgdb_tool get $1="}; value=${value%" success"} ;;
    *) die "Unexpected response while reading $1" ;;
  esac
  printf '%s\n' "$value"
}
safe_network() {
  [ "$(setting dialup_dataswitch)" = on ] && [ "$(setting dialup_dial_status)" = connected ] &&
    [ "$(setting mnet_sim_status)" = ready ] && [ "$(setting lcd_forbidden_button)" = enable ] &&
    [ "$(setting device_lock_button)" = disable ] || die "Check data switch, connection, SIM, buttons and device lock."
  profile=$(setting dialup_profile_default)
  case "$profile" in ''|*[!0-9]*) die "Unable to identify the current APN profile." ;; esac
  printf 'Connected, SIM ready, physical buttons enabled, APN profile %s.\n' "$profile"
}
if [ "$action" = check ]; then
  safe_network
  if [ -S "$socket" ]; then echo 'SMS socket present'; else echo 'SMS socket absent'; fi
  exec "$probe"
fi
if ! mkdir -m 700 "$state" 2>/dev/null; then
  [ -d "$state" ] && [ ! -L "$state" ] && [ "$(stat -c '%u:%a' "$state")" = "$(id -u):700" ] ||
    die "Cannot verify private runtime state directory."
fi
mkdir "$state/lock" 2>/dev/null || die "Another operation holds the runtime lock."
trap 'rmdir "$state/lock"' EXIT
trap 'exit 1' HUP INT TERM
kit_pid() {
  [ -r "$state/pid" ] || return 1
  read -r pid started < "$state/pid" || return 1
  case "$pid" in ''|0|1|*[!0-9]*) return 1 ;; esac
  [ "$(readlink "/proc/$pid/exe")" = "$binary" ] &&
    [ "$(awk '{print $22}' "/proc/$pid/stat")" = "$started" ]
}
holds_socket() {
  # /proc/net/unix lists the kernel socket inode in column 7 and path in column 8.
  # Kernel inode fields consist exclusively of decimal digits.
  # shellcheck disable=SC2013
  for inode in $(awk -v path="$socket" '$8==path && $7~/^[0-9]+$/{print $7}' /proc/net/unix); do
    for fd in /proc/"$pid"/fd/*; do
      [ "$(readlink "$fd")" != "socket:[$inode]" ] || return 0
    done
  done
  return 1
}
if [ "$action" = start ]; then
  safe_network
  if kit_pid && holds_socket; then echo 'Kit SMS service is already running.'; exit 0; fi
  if kit_pid || [ -e "$socket" ] || [ -L "$socket" ] || awk -v p="$socket" '$8==p{found=1}END{exit !found}' /proc/net/unix; then
    die "SMS slot is occupied. Check the current service using the web interface."
  fi
  nohup "$binary" >"$state/service.log" 2>&1 </dev/null &
  pid=$!
  started=$(awk '{print $22}' "/proc/$pid/stat") || die "SMS service exited during startup."
  printf '%s %s\n' "$pid" "$started" >"$state/pid"
  tries=0
  while [ "$tries" -lt 12 ]; do
    sleep 1
    kit_pid || die "SMS service exited during startup. See $state/service.log"
    if [ -S "$socket" ] && holds_socket; then
      stat -c '%d:%i' "$socket" >"$state/socket-id"
      echo 'SMS ready. Open the inbox to verify.'
      exit 0
    fi
    tries=$((tries+1))
  done
  kit_pid && kill -TERM "$pid"
  die "Service failed to claim the SMS socket. See $state/service.log"
fi
if [ "$action" = stop ]; then
  kit_pid || die "No matching service from this copy of the kit is running."
  kill -TERM "$pid"
  tries=0
  while kit_pid && [ "$tries" -lt 8 ]; do sleep 1; tries=$((tries+1)); done
  if kit_pid; then die "Service has not exited. Inspect the maintenance console."; fi
  if [ -S "$socket" ] && [ -f "$state/socket-id" ] &&
     [ "$(stat -c '%d:%i' "$socket")" = "$(cat "$state/socket-id")" ] &&
     ! awk -v p="$socket" '$8==p{found=1}END{exit !found}' /proc/net/unix; then
    rm -f "$socket"
  fi
  rm -f "$state/pid" "$state/socket-id"
  echo 'Kit SMS service stopped.'
  exit 0
fi
nas_matches() {
  grep -qx "service_domain_preference value=$2" "$1" &&
    grep -qx "lte_sms_domain value=$3" "$1" &&
    grep -q "^serving registration=1 cs_attach=$4 ps_attach=1 " "$1"
}
selection() { grep -E '^(rat_mode_preference|lte_band_preference|network_selection_preference) len=' "$1"; }
case "$action" in
  trial)
    safe_network
    [ ! -f "$state/phase" ] || die "An earlier trial/commit has been recorded. Inspect or restore first."
    "$probe" >"$state/before"
    nas_matches "$state/before" 1 0 2 || die "Expected PS only, CS detached, PS attached and no LTE SMS domain."
    selection "$state/before" >"$state/selection"
    [ "$(wc -l < "$state/selection")" -eq 3 ] || die "Missing selection diagnostics."
    printf '%s\n' "$profile" >"$state/profile"
    echo trial-pending >"$state/phase"
    "$kit/bin/fs050w-nas-csps-trial" || die "Check current state, then run restore-trial."
    echo "Trial submitted. Check cellular data and test actual SMS in both directions before commit."
    ;;
  commit)
    [ -f "$state/phase" ] && [ "$(cat "$state/phase")" = trial-pending ] || die "First perform a temporary trial in this boot session."
    safe_network
    [ "$profile" = "$(cat "$state/profile")" ] || die "APN profile changed during the trial."
    "$probe" >"$state/after"
    nas_matches "$state/after" 2 3 1 || die "CS/PS attachment and 3GPP SMS service must be ready."
    [ "$(selection "$state/after")" = "$(cat "$state/selection")" ] || die "RAT, band or network selection changed."
    echo commit-pending >"$state/phase"
    "$kit/bin/fs050w-nas-csps-permanent" || die "Permanent result uncertain. Check state; use restore-original for the original baseline."
    echo permanent >"$state/phase"
    echo "Permanent CS+PS written. Verify with check after a normal reboot."
    ;;
  restore-trial)
    [ -f "$state/phase" ] && [ "$(cat "$state/phase")" = trial-pending ] || die "No matching temporary trial."
    "$kit/bin/fs050w-nas-ps-restore"
    rm -f "$state/phase"
    echo "Restored temporary PS-only preference. Run check."
    ;;
  restore-original)
    # This explicit action also supports restoring after reboot erased the trial record.
    "$probe" >"$state/current"
    grep -qx 'service_domain_preference value=2' "$state/current" ||
      die "Current preference is not CS+PS. Confirm the original preference and the readback after reboot."
    "$kit/bin/fs050w-nas-ps-permanent-restore"
    rm -f "$state/phase"
    echo "Restored original permanent PS-only preference. Run check."
    ;;
esac
