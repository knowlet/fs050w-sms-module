#!/bin/sh
# Build and package locally. Specifying an extracted system directory allows a
# rebuild from the source subset in repair-kit.zip without the research tree.
set -eu
umask 077
here=$(CDPATH='' cd -P "$(dirname "$0")" && pwd)
if [ -d "$here/src/sms-module" ]; then
  src=$here/src/sms-module
  meta=$here
else
  src=$here/../sms-module
  meta=$here/maintenance
fi
if [ "${1:-}" = --help ]; then
  echo 'Usage: sh prepare-repair.sh [extracted-V2.1.0-system [output.zip]]'
  exit 0
fi
[ "$#" -le 2 ] || { echo 'Use --help for usage.' >&2; exit 2; }
fw=${1:-$here/../extractions/FS050W_V2.1.0.bin.extracted/0/system}
ROOT=$(CDPATH='' cd -P "$fw" && pwd) || { echo 'Supply your extracted FS050W V2.1.0 system directory.' >&2; exit 1; }
archive=${2:-$here/repair-kit.zip}
case "$archive" in /*) ;; *) archive=$PWD/$archive ;; esac
[ ! -d "$archive" ] || { echo 'Output must be a ZIP file path.' >&2; exit 1; }
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
trap 'exit 1' INT TERM HUP
kit=$stage/repair-kit
mkdir -p "$kit/bin" "$kit/src/sms-module/tests" "$kit/src/sms-module/nas-domain-trial"
if [ -z "${CLANG:-}" ]; then
  if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then CLANG=/opt/homebrew/opt/llvm/bin/clang; else CLANG=$(command -v clang); fi
fi
if [ -z "${LLD:-}" ]; then LLD=$(command -v ld.lld); fi
export CLANG LLD ROOT
unset SMS_TEST_PEER
cp "$meta/apply.sh" "$meta/V2.1.0.sha256" "$here/prepare-repair.sh" "$here/REPAIR.md" "$kit/"
for f in build.sh build-nas-attach-probe.sh start.S sms.c sms_submit.h sms_nas_attach_probe.c; do
  cp "$src/$f" "$kit/src/sms-module/"
done
for f in test_portable_submit.c test_inbound_visibility.c; do cp "$src/tests/$f" "$kit/src/sms-module/tests/"; done
for f in build.sh build_permanent.sh nas_domain_set.c nas_domain_permanent_set.c test_host.c test_permanent_host.c test_host.sh test_permanent_host.sh verify_permanent_wire.py verify_wire.py; do
  cp "$src/nas-domain-trial/$f" "$kit/src/sms-module/nas-domain-trial/"
done
src=$kit/src/sms-module
python3 - "$ROOT" "$kit/V2.1.0.sha256" <<'PY'
import hashlib
import sys
from pathlib import Path
for entry in Path(sys.argv[2]).read_text().splitlines():
    expected, name = entry.split()
    path = Path(sys.argv[1]) / name
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        sys.exit(f"Unsupported or missing FS050W V2.1.0 file: {name}")
PY
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DSMS_TW_MOBILE_PROFILE=1 "$src/tests/test_portable_submit.c" -o "$stage/test-submit"
"$stage/test-submit"
"${CC:-cc}" -std=c11 -fno-builtin -DSMS_TW_MOBILE_PROFILE=1 "$src/tests/test_inbound_visibility.c" -o "$stage/test-inbound"
"$stage/test-inbound"
sh "$src/nas-domain-trial/test_host.sh"
sh "$src/nas-domain-trial/test_permanent_host.sh"
SMS_TW_MOBILE_PROFILE=1 SMS_OUTPUT="$kit/bin/fs050w-sms" sh "$src/build.sh"
OUTPUT="$kit/bin/fs050w-nas-status" sh "$src/build-nas-attach-probe.sh"
OUT_DIR="$kit/bin" sh "$src/nas-domain-trial/build.sh"
OUT_DIR="$kit/bin" sh "$src/nas-domain-trial/build_permanent.sh"
python3 - "$kit" "$archive" <<'PY'
import hashlib
import os
from pathlib import Path
import sys
import tempfile
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED

kit, archive = map(Path, sys.argv[1:])
bins = sorted((kit / "bin").iterdir())
if len(bins) != 6:
    sys.exit("Missing an ARM binary")
for executable in bins:
    header = executable.read_bytes()[:20]
    if header[:6] != b"\x7fELF\x01\x01" or int.from_bytes(header[18:20], "little") != 40:
        sys.exit(f"Unexpected binary format: {executable.name}")
def manifest(files):
    return "".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(kit)}\n" for p in files)
(kit / "BIN.sha256").write_text(manifest(bins + [kit / "V2.1.0.sha256"]))
(kit / "SHA256SUMS").write_text(manifest(sorted(p for p in kit.rglob("*") if p.is_file())))
# Atomic replacement also leaves a previous kit untouched when rebuilding fails.
fd, tmp = tempfile.mkstemp(prefix=".fs050w-kit-", dir=archive.parent)
os.close(fd)
try:
    with ZipFile(tmp, "w", compression=ZIP_DEFLATED) as z:
        for p in sorted(q for q in kit.rglob("*") if q.is_file()):
            name = p.relative_to(kit).as_posix()
            info = ZipInfo("repair-kit/" + name)
            info.create_system = 3
            info.external_attr = ((0o100755 if p.suffix == ".sh" or p in bins else 0o100644) << 16)
            z.writestr(info, p.read_bytes(), compress_type=ZIP_DEFLATED)
    os.replace(tmp, archive)
finally:
    if os.path.exists(tmp):
        os.unlink(tmp)
print(f"{archive}\nSHA-256 {hashlib.sha256(archive.read_bytes()).hexdigest()}")
PY
