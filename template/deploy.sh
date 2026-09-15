#!/bin/bash
# Copy the binary to the board and run it.
#   ./deploy.sh              push and run
#   ./deploy.sh --no-run     push only
set -euo pipefail

BOARD="${BOARD:-root@192.168.3.63}"
DEST="${DEST:-/data}"
BIN="${BIN:-build/myapp}"

if [ ! -f "$BIN" ]; then
	echo "error: $BIN not found -- run ./build.sh first" >&2
	exit 1
fi

NAME="$(basename "$BIN")"

scp "$BIN" "$BOARD:$DEST/"
ssh "$BOARD" "chmod +x $DEST/$NAME"

if [ "${1:-}" = "--no-run" ]; then
	echo "pushed to $BOARD:$DEST/$NAME"
	exit 0
fi

echo "--- running $DEST/$NAME ---"
ssh "$BOARD" "$DEST/$NAME"
