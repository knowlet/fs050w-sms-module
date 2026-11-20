#!/bin/sh
set -eu
SCRIPT_DIR="$(CDPATH='' cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
ROOT="${ROOT:-$(cd "$SCRIPT_DIR/../../extractions/FS050W_V2.1.0.bin.extracted/0/system" && pwd)}"
CLANG="${CLANG:-/opt/homebrew/opt/llvm/bin/clang}"
LLD="${LLD:-/opt/homebrew/bin/ld.lld}"
OUT_DIR="${OUT_DIR:-/private/tmp}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
ln -s "$ROOT/lib/libc-2.31.so" "$STAGE/libc.so"
ln -s "$ROOT/lib/ld-2.31.so" "$STAGE/ld-linux-armhf.so.3"
ln -s "$ROOT/lib/libpthread-2.31.so" "$STAGE/libpthread.so"
ln -s "$ROOT/usr/lib/libqmi_cci.so.1.0.0" "$STAGE/libqmi_cci.so"
ln -s "$ROOT/usr/lib/libqmiservices.so.1.0.0" "$STAGE/libqmiservices.so"
ln -s "$ROOT/usr/lib/libqmi_encdec.so.1.0.0" "$STAGE/libqmi_encdec.so"
ln -s "$ROOT/usr/lib/liblog.so.0.0.0" "$STAGE/liblog.so"
for target in 2 1; do
	case "$target" in
	2) name=fs050w-nas-csps-permanent ;;
	1) name=fs050w-nas-ps-permanent-restore ;;
	esac
	"$CLANG" --target=armv7-unknown-linux-gnueabihf -mthumb -mfpu=neon -mfloat-abi=hard \
		-Os -Wall -Wextra -Werror -fPIE -fno-stack-protector -fno-builtin \
		-nostdlib -nostartfiles --ld-path="$LLD" \
		-Wl,-pie -Wl,-e,_start -Wl,-z,now -Wl,--hash-style=gnu \
		-Wl,--pack-dyn-relocs=none \
		-Wl,-dynamic-linker,/lib/ld-linux-armhf.so.3 \
		-L"$STAGE" "../start.S" nas_domain_permanent_set.c \
		-DNAS_TARGET_DOMAIN="$target" \
		-lqmiservices -lqmi_cci -lqmi_encdec -lpthread -lc \
		-o "$OUT_DIR/$name"
	file "$OUT_DIR/$name"
	done
