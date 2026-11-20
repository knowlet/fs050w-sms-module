#!/usr/bin/env python3
"""Owner-authenticated FS050W V2.1.0 NTP maintenance channel.

The firmware's NTP setter executes a short server-name string as a shell
command. Every request restores *all* original NTP fields and reads them back.
This module uses only the Python standard library and never stores a password.
"""

from __future__ import annotations

import base64
import getpass
import hashlib
import hmac
import http.cookiejar
import ipaddress
import json
import re
import secrets
import socket
import subprocess
import sys
import threading
import time
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError, URLError
from urllib.request import HTTPCookieProcessor, HTTPRedirectHandler, ProxyHandler, Request, build_opener


USERNAME = "4cc68e3626e5b94602c325f7c4ca5dee"
FIXED_KEY = b"0123456789"
NTP_KEYS = ("ntp_enable_state", "ntp_daylight_state", *(f"ntp_server_{i}" for i in range(1, 6)))
SAFE_KEYS = ("device_software_version", "dialup_dataswitch", "dialup_dial_status",
             "mnet_sim_status", "lcd_forbidden_button", "device_lock_button",
             "dialup_profile_default")
SERVER_VALUE = re.compile(r"^[A-Za-z0-9_.:-]{0,63}$")
BLOCKED_COMMAND = re.compile(
    r"\b(?:reboot|poweroff|halt|shutdown)\b|\binit\s+6\b|"
    r"dialup_dataswitch\s*=\s*off|lcd_forbidden_button\s*=\s*disable",
    re.I,
)


class MaintenanceError(RuntimeError):
    pass


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        return None


def password_hash(password: str, private_key: str) -> str:
    inner = hmac.new(FIXED_KEY, password.encode(), hashlib.md5).hexdigest()
    mid = hashlib.md5(f"{USERNAME}:example.com:{inner}".encode()).hexdigest()
    return hmac.new(FIXED_KEY, (mid + private_key).encode(), hashlib.md5).hexdigest()


def requested_password(env_name: str | None = None) -> str:
    if env_name:
        import os
        value = os.environ.get(env_name)
        if not value:
            raise MaintenanceError(f"環境變數 {env_name} 沒有管理密碼")
        return value
    value = getpass.getpass("FS050W 管理密碼：")
    if not value:
        raise MaintenanceError("未輸入管理密碼")
    return value


class RouterAPI:
    """Small no-dependency API client with cookies, CSRF and no POST replay."""

    def __init__(self, host: str, password: str, timeout: int = 20):
        address = ipaddress.ip_address(host)
        if address.version != 4 or not address.is_private or address.is_loopback:
            raise MaintenanceError("只接受區域網路的 IPv4 設備位址")
        self.host = host
        self.password = password
        self.timeout = timeout
        self.base = f"http://{host}"
        self.cookies = http.cookiejar.CookieJar()
        self.opener = build_opener(ProxyHandler({}), HTTPCookieProcessor(self.cookies), _NoRedirect())
        self.logged_in = False

    def _request(self, path: str, *, data: dict | None = None, csrf: str = "") -> tuple[int, object, bytes]:
        body = None if data is None else json.dumps(data, separators=(",", ":")).encode()
        headers = {"X-Requested-With": "XMLHttpRequest"}
        if body is not None:
            headers["Content-Type"] = "application/json"
            headers["X-Csrf-Token"] = csrf
        req = Request(self.base + "/" + path.lstrip("/"), body, headers=headers,
                      method="POST" if body is not None else "GET")
        try:
            with self.opener.open(req, timeout=self.timeout) as response:
                return response.status, response.headers, response.read()
        except HTTPError as exc:
            raise MaintenanceError(f"設備 HTTP {exc.code}：{path}") from exc
        except URLError as exc:
            raise MaintenanceError(f"無法連上設備：{exc.reason}") from exc

    def csrf(self) -> str:
        status, headers, _ = self._request("goform/x_csrf_token")
        token = headers.get("X-Csrf-Token", "")
        if status != 200 or not token:
            raise MaintenanceError("設備沒有提供 CSRF token")
        return token

    def post(self, path: str, data: dict) -> dict:
        status, _, body = self._request(path, data=data, csrf=self.csrf())
        if status != 200:
            raise MaintenanceError(f"設備 HTTP {status}：{path}")
        try:
            result = json.loads(body)
        except (ValueError, UnicodeDecodeError) as exc:
            raise MaintenanceError(f"設備回應不是 JSON：{path}") from exc
        if not isinstance(result, dict):
            raise MaintenanceError(f"設備回應格式不符：{path}")
        return result

    def login(self) -> None:
        key = self.post("goform/get_private_key", {})
        if key.get("retcode") != 0 or not isinstance(key.get("prikey"), str):
            raise MaintenanceError("取得登入金鑰失敗")
        result = self.post("goform/login2", {
            "username": USERNAME,
            "password": password_hash(self.password, key["prikey"]),
            "prikey": key["prikey"],
        })
        if result.get("retcode") != 0:
            raise MaintenanceError(f"管理頁登入失敗（回碼 {result.get('retcode')}）")
        self.logged_in = True

    def get_params(self, keys: list[str] | tuple[str, ...]) -> dict[str, str]:
        result = self.post("action/get_mgdb_params", {"keys": list(keys)})
        if result.get("retcode") != 0 or not isinstance(result.get("data"), dict):
            raise MaintenanceError("讀取設備狀態失敗")
        return result["data"]

    def get_param(self, key: str) -> str:
        return self.get_params([key]).get(key, "")

    def get_home(self) -> bytes:
        status, _, body = self._request("common/home.html")
        if status != 200:
            raise MaintenanceError("無法讀取原廠首頁")
        return body

    def logout(self) -> None:
        if self.logged_in:
            try:
                self.post("action/logout", {})
            except MaintenanceError:
                pass
            self.logged_in = False


