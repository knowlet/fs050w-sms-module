#!/usr/bin/env python3
"""Prepare and install the FS050W SMS package through the owner NTP channel.

No firmware changes are attempted unless the exact V2.1.0 original home page
matches. The router-side installer performs the final firmware and safety
checks and keeps its rollback payload in /m_data/usr/fs050w-install.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import sys
import tarfile
import tempfile
import time
from pathlib import Path

from ntp_shell import (MaintenanceError, NtpShell, RouterAPI, find_host_ip,
                       requested_password, serve_bytes)
from patch_home import ORIGINAL_SHA256
from prepare_install import prepare


REMOTE_DIR = "/m_data/usr/fs050w-install"
ROOT = Path(__file__).resolve().parent


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def pause_if_guided(guided: bool, description: str) -> None:
    print(description)
    if guided:
        answer = input("繼續？輸入 yes：").strip().lower()
        if answer != "yes":
            raise MaintenanceError("使用者已取消，設備未進入下一步")


def tar_payload(directory: Path) -> bytes:
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w") as archive:
        root = tarfile.TarInfo("fs050w-install")
        root.type = tarfile.DIRTYPE
        root.mode = 0o700
        root.uid = root.gid = 0
        root.mtime = 0
        archive.addfile(root)
        for path in sorted(directory.iterdir()):
            if not path.is_file() or path.is_symlink():
                raise MaintenanceError(f"準備目錄含非一般檔案：{path.name}")
            blob = path.read_bytes()
            item = tarfile.TarInfo("fs050w-install/" + path.name)
            item.size = len(blob)
            item.mode = 0o755 if path.name in {"sm3", "install_on_router.sh", "rollback_on_router.sh"} else 0o600
            item.uid = item.gid = 0
            item.mtime = 0
            archive.addfile(item, io.BytesIO(blob))
    return data.getvalue()


def remote_capture(shell: NtpShell, host_ip: str, command: str) -> str:
    status, output, truncated = shell.run_capture(command, host_ip, max_output=2048)
    if status or truncated:
        raise MaintenanceError(f"設備檢查失敗（退出碼 {status}）：{output[:400]}")
    return output


def remote_preflight(shell: NtpShell, host_ip: str) -> tuple[bool, bool]:
    # A shell script is served from this host because the NTP parameter holds
    # only 63 bytes. It performs no writes and returns a compact status line.
    audit = r'''set -eu
case "$(uname -m)" in armv7*) ;; *) echo wrong-architecture; exit 2 ;; esac
for tool in wget tar sha256sum mgdb_tool mount umount; do command -v "$tool" >/dev/null || { echo missing-$tool; exit 2; }; done
[ -d /m_data/usr ] && [ -w /m_data/usr ] || { echo no-persistent-storage; exit 2; }
for item in /m_data/usr/fs050w-install /m_data/usr/.fs050w-sms-install /m_data/usr/sm3 /m_data/usr/sms-home.html; do
  [ ! -e "$item" ] && [ ! -L "$item" ] || { echo existing-install; exit 2; }
done
[ ! -S /tmp/m_tmp/ipc/sms ] || { echo existing-sms-socket; exit 2; }
if [ -f /m_data/usr/device_debug_mode ] || [ -f /m_data/usr_permanent/device_debug_mode ]; then marker=yes; else marker=no; fi
if [ -f /m_data/usr/debug_mode.sh ]; then hook=yes; else hook=no; fi
printf 'marker=%s hook=%s\n' "$marker" "$hook"
'''
    output = remote_capture(shell, host_ip, audit).strip()
    parts = dict(part.split("=", 1) for part in output.split() if "=" in part)
    if parts.get("marker") not in ("yes", "no") or parts.get("hook") not in ("yes", "no"):
        raise MaintenanceError(f"設備檢查結果無法辨識：{output[:120]}")
    return parts["marker"] == "yes", parts["hook"] == "yes"


def cleanup_uninstalled_stage(shell: NtpShell, host_ip: str, payload: Path) -> None:
    """Remove only this invocation's untouched files, before installer starts."""
    files = sorted(path.name for path in payload.iterdir())
    assert all(name.replace("-", "").replace(".", "").replace("_", "").isalnum() for name in files)
    expected_manifest = digest((payload / "PAYLOAD.sha256").read_bytes())
    cleanup = f'''set -eu
d=/m_data/usr/fs050w-install
[ -d "$d" ] && [ ! -L "$d" ] || exit 2
[ ! -e /m_data/usr/.fs050w-sms-install ] || exit 2
[ ! -e /m_data/usr/sm3 ] || exit 2
cd "$d"
[ "$(sha256sum PAYLOAD.sha256 | cut -c1-64)" = {expected_manifest} ] || exit 2
[ "$(ls -A | wc -l)" -eq {len(files)} ] || exit 2
for name in {' '.join(files)}; do [ -f "$name" ] && [ ! -L "$name" ] || exit 2; done
sha256sum -c PAYLOAD.sha256 >/dev/null
rm {' '.join(files)}
cd /m_data/usr
rmdir fs050w-install
echo cleaned
'''
    if remote_capture(shell, host_ip, cleanup).strip() != "cleaned":
        raise MaintenanceError("暫存資料夾未確認清除")


