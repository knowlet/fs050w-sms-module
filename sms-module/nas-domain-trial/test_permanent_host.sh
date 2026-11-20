#!/bin/sh
set -eu
SCRIPT_DIR="$(CDPATH='' cd "$(dirname "$0")" && pwd)"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
for target in 2 1; do
	"${CC:-cc}" -std=c11 -O1 -Wall -Wextra -Werror -DNAS_TARGET_DOMAIN="$target" \
		"$SCRIPT_DIR/test_permanent_host.c" -o "$STAGE/test-$target"
	"$STAGE/test-$target" > "$STAGE/log-$target"
	tail -n 1 "$STAGE/log-$target"
done
python3 "$SCRIPT_DIR/verify_permanent_wire.py"
