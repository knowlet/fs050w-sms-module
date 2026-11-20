"""Offline checks for the host NTP channel and prepared archive."""

import io
import hashlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from install_via_ntp import cleanup_uninstalled_stage, tar_payload  # noqa: E402
from ntp_shell import MaintenanceError, NTP_KEYS, NtpShell, find_host_ip  # noqa: E402


class FakeAPI:
    host = "192.168.100.1"

    def __init__(self):
        self.state = {
            "device_software_version": "V2.1.0", "dialup_dataswitch": "on",
            "dialup_dial_status": "connected", "mnet_sim_status": "ready",
            "lcd_forbidden_button": "enable", "device_lock_button": "disable",
            "dialup_profile_default": "18", "ntp_enable_state": "on",
            "ntp_daylight_state": "off", "ntp_server_1": "ntp.nict.jp",
            "ntp_server_2": "", "ntp_server_3": "", "ntp_server_4": "",
            "ntp_server_5": "", "ntp_probe_ok": "",
        }
        self.fail_injection = False
        self.writes = []

    def get_params(self, keys):
        return {key: self.state.get(key, "") for key in keys}

    def get_param(self, key):
        return self.state.get(key, "")

    def post(self, path, data):
        assert path == "action/ntp_set_ntp_params"
        self.writes.append(data.copy())
        value = data["ntp_server_1"]
        if value.startswith("x;") and self.fail_injection:
            return {"retcode": 103}
        self.state.update(data)
        if "mgdb_tool del ntp_probe_ok" in value:
            self.state["ntp_probe_ok"] = ""
        if "mgdb_tool set ntp_probe_ok=" in value:
            prefix = value.split("mgdb_tool set ntp_probe_ok=", 1)[1].split("$(", 1)[0]
            self.state["ntp_probe_ok"] = prefix + "0"
        return {"retcode": 0}

    def logout(self):
        pass


