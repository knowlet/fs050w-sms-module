#!/bin/sh
# Restore only files created by install_on_router.sh; leave the owner's hook intact.
set -eu
umask 077
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

die() { echo "FS050W SMS 回復：$*" >&2; exit 1; }
[ "$#" -eq 0 ] || die '用法：sh rollback_on_router.sh'
[ "$(id -u)" = 0 ] || die '需要既有的 root 維護 shell'
state=/m_data/usr/.fs050w-sms-install
home=/m_webui/webui/common/home.html
mounted=/m_data/usr/sms-home.html
binary=/m_data/usr/sm3
hook=/m_data/usr/debug_mode.sh
marker=/m_data/usr/device_debug_mode
socket=/tmp/m_tmp/ipc/sms
original_home_sha=825029c449238980927c213f86e62dddf3be6d0a4e3802a38e54b034cb7d4815
[ -d "$state" ] && [ ! -L "$state" ] && [ -f "$state/backup-ready" ] || die '找不到本安裝器的備份'
[ ! -f "$state/rolled-back" ] || { echo 'FS050W SMS 回復：已回復。'; exit 0; }
for name in BACKUPS.sha256 original-home.html original-debug-mode.sh home.sha256 binary.sha256 hook.sha256; do
  [ -f "$state/$name" ] && [ ! -L "$state/$name" ] || die "備份不完整：$name"
done
(cd "$state" && sha256sum -c BACKUPS.sha256 >/dev/null) || die '原始備份雜湊不符'
[ "$(sha256sum "$state/original-home.html" | awk '{print $1}')" = "$original_home_sha" ] || die '原廠首頁備份不符'
read -r home_sha < "$state/home.sha256"
read -r binary_sha < "$state/binary.sha256"
read -r hook_sha < "$state/hook.sha256"
original_hook_sha=$(sha256sum "$state/original-debug-mode.sh" | awk '{print $1}')
hook_created=0
marker_created=0
if [ -e "$state/hook-created" ] || [ -L "$state/hook-created" ]; then
  [ -f "$state/hook-created" ] && [ ! -L "$state/hook-created" ] && [ ! -s "$state/hook-created" ] ||
    die '開機掛鉤來源記錄異常'
  hook_created=1
fi
if [ -e "$state/marker-created" ] || [ -L "$state/marker-created" ]; then
  [ -f "$state/marker-created" ] && [ ! -L "$state/marker-created" ] && [ ! -s "$state/marker-created" ] ||
    die '除錯標記來源記錄異常'
  marker_created=1
fi

# Check every managed path before changing anything. Refuse to overwrite later edits.
current_hook_sha=
if [ -e "$hook" ] || [ -L "$hook" ]; then
  [ -f "$hook" ] && [ ! -L "$hook" ] || die '開機掛鉤變成符號連結或其他類型'
  current_hook_sha=$(sha256sum "$hook" | awk '{print $1}')
  [ "$current_hook_sha" = "$hook_sha" ] ||
    { [ "$hook_created" -eq 0 ] && [ "$current_hook_sha" = "$original_hook_sha" ]; } ||
    die '開機掛鉤已被其他操作修改，停止以免覆蓋變更'
else
  [ "$hook_created" -eq 1 ] || die '原有開機掛鉤消失'
fi
if [ "$marker_created" -eq 1 ] && { [ -e "$marker" ] || [ -L "$marker" ]; }; then
  [ -f "$marker" ] && [ ! -L "$marker" ] && [ ! -s "$marker" ] ||
    die '除錯標記已被其他操作修改，停止以免覆蓋變更'
fi
if awk -v target="$home" '$2==target{found=1} END{exit !found}' /proc/mounts; then
  [ "$(sha256sum "$home" | awk '{print $1}')" = "$home_sha" ] ||
    die '首頁掛載內容不是本安裝器版本'
else
  [ "$(sha256sum "$home" | awk '{print $1}')" = "$original_home_sha" ] ||
    die '目前首頁不是原廠版本'
fi
if [ -e "$mounted" ] || [ -L "$mounted" ]; then
  [ -f "$mounted" ] && [ ! -L "$mounted" ] &&
    [ "$(sha256sum "$mounted" | awk '{print $1}')" = "$home_sha" ] ||
    die '補丁首頁已被其他操作修改'
fi
if [ -e "$binary" ] || [ -L "$binary" ]; then
  [ -f "$binary" ] && [ ! -L "$binary" ] &&
    [ "$(sha256sum "$binary" | awk '{print $1}')" = "$binary_sha" ] ||
    die 'SMS 模組已被其他操作修改'
fi

# Stop only the service installed at this path. Validate all candidates first.
service_pids=$(pidof sm3 2>/dev/null || true)
for pid in $service_pids; do
  case "$pid" in ''|0|1|*[!0-9]*) die 'SMS 行程編號異常' ;; esac
  exe=$(readlink "/proc/$pid/exe" 2>/dev/null || true)
  [ "$exe" = "$binary" ] || die "發現非本安裝器的 SMS 行程 $pid"
done
for pid in $service_pids; do
  kill -TERM "$pid" || die "無法停止 SMS 行程 $pid"
done
n=0
while pidof sm3 >/dev/null 2>&1 && [ "$n" -lt 8 ]; do sleep 1; n=$((n+1)); done
pidof sm3 >/dev/null 2>&1 && die 'SMS 行程仍在執行，請檢查後重試'
if [ -S "$socket" ]; then
  if awk -v path="$socket" '$8==path{found=1} END{exit !found}' /proc/net/unix; then
    die 'SMS IPC socket 仍有持有者'
  fi
  rm -f "$socket"
fi

# Restore the original boot hook first so a later reboot cannot relaunch the module.
if [ "$current_hook_sha" = "$hook_sha" ]; then
  if [ "$hook_created" -eq 1 ]; then
    rm -f "$hook"
  else
    cp -p "$state/original-debug-mode.sh" "$hook.rollback.$$"
    mv "$hook.rollback.$$" "$hook"
  fi
fi
if [ "$marker_created" -eq 1 ] && [ -f "$marker" ]; then rm -f "$marker"; fi

if awk -v target="$home" '$2==target{found=1} END{exit !found}' /proc/mounts; then
  umount "$home" || die '無法卸載首頁覆蓋'
fi
[ "$(sha256sum "$home" | awk '{print $1}')" = "$original_home_sha" ] || die '原廠首頁未回復'

if [ -e "$mounted" ] || [ -L "$mounted" ]; then
  rm -f "$mounted"
fi
if [ -e "$binary" ] || [ -L "$binary" ]; then
  rm -f "$binary"
fi
touch "$state/rolled-back"
rm -f "$state/installed" "$state/service-pid"
echo 'FS050W SMS 回復：原廠首頁與原開機狀態已恢復；備份保留供核對。'
