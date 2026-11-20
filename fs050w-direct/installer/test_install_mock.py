"""Exercise installer rollback paths against a disposable, local file tree.

The script substitutes router paths and skips hardware preflight. It cannot
establish behavior on a real modem, but catches filesystem rollback mistakes.
"""

import hashlib
import subprocess
import tempfile
from pathlib import Path


SOURCE = Path(__file__).resolve().parent


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run(hook_existing=True, marker_existing=True):
    needs_bootstrap = not (hook_existing and marker_existing)
    scenario = f"hook={'existing' if hook_existing else 'new'}, marker={'existing' if marker_existing else 'new'}"
    with tempfile.TemporaryDirectory(prefix="fs050w-install-mock-") as temporary:
        root = Path(temporary).resolve()
        userdir = root / "m_data/usr"
        webdir = root / "m_webui/webui/common"
        procdir = root / "proc"
        payload = root / "payload"
        for directory in (userdir, webdir, procdir / "net", root / "stub", payload):
            directory.mkdir(parents=True, exist_ok=True)
        (procdir / "mounts").write_text("")
        (procdir / "net/unix").write_text("")
        original = b"original OEM home\n"
        patched = b"patched SMS home\n"
        binary = b"mock binary\n"
        (webdir / "home.html").write_bytes(original)
        if hook_existing:
            (userdir / "debug_mode.sh").write_text("#!/bin/sh\nexit 0\n")
            (userdir / "debug_mode.sh").chmod(0o755)
        if marker_existing:
            (userdir / "device_debug_mode").touch()
        (payload / "home.html").write_bytes(patched)
        (payload / "sm3").write_bytes(binary)
        mount_stub = root / "stub/mount"
        mount_stub.write_text("#!/bin/sh\nexit 1\n")
        mount_stub.chmod(0o755)
        umount_stub = root / "stub/umount"
        umount_stub.write_text(
            f'#!/bin/sh\ncp "{userdir}/.fs050w-sms-install/original-home.html" '
            f'"{webdir}/home.html"\n: > "{procdir}/mounts"\n'
        )
        umount_stub.chmod(0o755)
        replacements = {
            "/m_data/*|/usrdata/*": f"{root}/*",
            "/m_data/usr": str(userdir),
            "/m_webui/webui/common": str(webdir),
            "/tmp/m_tmp/ipc": str(root / "tmp/m_tmp/ipc"),
            "/proc/mounts": str(procdir / "mounts"),
            "/proc/net/unix": str(procdir / "net/unix"),
            "825029c449238980927c213f86e62dddf3be6d0a4e3802a38e54b034cb7d4815": digest(original),
            "5c748f1a9e53c4e8868ff2f2784d8a45c54298023bedac89dcb7d0b9bbc94f3b": digest(binary),
            "PATH=/usr/sbin:/usr/bin:/sbin:/bin": f"PATH={root}/stub:/usr/sbin:/usr/bin:/sbin:/bin",
        }

        def mock_script(name):
            content = (SOURCE / name).read_text()
            for before, after in replacements.items():
                content = content.replace(before, after)
            if name == "install_on_router.sh":
                begin = content.index('[ "$(id -u)" = 0 ]')
                end = content.index('marker=', begin)
                content = content[:begin] + "profile=18\n" + content[end:]
                content = content.replace(
                    'chmod "$(stat -c \'%a\' "$state/original-debug-mode.sh")" "$hook_stage"',
                    'chmod 0755 "$hook_stage"',
                )
            else:
                content = content.replace(
                    '[ "$(id -u)" = 0 ] || die \'需要既有的 root 維護 shell\'',
                    "",
                )
            result = payload / name
            result.write_text(content)
            result.chmod(0o755)

        mock_script("install_on_router.sh")
        mock_script("rollback_on_router.sh")
        if needs_bootstrap:
            refused = subprocess.run(
                ["sh", str(payload / "install_on_router.sh"), str(payload)],
                capture_output=True,
                text=True,
            )
            assert refused.returncode != 0 and "-d" in refused.stderr
            assert not (userdir / ".fs050w-sms-install").exists()
            assert (userdir / "device_debug_mode").exists() == marker_existing
            assert (userdir / "debug_mode.sh").exists() == hook_existing
            print("missing debug bootstrap -> explicit opt-in required: PASS")
        invocation = ["sh", str(payload / "install_on_router.sh")]
        if needs_bootstrap:
            invocation.append("-d")
        invocation.append(str(payload))
        failed = subprocess.run(
            invocation,
            capture_output=True,
            text=True,
        )
        state = userdir / ".fs050w-sms-install"
        assert failed.returncode != 0, failed.stdout + failed.stderr
        assert (state / "rolled-back").exists(), failed.stdout + failed.stderr
        assert (webdir / "home.html").read_bytes() == original
        if hook_existing:
            assert (userdir / "debug_mode.sh").read_text() == "#!/bin/sh\nexit 0\n"
        else:
            assert not (userdir / "debug_mode.sh").exists()
            assert (state / "hook-created").exists()
        if marker_existing:
            assert (userdir / "device_debug_mode").exists()
        else:
            assert not (userdir / "device_debug_mode").exists()
            assert (state / "marker-created").exists()
        assert not (userdir / "sms-home.html").exists()
        assert not (userdir / "sm3").exists()
        print(f"injected mount failure -> automatic rollback ({scenario}): PASS")

        # A second install must not consume or roll back an existing state dir.
        second = subprocess.run(
            invocation,
            capture_output=True,
            text=True,
        )
        assert second.returncode != 0
        assert (state / "rolled-back").exists(), second.stdout + second.stderr
        print("existing state -> refused without changes: PASS")

        # Simulate a later explicit rollback after successful file activation.
        (state / "rolled-back").unlink()
        managed_hook = b"#!/bin/sh\n# managed mock hook\n"
        (userdir / "debug_mode.sh").write_bytes(managed_hook)
        (state / "hook.sha256").write_text(digest(managed_hook) + "\n")
        if not marker_existing:
            (userdir / "device_debug_mode").touch()
        (webdir / "home.html").write_bytes(patched)
        (userdir / "sms-home.html").write_bytes(patched)
        (userdir / "sm3").write_bytes(binary)
        (procdir / "mounts").write_text(
            f"/dev/mock {webdir}/home.html ext4 rw 0 0\n"
        )
        recovered = subprocess.run(
            ["sh", str(payload / "rollback_on_router.sh")],
            capture_output=True,
            text=True,
        )
        assert recovered.returncode == 0, recovered.stdout + recovered.stderr
        assert (webdir / "home.html").read_bytes() == original
        if hook_existing:
            assert (userdir / "debug_mode.sh").read_text() == "#!/bin/sh\nexit 0\n"
        else:
            assert not (userdir / "debug_mode.sh").exists()
        if marker_existing:
            assert (userdir / "device_debug_mode").exists()
        else:
            assert not (userdir / "device_debug_mode").exists()
        assert not (userdir / "sms-home.html").exists()
        assert not (userdir / "sm3").exists()
        assert (state / "rolled-back").exists()
        print(f"simulated installed state -> explicit rollback ({scenario}): PASS")


if __name__ == "__main__":
    run()
    run(hook_existing=False, marker_existing=False)
    run(hook_existing=False, marker_existing=True)
    run(hook_existing=True, marker_existing=False)