class NtpShell:
    """Sends short shell commands and guarantees NTP rollback on each call."""

    def __init__(self, api: RouterAPI):
        self.api = api
        state = api.get_params([*SAFE_KEYS, *NTP_KEYS])
        required = {
            "device_software_version": "V2.1.0", "dialup_dataswitch": "on",
            "mnet_sim_status": "ready", "lcd_forbidden_button": "enable",
            "device_lock_button": "disable",
        }
        for key, wanted in required.items():
            if state.get(key) != wanted:
                raise MaintenanceError(f"設備預檢未通過：{key}")
        if state.get("dialup_dial_status") not in ("connected", "disconnected"):
            raise MaintenanceError("行動數據狀態無法辨識")
        if not re.fullmatch(r"[0-9]+", state.get("dialup_profile_default", "")):
            raise MaintenanceError("無法辨識 APN profile")
        self.profile = state["dialup_profile_default"]
        self.base = {key: state.get(key, "") for key in NTP_KEYS}
        for key in NTP_KEYS[2:]:
            if not SERVER_VALUE.fullmatch(self.base[key]):
                raise MaintenanceError(f"{key} 原值含特殊字元，請先用原廠介面修復")
        self.probe_was = api.get_param("ntp_probe_ok")
        if self.probe_was:
            raise MaintenanceError("先前的 NTP 探測標記仍存在，請先人工核對設備狀態")
        self.probe_used = False
        self.ntp_healthy = True

    @staticmethod
    def _wrapped(fragment: str) -> str:
        try:
            raw = f"x;{fragment};#".encode("ascii")
        except UnicodeEncodeError as exc:
            raise MaintenanceError("NTP 短命令只接受 ASCII；可用互動腳本執行中文命令") from exc
        if len(raw) > 63 or b"\n" in raw or b"\r" in raw:
            raise MaintenanceError(f"NTP 命令長度 {len(raw)} 超過 63 bytes 或含換行")
        return raw.decode()

    def _set_server(self, value: str) -> None:
        response = self.api.post("action/ntp_set_ntp_params", {**self.base, "ntp_server_1": value})
        if response.get("retcode") != 0:
            raise MaintenanceError(f"NTP 設定失敗（回碼 {response.get('retcode')}）")

    def restore(self) -> None:
        try:
            response = self.api.post("action/ntp_set_ntp_params", self.base)
            if response.get("retcode") != 0:
                raise MaintenanceError("NTP 還原 API 失敗，請立即到原廠管理頁檢查時間伺服器")
            actual = self.api.get_params(NTP_KEYS)
            if any(actual.get(key, "") != self.base[key] for key in NTP_KEYS):
                raise MaintenanceError("NTP 讀回與原值不同，請立即到原廠管理頁檢查")
        except Exception:
            self.ntp_healthy = False
            raise
        self.ntp_healthy = True

    def command(self, fragment: str, pause: float = 0.2) -> None:
        if not self.ntp_healthy:
            raise MaintenanceError("NTP 還原未確認，停止傳送新的維護命令")
        wrapped = self._wrapped(fragment)
        try:
            self._set_server(wrapped)
            time.sleep(pause)
        finally:
            self.restore()

    def query(self, expression: str, timeout: float = 5.0) -> str:
        if not self.ntp_healthy:
            raise MaintenanceError("NTP 還原未確認，停止傳送新的維護命令")
        tag = "Q" + secrets.token_hex(3)
        wrapped = self._wrapped(f"mgdb_tool set ntp_probe_ok={tag}$({expression})")
        self.probe_used = True
        previous = self.api.get_param("ntp_probe_ok")
        try:
            self._set_server(wrapped)
            until = time.monotonic() + timeout
            while time.monotonic() < until:
                value = self.api.get_param("ntp_probe_ok")
                if value.startswith(tag) and value != previous:
                    return value[len(tag):]
                time.sleep(0.2)
            raise MaintenanceError("NTP 查詢沒有回應")
        finally:
            self.restore()

    def close(self) -> None:
        # The caller must never call this after a reboot command. This CLI
        # deliberately has no reboot action; writes are always restored first.
        try:
            if self.probe_used and self.ntp_healthy:
                self.command("mgdb_tool del ntp_probe_ok")
                if self.api.get_param("ntp_probe_ok"):
                    raise MaintenanceError("探測標記未清除")
        finally:
            self.api.logout()

    def run_capture(self, command: str, host_ip: str, *, max_output: int = 2048,
                    execution_timeout: float = 120.0) -> tuple[int, str, bool]:
        if BLOCKED_COMMAND.search(command):
            raise MaintenanceError("互動工具拒絕重啟、關閉數據或禁用實體按鍵的命令")
        if max_output < 1 or max_output > 8192:
            raise MaintenanceError("輸出上限須介於 1 和 8192 bytes")
        if execution_timeout <= 0 or execution_timeout > 600:
            raise MaintenanceError("執行等待上限須介於 0 和 600 秒")
        script = ("#!/bin/sh\n" + command + "\n").encode()
        original_error = None
        owns_files = False
        running = False
        try:
            # A previous timed-out command may still be writing these files.
            # Leave it for inspection instead of overwriting its result.
            if (self.query("test -e /tmp/c&&echo 1").strip() or
                    self.query("test -e /tmp/e&&echo 1").strip()):
                raise MaintenanceError("先前維護命令的暫存檔仍在；請先檢查設備再執行新命令")
            owns_files = True
            with serve_bytes(script, host_ip, self.api.host) as url:
                self.command(f"wget {url} -O /tmp/c", pause=0.8)
            expected = hashlib.sha256(script).hexdigest()
            self.command("sha256sum /tmp/c >/tmp/v")
            digest_parts = self.query("cat /tmp/v").split()
            if not digest_parts or digest_parts[0] != expected:
                raise MaintenanceError("互動命令下載後雜湊不符")
            running = True
            self.command("sh /tmp/c >/tmp/o 2>&1; echo $? >/tmp/e", pause=0.5)
            deadline = time.monotonic() + execution_timeout
            while time.monotonic() < deadline:
                marker = self.query("cat /tmp/e").strip()
                if not marker:
                    time.sleep(0.4)
                    continue
                if not marker.isascii() or not marker.isdecimal() or int(marker) > 255:
                    raise MaintenanceError("維護命令的完成標記無法辨識；暫存輸出已保留供檢查")
                code = int(marker)
                running = False
                break
            else:
                raise MaintenanceError("維護命令尚未回報完成；暫存輸出已保留，請先檢查設備")
            size = int(self.query("wc -c </tmp/o").strip())
            self.command(f"head -c {max_output} /tmp/o|base64|fold -w 48 >/tmp/b")
            count = int(self.query("wc -l </tmp/b").strip())
            if count > 240:
                raise MaintenanceError("輸出分段異常")
            lines = [self.query(f"sed -n {i}p /tmp/b").strip() for i in range(1, count + 1)]
            output = base64.b64decode("".join(lines), validate=True).decode(errors="replace")
            return code, output, size > max_output
        except Exception as exc:
            original_error = exc
            raise
        finally:
            if self.ntp_healthy and owns_files and not running:
                try:
                    self.command("rm -f /tmp/c /tmp/o /tmp/e /tmp/b /tmp/v")
                except Exception as cleanup_error:
                    if original_error is None:
                        raise
                    print(f"暫存清理未完成：{cleanup_error}", file=sys.stderr)