class NtpHostTests(unittest.TestCase):
    def test_command_restores_every_original_ntp_field(self):
        api = FakeAPI()
        shell = NtpShell(api)
        shell.command("id -u", pause=0)
        self.assertEqual(api.get_params(NTP_KEYS), shell.base)
        self.assertEqual(len(api.writes), 2)

    def test_failed_injection_still_restores(self):
        api = FakeAPI()
        shell = NtpShell(api)
        api.fail_injection = True
        with self.assertRaises(MaintenanceError):
            shell.command("id -u", pause=0)
        self.assertEqual(api.get_params(NTP_KEYS), shell.base)

    def test_query_and_cleanup(self):
        api = FakeAPI()
        shell = NtpShell(api)
        self.assertEqual(shell.query("id -u", timeout=0.01), "0")
        shell.close()
        self.assertEqual(api.get_param("ntp_probe_ok"), "")
        self.assertEqual(api.get_params(NTP_KEYS), shell.base)

    def test_rejects_overlong_short_command_and_unsafe_ntp_original(self):
        with self.assertRaises(MaintenanceError):
            NtpShell._wrapped("x" * 64)
        api = FakeAPI()
        api.state["ntp_server_1"] = "x;reboot;#"
        with self.assertRaises(MaintenanceError):
            NtpShell(api)

    def test_host_ip_must_be_on_router_wifi(self):
        self.assertEqual(find_host_ip("192.168.100.1", "192.168.100.23"), "192.168.100.23")
        with self.assertRaises(MaintenanceError):
            find_host_ip("192.168.100.1", "192.168.0.23")

    def test_tar_has_only_expected_regular_files(self):
        with tempfile.TemporaryDirectory() as td:
            directory = Path(td)
            (directory / "home.html").write_bytes(b"home")
            (directory / "sm3").write_bytes(b"module")
            tar = tar_payload(directory)
            with tarfile.open(fileobj=io.BytesIO(tar)) as archive:
                names = archive.getnames()
                self.assertEqual(names, ["fs050w-install", "fs050w-install/home.html", "fs050w-install/sm3"])
                self.assertEqual(archive.extractfile("fs050w-install/home.html").read(), b"home")
                self.assertEqual(archive.extractfile("fs050w-install/sm3").read(), b"module")

    def test_capture_stops_after_unconfirmed_ntp_restore(self):
        fake = SimpleNamespace(api=SimpleNamespace(host="192.168.100.1"), ntp_healthy=True)
        calls = []

        def fail(fragment, pause=0):
            calls.append(fragment)
            fake.ntp_healthy = False
            raise MaintenanceError("NTP 還原未確認")

        fake.command = fail
        fake.query = lambda expression: ""

        @contextmanager
        def serve(*_):
            yield "http://192.168.100.23:8888/p"

        with patch("ntp_shell.serve_bytes", serve):
            with self.assertRaisesRegex(MaintenanceError, "NTP 還原未確認"):
                NtpShell.run_capture(fake, "echo ok", "192.168.100.23")
        self.assertEqual(len(calls), 1, "清理不可在 NTP 還原失敗後再注入")

    def test_capture_preserves_original_error_if_cleanup_fails(self):
        fake = SimpleNamespace(api=SimpleNamespace(host="192.168.100.1"), ntp_healthy=True)
        calls = []

        def command(fragment, pause=0):
            calls.append(fragment)
            if fragment.startswith("rm -f"):
                raise MaintenanceError("cleanup failed")

        fake.command = command
        fake.query = lambda expression: "" if expression.startswith("test -e") else "wrong-hash"

        @contextmanager
        def serve(*_):
            yield "http://192.168.100.23:8888/p"

        with patch("ntp_shell.serve_bytes", serve):
            with patch("sys.stderr", new=io.StringIO()):
                with self.assertRaisesRegex(MaintenanceError, "雜湊不符"):
                    NtpShell.run_capture(fake, "echo ok", "192.168.100.23")
        self.assertEqual(len(calls), 3)

    def test_capture_waits_for_remote_completion_before_reading_output(self):
        fake = SimpleNamespace(api=SimpleNamespace(host="192.168.100.1"), ntp_healthy=True)
        calls = []
        markers = iter(("", "", "0"))
        script = b"#!/bin/sh\necho ok\n"

        def command(fragment, pause=0):
            calls.append(fragment)

        def query(expression):
            calls.append(expression)
            if expression.startswith("test -e"):
                return ""
            if expression == "cat /tmp/v":
                return hashlib.sha256(script).hexdigest() + "  /tmp/c"
            if expression == "cat /tmp/e":
                return next(markers)
            if expression == "wc -c </tmp/o":
                self.assertEqual(calls.count("cat /tmp/e"), 3)
                return "3"
            if expression == "wc -l </tmp/b":
                return "1"
            if expression == "sed -n 1p /tmp/b":
                return "b2sK"
            raise AssertionError(expression)

        fake.command = command
        fake.query = query

        @contextmanager
        def serve(*_):
            yield "http://192.168.100.23:8888/p"

        with patch("ntp_shell.serve_bytes", serve), patch("ntp_shell.time.sleep"):
            self.assertEqual(NtpShell.run_capture(fake, "echo ok", "192.168.100.23"),
                             (0, "ok\n", False))
        self.assertTrue(any(call.startswith("rm -f /tmp/c") for call in calls))

    def test_capture_timeout_leaves_remote_files_for_inspection(self):
        fake = SimpleNamespace(api=SimpleNamespace(host="192.168.100.1"), ntp_healthy=True)
        calls = []
        script = b"#!/bin/sh\necho ok\n"

        def command(fragment, pause=0):
            calls.append(fragment)

        def query(expression):
            calls.append(expression)
            if expression.startswith("test -e"):
                return ""
            if expression == "cat /tmp/v":
                return hashlib.sha256(script).hexdigest() + "  /tmp/c"
            if expression == "cat /tmp/e":
                return ""
            raise AssertionError(expression)

        clock = iter((0.0, 0.1, 0.6))
        fake.command = command
        fake.query = query

        @contextmanager
        def serve(*_):
            yield "http://192.168.100.23:8888/p"

        with patch("ntp_shell.serve_bytes", serve), patch("ntp_shell.time.sleep"), \
             patch("ntp_shell.time.monotonic", side_effect=lambda: next(clock)):
            with self.assertRaisesRegex(MaintenanceError, "暫存輸出已保留"):
                NtpShell.run_capture(fake, "echo ok", "192.168.100.23",
                                     execution_timeout=0.5)
        self.assertFalse(any(call.startswith("rm -f /tmp/c") for call in calls))

    def test_capture_does_not_remove_previous_unknown_command(self):
        fake = SimpleNamespace(api=SimpleNamespace(host="192.168.100.1"), ntp_healthy=True)
        calls = []
        fake.query = lambda expression: "1" if expression.startswith("test -e") else ""
        fake.command = lambda fragment, pause=0: calls.append(fragment)
        with self.assertRaisesRegex(MaintenanceError, "先前維護命令"):
            NtpShell.run_capture(fake, "echo ok", "192.168.100.23")
        self.assertEqual(calls, [])

    def test_capture_refuses_stale_exit_marker(self):
        fake = SimpleNamespace(api=SimpleNamespace(host="192.168.100.1"), ntp_healthy=True)
        calls = []
        fake.query = lambda expression: "1" if expression == "test -e /tmp/e&&echo 1" else ""
        fake.command = lambda fragment, pause=0: calls.append(fragment)
        with self.assertRaisesRegex(MaintenanceError, "先前維護命令"):
            NtpShell.run_capture(fake, "echo ok", "192.168.100.23")
        self.assertEqual(calls, [])

    def test_uninstalled_stage_cleanup_checks_ownership(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "source"
            remote_usr = root / "m_data/usr"
            remote = remote_usr / "fs050w-install"
            source.mkdir()
            remote.mkdir(parents=True)
            files = {"home.html": b"patched", "sm3": b"module"}
            manifest = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in files.items())
            files["PAYLOAD.sha256"] = manifest.encode()
            for name, data in files.items():
                (source / name).write_bytes(data)
                (remote / name).write_bytes(data)

            def local_capture(_shell, _host_ip, script):
                local = script.replace("/m_data/usr", str(remote_usr))
                result = subprocess.run(["sh", "-c", local], capture_output=True, text=True)
                if result.returncode:
                    raise MaintenanceError(f"cleanup exit {result.returncode}")
                return result.stdout

            (remote_usr / ".fs050w-sms-install").mkdir()
            with patch("install_via_ntp.remote_capture", local_capture):
                with self.assertRaises(MaintenanceError):
                    cleanup_uninstalled_stage(None, "127.0.0.1", source)
                self.assertTrue(remote.exists())
                shutil.rmtree(remote_usr / ".fs050w-sms-install")
                cleanup_uninstalled_stage(None, "127.0.0.1", source)
                self.assertFalse(remote.exists())


if __name__ == "__main__":
    unittest.main()