def install(api: RouterAPI, shell: NtpShell, host_ip: str, *, guided: bool,
            enable_debug_mode: bool) -> None:
    if shell.query("id -u").strip() != "0":
        raise MaintenanceError("NTP 維護通道沒有 root 權限")
    original = api.get_home()
    if digest(original) != ORIGINAL_SHA256:
        raise MaintenanceError("目前首頁不是已驗證的 V2.1.0 原廠檔案；不覆蓋既有介面")
    print("原廠首頁、設備版本與安全狀態已核對。")
    marker, hook = remote_preflight(shell, host_ip)
    if not (marker and hook):
        print("設備尚未啟用 debug mode 或沒有開機掛鉤。此模式可能擴大原廠網頁除錯功能權限。")
        if guided and not enable_debug_mode:
            enable_debug_mode = input("要明確啟用並保留可回復備份嗎？輸入 yes：").strip().lower() == "yes"
        if not enable_debug_mode:
            raise MaintenanceError("需要 --enable-debug-mode 明確同意建立缺少的開機條件")
    pause_if_guided(guided, "準備從這台設備讀取的原廠首頁製作專用安裝包。")
    with tempfile.TemporaryDirectory(prefix="fs050w-sms-prepare-") as td:
        directory = Path(td)
        source = directory / "original-home.html"
        source.write_bytes(original)
        payload = directory / "fs050w-install"
        prepare(source, payload)
        archive = tar_payload(payload)
        archive_hash = digest(archive)
        print(f"安裝包已備妥：{len(archive)} bytes，SHA-256 {archive_hash}")
        pause_if_guided(guided, "接著傳入設備，並由設備本身再核對所有檔案和韌體。")
        with serve_bytes(archive, host_ip, api.host) as url:
            shell.command(f"wget {url} -O /tmp/p", pause=1.0)
        shell.command("sha256sum /tmp/p >/tmp/v")
        if shell.query("cat /tmp/v").split()[0] != archive_hash:
            raise MaintenanceError("設備收到的安裝包雜湊不符，停止安裝")
        installer_started = False
        try:
            shell.command("tar xf /tmp/p -C /m_data/usr", pause=0.4)
            remote_capture(shell, host_ip,
                           "cd /m_data/usr/fs050w-install && sha256sum -c PAYLOAD.sha256 >/dev/null && "
                           "(cd / && sha256sum -c /m_data/usr/fs050w-install/V2.1.0.sha256 >/dev/null) "
                           "&& echo verified")
            print("設備端檔案與韌體函式庫已核對。")
            pause_if_guided(guided, "現在安裝首頁與 SMS 模組。安裝失敗時設備端會用備份回復。")
            suffix = " -d" if enable_debug_mode else ""
            installer_started = True
            install_output = remote_capture(shell, host_ip,
                                            f"cd {REMOTE_DIR} && sh install_on_router.sh{suffix}")
            print(install_output.strip())
        except Exception as error:
            if not installer_started and shell.ntp_healthy:
                try:
                    cleanup_uninstalled_stage(shell, host_ip, payload)
                except Exception as cleanup_error:
                    raise MaintenanceError(
                        f"{error}；安裝尚未開始，設備上可能留有 {REMOTE_DIR}。"
                        f"請先以互動維護模式核對，清理未完成：{cleanup_error}"
                    ) from error
            raise
        expected_home = digest((payload / "home.html").read_bytes())
        verification_error = None
        try:
            actual_home = digest(api.get_home())
            count_code = None
            for _ in range(5):
                count_code = api.post("action/sms_get_sms_count", {}).get("retcode")
                if count_code == 0:
                    break
                time.sleep(1)
            if actual_home != expected_home or count_code != 0:
                verification_error = MaintenanceError("首頁或簡訊收件計數與預期不符")
        except MaintenanceError as exc:
            verification_error = exc
        if verification_error is not None:
            try:
                rollback(api, shell, host_ip)
            except MaintenanceError as exc:
                raise MaintenanceError("安裝後驗證失敗，且自動回復未能確認：" + str(exc)) from exc
            raise MaintenanceError(f"安裝後驗證失敗（{verification_error}），已回復原廠檔案")
        print("安裝成功。原廠首頁已顯示簡訊中心，SMS 收件計數回碼 0。")
        print(f"回復方式：python3 install_via_ntp.py rollback；設備備份位於 /m_data/usr/.fs050w-sms-install")
    try:
        shell.command("rm -f /tmp/p /tmp/v")
    except MaintenanceError:
        print("提醒：安裝已完成，臨時傳輸檔未能清除。")