def find_host_ip(router_ip: str, override: str | None = None) -> str:
    network = ipaddress.IPv4Network(f"{router_ip}/24", strict=False)
    if override:
        address = ipaddress.IPv4Address(override)
        if address not in network or str(address) == router_ip:
            raise MaintenanceError("--host-ip 必須是 FS050W 同網段的電腦位址")
        return override
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.connect((router_ip, 80))
            value = sock.getsockname()[0]
            if value and ipaddress.IPv4Address(value) in network and value != router_ip:
                return value
        finally:
            sock.close()
    except OSError:
        pass
    for tool in ("ifconfig", "ip"):
        try:
            output = subprocess.check_output([tool, "-4", "addr"] if tool == "ip" else [tool],
                                             text=True, stderr=subprocess.DEVNULL)
        except (OSError, subprocess.CalledProcessError):
            continue
        addresses = re.findall(r"\binet\s+(\d+\.\d+\.\d+\.\d+)", output)
        prefix = ".".join(router_ip.split(".")[:3]) + "."
        candidates = [address for address in addresses if address.startswith(prefix) and address != router_ip]
        if len(candidates) == 1:
            return candidates[0]
    raise MaintenanceError("無法判斷電腦在路由器 Wi-Fi 上的位址；請指定 --host-ip")


