#!/bin/bash
# Cross-compile for QCS8550 (aarch64). Sources the SDK environment so you
# cannot forget it -- the usual cause of an x86_64 binary that fails on the
# board with "cannot execute binary file".
set -euo pipefail

SDK_ENV="${SDK_ENV:-/home/agmis/Documents/qc/sdk/environment-setup-aarch64-oe-linux}"
BUILD_DIR="${BUILD_DIR:-build}"

if [ ! -f "$SDK_ENV" ]; then
	echo "error: SDK environment not found: $SDK_ENV" >&2
	echo "       set SDK_ENV to the correct path" >&2
	exit 1
fi

# shellcheck disable=SC1090
. "$SDK_ENV"

# CMake caches the compiler on first configure. If the cache was created
# without the SDK environment it would keep using the host gcc, so drop it.
if [ -f "$BUILD_DIR/CMakeCache.txt" ] &&
   ! grep -q "aarch64" "$BUILD_DIR/CMakeCache.txt"; then
	echo "note: stale host-built cache detected, reconfiguring"
	rm -rf "$BUILD_DIR"
fi

cmake -B "$BUILD_DIR" "$@"
cmake --build "$BUILD_DIR" -j"$(nproc)"

# Fail loudly rather than shipping a binary the board cannot run.
BIN="$BUILD_DIR/myapp"
if ! file "$BIN" | grep -q "ARM aarch64"; then
	echo "error: $BIN is not an aarch64 binary" >&2
	file "$BIN" >&2
	exit 1
fi

echo
file "$BIN"
