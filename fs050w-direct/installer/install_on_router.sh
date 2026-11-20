#!/bin/sh
# Install a prepared, owner-specific payload; debug bootstrap needs explicit opt-in.
set -eu
umask 077
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

die() { echo "FS050W SMS: $*" >&2; exit 1; }
enable_debug=0
case "${1:-}" in
  -d|--enable-debug-mode) enable_debug=1; shift ;;
esac
[ "$#" -le 1 ] || die '用法：sh install_on_router.sh [-d|--enable-debug-mode] [已準備的資料夾]'
payload=$(CDPATH='' cd -P "${1:-$(dirname "$0")}" && pwd) || die '找不到安裝資料夾'
case "$payload" in /m_data/*|/usrdata/*) ;; *) die '請先把安裝資料夾放在設備的持久儲存區' ;; esac
state=/m_data/usr/.fs050w-sms-install
home=/m_webui/webui/common/home.html
mounted=/m_data/usr/sms-home.html
binary=/m_data/usr/sm3
hook=/m_data/usr/debug_mode.sh
socket=/tmp/m_tmp/ipc/sms
original_home_sha=825029c449238980927c213f86e62dddf3be6d0a4e3802a38e54b034cb7d4815
binary_sha=fda2b5885897876bf47f76f58a10b2d33f1b72aa46a3a8f40855b46629a25558
firmware_manifest_sha=e82e4eebc58cdfb6ccd14550187a6bd1da05afbe5fde882047b33c28ea7f2fc9
committed=0
state_owned=0

setting() {
  value=$(mgdb_tool get "$1") || die "無法讀取 $1"
  case "$value" in
    "mgdb_tool get $1="*" success") value=${value#"mgdb_tool get $1="}; value=${value%" success"} ;;
    *) die "讀取 $1 時格式不符" ;;
  esac
  printf '%s\n' "$value"
}

mount_active() {
  awk -v target="$home" '$2==target{found=1} END{exit !found}' /proc/mounts
}

service_holds_socket() {
  # Kernel socket inode fields contain only decimal digits, with no whitespace.
  # shellcheck disable=SC2013
  for inode in $(awk -v path="$socket" '$8==path && $7~/^[0-9]+$/{print $7}' /proc/net/unix); do
    for fd in /proc/"$service_pid"/fd/*; do
      [ "$(readlink "$fd" 2>/dev/null || true)" != "socket:[$inode]" ] || return 0
    done
  done
  return 1
}

on_exit() {
  result=$1
  trap - 0
  if [ "$state_owned" -eq 1 ]; then
    rm -f "$mounted.stage.$$" "$binary.stage.$$" "$hook.stage.$$"
  fi
  if [ "$state_owned" -eq 1 ] && [ "$committed" -ne 1 ] && [ -f "$state/backup-ready" ]; then
    echo 'FS050W SMS: 安裝未完成，正在回復備份。' >&2
    sh "$payload/rollback_on_router.sh" ||
      echo "FS050W SMS: 自動回復失敗，請保留 $state 並手動檢查。" >&2
  elif [ "$state_owned" -eq 1 ] && [ "$committed" -ne 1 ] &&
       [ -d "$state" ] && [ ! -L "$state" ]; then
    rm -f "$state/original-home.html" "$state/original-debug-mode.sh" \
      "$state/BACKUPS.sha256" "$state/apn-profile-before" \
      "$state/binary.sha256" "$state/home.sha256" "$state/hook.sha256" \
      "$state/hook-created" "$state/marker-created"
    rmdir "$state" 2>/dev/null || true
  fi
  exit "$result"
}
trap 'on_exit $?' 0
trap 'exit 1' 1 2 3 15

[ "$(id -u)" = 0 ] || die '需要既有的 root 維護 shell'
case "$(uname -m)" in armv7*) ;; *) die '只支援 FS050W ARMv7 環境' ;; esac
[ "$(setting device_software_version)" = V2.1.0 ] || die '設備不是 V2.1.0'
for name in home.html sm3 V2.1.0.sha256 install_on_router.sh rollback_on_router.sh PAYLOAD.sha256; do
  [ -f "$payload/$name" ] && [ ! -L "$payload/$name" ] || die "缺少或拒絕符號連結：$name"
done
(cd "$payload" && sha256sum -c PAYLOAD.sha256 >/dev/null) || die '安裝包校驗失敗'
[ "$(sha256sum "$payload/sm3" | awk '{print $1}')" = "$binary_sha" ] || die 'SMS 模組不是已驗證版本'
[ "$(sha256sum "$payload/V2.1.0.sha256" | awk '{print $1}')" = "$firmware_manifest_sha" ] ||
  die '韌體雜湊清單不是已驗證版本'
(cd / && sha256sum -c "$payload/V2.1.0.sha256" >/dev/null) || die '韌體函式庫雜湊不符'

[ "$(setting dialup_dataswitch)" = on ] || die '數據開關必須為 on'
[ "$(setting dialup_dial_status)" = connected ] || die '行動數據必須已連線'
[ "$(setting mnet_sim_status)" = ready ] || die 'SIM 必須就緒'
[ "$(setting lcd_forbidden_button)" = enable ] || die '實體按鍵必須可用'
[ "$(setting device_lock_button)" = disable ] || die '設備鎖定狀態異常'
profile=$(setting dialup_profile_default)
case "$profile" in ''|*[!0-9]*) die '無法辨識目前 APN profile' ;; esac
marker=/m_data/usr/device_debug_mode
marker_present=0
for candidate in "$marker" /m_data/usr_permanent/device_debug_mode; do
  [ ! -L "$candidate" ] || die '除錯標記是符號連結，停止安裝'
  if [ -e "$candidate" ]; then
    [ -f "$candidate" ] || die '除錯標記不是一般檔案'
    marker_present=1
  fi
done
[ "$marker_present" -eq 1 ] || [ "$enable_debug" -eq 1 ] ||
  die '既有的 device_debug_mode 標記不存在；要明確選擇 -d 才會啟用除錯模式'
[ -d /m_data/usr ] && [ -w /m_data/usr ] && [ ! -L /m_data/usr ] || die '/m_data/usr 不可寫'
hook_present=0
if [ -e "$hook" ] || [ -L "$hook" ]; then
  [ -f "$hook" ] && [ ! -L "$hook" ] && [ -x "$hook" ] || die '既有的 debug_mode.sh 開機掛鉤不可用'
  hook_present=1
  first_line=$(sed -n '1p' "$hook")
  case "$first_line" in '#!/bin/sh'|'#!/bin/ash') ;; *) die '開機掛鉤不是已知的 shell 腳本' ;; esac
  sh -n "$hook" || die '原開機掛鉤語法檢查失敗'
else
  [ "$enable_debug" -eq 1 ] || die '既有的 debug_mode.sh 不存在；要明確選擇 -d 才會建立'
  first_line='#!/bin/sh'
fi
[ -f "$home" ] && [ ! -L "$home" ] || die '原廠首頁不可用'
[ "$(sha256sum "$home" | awk '{print $1}')" = "$original_home_sha" ] || die '原廠首頁雜湊不符或已修改'
if mount_active; then die '首頁已被掛載，請先查明目前狀態'; fi
for path in "$mounted" "$binary" "$state"; do
  [ ! -e "$path" ] && [ ! -L "$path" ] || die "已有安裝檔案：$path"
done
if [ -e "$socket" ] || [ -L "$socket" ] ||
   awk -v path="$socket" '$8==path{found=1} END{exit !found}' /proc/net/unix ||
   pidof sm3 >/dev/null 2>&1; then
  die 'SMS IPC 位置已有服務，請先檢查既有安裝'
fi
if [ "$hook_present" -eq 1 ] && grep -Eq '(sm3|fs050w-sms|/tmp/m_tmp/ipc/sms)' "$hook"; then
  die '既有開機掛鉤已包含 SMS 服務，請先檢查既有安裝'
fi
if [ "$enable_debug" -eq 1 ] && { [ "$marker_present" -eq 0 ] || [ "$hook_present" -eq 0 ]; }; then
  echo 'FS050W SMS: 已明確選擇 -d；啟用 device_debug_mode 可能擴大原廠網頁除錯權限。' >&2
fi

mkdir -m 700 "$state" || die '無法建立私有備份資料夾'
state_owned=1
cp -p "$home" "$state/original-home.html" || die '無法備份原廠首頁'
if [ "$hook_present" -eq 1 ]; then
  cp -p "$hook" "$state/original-debug-mode.sh" || die '無法備份原開機掛鉤'
else
  printf '#!/bin/sh\n' > "$state/original-debug-mode.sh"
  chmod 0755 "$state/original-debug-mode.sh"
  touch "$state/hook-created"
fi
if [ "$marker_present" -eq 0 ]; then touch "$state/marker-created"; fi
(
  cd "$state"
  sha256sum original-home.html original-debug-mode.sh > BACKUPS.sha256
) || die '無法記錄備份雜湊'
printf '%s\n' "$profile" > "$state/apn-profile-before"
printf '%s\n' "$binary_sha" > "$state/binary.sha256"
sha256sum "$payload/home.html" | awk '{print $1}' > "$state/home.sha256"

# The injected work runs only on `start`. The original hook body is retained byte for byte.
hook_stage="$hook.stage.$$"
case "$first_line" in '#!'*) printf '%s\n' "$first_line" > "$hook_stage" ;; *) : > "$hook_stage" ;; esac
printf '# FS050W_SMS_INSTALL_BEGIN\nfs050w_sms_home_sha=%s\nfs050w_sms_binary_sha=%s\n' \
  "$(cat "$state/home.sha256")" "$binary_sha" >> "$hook_stage"
cat >> "$hook_stage" <<'HOOK'
if [ "${1:-}" = start ]; then
  (
    trap '' HUP
    n=0
    while [ "$n" -lt 45 ]; do
      if [ -f /m_webui/webui/common/home.html ] && [ -f /m_data/usr/sms-home.html ]; then
        [ "$(sha256sum /m_data/usr/sms-home.html | awk '{print $1}')" = "$fs050w_sms_home_sha" ] || exit 1
        if ! awk '$2=="/m_webui/webui/common/home.html"{found=1} END{exit !found}' /proc/mounts; then
          mount --bind /m_data/usr/sms-home.html /m_webui/webui/common/home.html
        fi
        exit 0
      fi
      sleep 2
      n=$((n+1))
    done
  ) </dev/null >/m_data/usr/.fs050w-sms-install/mount.log 2>&1 &
  (
    trap '' HUP
    n=0
    while [ "$n" -lt 45 ]; do
      if [ -d /tmp/m_tmp/ipc ] && pidof mal_service >/dev/null 2>&1; then break; fi
      sleep 2
      n=$((n+1))
    done
    [ -d /tmp/m_tmp/ipc ] && pidof mal_service >/dev/null 2>&1 || exit 1
    sleep 10
    if [ ! -e /tmp/m_tmp/ipc/sms ] && ! pidof sm3 >/dev/null 2>&1 &&
       [ -x /m_data/usr/sm3 ]; then
      [ "$(sha256sum /m_data/usr/sm3 | awk '{print $1}')" = "$fs050w_sms_binary_sha" ] || exit 1
      exec /m_data/usr/sm3 > /m_data/usr/.fs050w-sms-install/service.log 2>&1
    fi
  ) </dev/null >/dev/null 2>&1 &
fi
# FS050W_SMS_INSTALL_END
HOOK
case "$first_line" in '#!'*) sed '1d' "$state/original-debug-mode.sh" >> "$hook_stage" ;; *) cat "$state/original-debug-mode.sh" >> "$hook_stage" ;; esac
sh -n "$hook_stage" || die '新開機掛鉤語法檢查失敗'
chmod "$(stat -c '%a' "$state/original-debug-mode.sh")" "$hook_stage"
sha256sum "$hook_stage" | awk '{print $1}' > "$state/hook.sha256"
touch "$state/backup-ready"

# /m_data/usr is writable; stage in the same filesystem and rename into place.
cp "$payload/home.html" "$mounted.stage.$$"
chmod 0644 "$mounted.stage.$$"
mv "$mounted.stage.$$" "$mounted"
cp "$payload/sm3" "$binary.stage.$$"
chmod 0755 "$binary.stage.$$"
mv "$binary.stage.$$" "$binary"

# Verify the replacement in this boot before making the boot hook persistent.
mount --bind "$mounted" "$home" || die '無法覆蓋唯讀首頁'
[ "$(sha256sum "$home" | awk '{print $1}')" = "$(cat "$state/home.sha256")" ] || die '覆蓋後首頁不符'
if [ ! -d /tmp/m_tmp/ipc ] || ! pidof mal_service >/dev/null 2>&1; then
  die 'MAL 或 IPC 不可用'
fi
nohup "$binary" > "$state/service.log" 2>&1 </dev/null &
service_pid=$!
printf '%s %s\n' "$service_pid" "$(awk '{print $22}' "/proc/$service_pid/stat")" > "$state/service-pid"
n=0
while [ "$n" -lt 12 ]; do
  if [ -S "$socket" ] && kill -0 "$service_pid" 2>/dev/null && service_holds_socket; then break; fi
  sleep 1
  n=$((n+1))
done
if [ ! -S "$socket" ] || ! kill -0 "$service_pid" 2>/dev/null || ! service_holds_socket; then
  die 'SMS 模組未建立 IPC socket'
fi
[ "$(readlink "/proc/$service_pid/exe")" = "$binary" ] || die 'SMS 行程執行檔不符'
mv "$hook_stage" "$hook" || die '開機掛鉤原子替換失敗'
[ "$(sha256sum "$hook" | awk '{print $1}')" = "$(cat "$state/hook.sha256")" ] || die '開機掛鉤替換後雜湊不符'
if [ -f "$state/marker-created" ]; then
  [ ! -e "$marker" ] && [ ! -L "$marker" ] || die '除錯標記出現其他變更'
  : > "$marker" || die '無法建立除錯標記'
  [ -f "$marker" ] && [ ! -L "$marker" ] && [ ! -s "$marker" ] || die '除錯標記建立後不符'
fi
touch "$state/installed"
committed=1
echo 'FS050W SMS: 安裝完成。請在原廠管理頁開啟「簡訊中心」並實際核對收件與發送。'
echo "FS050W SMS: 原始備份位於 $state；回復：sh $payload/rollback_on_router.sh"
