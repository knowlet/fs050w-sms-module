#!/bin/sh
set -eu
SCRIPT_DIR="$(CDPATH='' cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
ROOT="${ROOT:-$(cd "$SCRIPT_DIR/../extractions/FS050W_V2.1.0.bin.extracted/0/system" && pwd)}"
CLANG="${CLANG:-/opt/homebrew/opt/llvm/bin/clang}"
LLD="${LLD:-/opt/homebrew/bin/ld.lld}"
OUTPUT="${SMS_OUTPUT-sms}"
[ -n "$OUTPUT" ] || { echo "SMS_OUTPUT must not be empty" >&2; exit 2; }

set --
if [ "${SMS_TW_MOBILE_PROFILE:-0}" = 1 ]; then
	set -- -DSMS_TW_MOBILE_PROFILE=1
elif [ "${SMS_TEST_PEER+x}" = x ]; then
	if ! printf '%s\n' "$SMS_TEST_PEER" | LC_ALL=C grep -Eq '^(09[0-9]{8}|\+?8869[0-9]{8})$'; then
		echo "SMS_TEST_PEER must be a Taiwan mobile number" >&2
		exit 2
	fi
	set -- "-DSMS_TEST_PEER=\"$SMS_TEST_PEER\""
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

# Linker searches libc.so / libmeigapps.so. The SONAME inside those ELFs
# is what the device loader opens (libc.so.6, libmeigapps.so).
ln -s "$ROOT/lib/libc-2.31.so" "$STAGE/libc.so"
ln -s "$ROOT/lib/ld-2.31.so" "$STAGE/ld-linux-armhf.so.3"
ln -s "$ROOT/lib/libpthread-2.31.so" "$STAGE/libpthread.so"
ln -s "$ROOT/usr/lib/libmeigapps.so" "$STAGE/libmeigapps.so"
ln -s "$ROOT/usr/lib/libmeigbase.so" "$STAGE/libmeigbase.so"
ln -s "$ROOT/usr/lib/libmal.so" "$STAGE/libmal.so"
ln -s "$ROOT/usr/lib/libcrypto.so.1.1" "$STAGE/libcrypto.so"

"$CLANG" --target=armv7-unknown-linux-gnueabihf -mthumb -mfpu=neon -mfloat-abi=hard \
	-Os -fPIE -fno-stack-protector -fno-builtin -nostdlib -nostartfiles \
	-fuse-ld="$LLD" \
	-Wl,-pie -Wl,-e,_start -Wl,-z,now -Wl,--hash-style=gnu \
	-Wl,--pack-dyn-relocs=none \
	-Wl,-dynamic-linker,/lib/ld-linux-armhf.so.3 \
	-L"$STAGE" \
	"$@" start.S sms.c \
	-lmal -lmeigapps -lpthread -lc \
	-o "$OUTPUT"
file "$OUTPUT"