def rollback(api: RouterAPI, shell: NtpShell, host_ip: str) -> None:
    if shell.query("id -u").strip() != "0":
        raise MaintenanceError("NTP 維護通道沒有 root 權限")
    output = remote_capture(shell, host_ip,
                            "sh /m_data/usr/fs050w-install/rollback_on_router.sh")
    if digest(api.get_home()) != ORIGINAL_SHA256:
        raise MaintenanceError("回復後首頁雜湊仍不等於原廠版本，請人工檢查")
    print(output.strip())
    print("原廠首頁與開機掛鉤已回復；原備份仍留在設備上。")


def main() -> None:
    parser = argparse.ArgumentParser(description="FS050W V2.1.0 簡訊中心安裝工具")
    parser.add_argument("--router", default="192.168.100.1")
    parser.add_argument("--host-ip", help="這台電腦在 FS050W Wi-Fi 上的 IPv4 位址")
    parser.add_argument("--password-env", help="從指定環境變數讀取管理密碼；預設互動輸入")
    sub = parser.add_subparsers(dest="action", required=True)
    quick = sub.add_parser("install", help="一次執行檢查、傳入、安裝與驗證")
    quick.add_argument("--enable-debug-mode", action="store_true",
                       help="明確同意為出廠機建立缺少的 debug 標記與開機掛鉤")
    guided = sub.add_parser("interactive", help="逐步顯示檢查與安裝選擇")
    guided.add_argument("--enable-debug-mode", action="store_true")
    sub.add_parser("rollback", help="使用設備內的備份回復")
    args = parser.parse_args()
    api = RouterAPI(args.router, requested_password(args.password_env), timeout=45)
    api.login()
    try:
        shell = NtpShell(api)
        host_ip = find_host_ip(args.router, args.host_ip)
    except Exception:
        api.logout()
        raise
    original_error = None
    try:
        if args.action == "rollback":
            rollback(api, shell, host_ip)
        else:
            install(api, shell, host_ip, guided=args.action == "interactive",
                    enable_debug_mode=args.enable_debug_mode)
    except Exception as exc:
        original_error = exc
        raise
    finally:
        try:
            shell.close()
        except Exception as close_error:
            if original_error is None:
                raise
            print(f"結束清理未完成：{close_error}", file=sys.stderr)


if __name__ == "__main__":
    try:
        main()
    except (MaintenanceError, OSError, ValueError) as exc:
        raise SystemExit(f"FS050W SMS 安裝：{exc}") from exc