@contextmanager
def serve_bytes(blob: bytes, host_ip: str, router_ip: str):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.client_address[0] != router_ip or self.path != "/p":
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(blob)))
            self.end_headers()
            self.wfile.write(blob)

        def log_message(self, fmt, *args):
            pass

    server = ThreadingHTTPServer((host_ip, 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://{host_ip}:{server.server_port}/p"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def main() -> None:
    import argparse
    parser = argparse.ArgumentParser(description="FS050W V2.1.0 NTP 維護終端")
    parser.add_argument("--router", default="192.168.100.1")
    parser.add_argument("--host-ip", help="本機在 FS050W Wi-Fi 上的 IPv4 位址")
    parser.add_argument("--password-env", help="從指定環境變數讀取管理密碼；預設互動輸入")
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("interactive", help="逐行執行維護命令，輸入 exit 結束")
    command = sub.add_parser("exec", help="執行一條維護命令並顯示輸出")
    command.add_argument("command")
    args = parser.parse_args()
    api = RouterAPI(args.router, requested_password(args.password_env))
    api.login()
    try:
        shell = NtpShell(api)
        host_ip = find_host_ip(args.router, args.host_ip)
    except Exception:
        api.logout()
        raise
    print(f"已連上 FS050W V2.1.0；APN profile {shell.profile}；NTP 設定會在每次命令後讀回還原。")
    original_error = None
    try:
        if args.action == "exec":
            code, output, truncated = shell.run_capture(args.command, host_ip)
            print(output, end="" if output.endswith("\n") else "\n")
            print(f"退出碼 {code}" + ("；輸出已截斷" if truncated else ""))
        else:
            while True:
                try:
                    line = input("fs050w> ")
                except EOFError:
                    break
                if line.strip() in ("exit", "quit"):
                    break
                if not line.strip():
                    continue
                try:
                    code, output, truncated = shell.run_capture(line, host_ip)
                    print(output, end="" if output.endswith("\n") else "\n")
                    print(f"退出碼 {code}" + ("；輸出已截斷" if truncated else ""))
                except MaintenanceError as exc:
                    print(f"錯誤：{exc}")
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
        raise SystemExit(f"FS050W 維護終端：{exc}") from exc
