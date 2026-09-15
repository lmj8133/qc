#!/usr/bin/env bash
# One-shot: build, stage the model, and run the depth pipeline on the board.
#
# /dev/shm is a tmpfs, so everything staged there is lost on reboot -- this
# script re-stages each time rather than assuming previous state survives.
#
# Usage:
#   ./run.sh                 # 512px with display (30 FPS, the recommended mode)
#   ./run.sh 512 bench       # 512px headless benchmark, 300 frames
#   ./run.sh 640 bench       # 640px headless benchmark
#   ./run.sh 512 live        # run until Ctrl-C
# Exit code 0 on success, non-zero on failure.
set -euo pipefail

SIZE="${1:-512}"
MODE="${2:-display}"
BOARD="${BOARD:-192.168.3.80}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="y26n_${SIZE}_fp16_v73.bin"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 root@${BOARD}"

[ -f "$HERE/../artifacts/$BIN" ] || {
	echo "ERROR: $HERE/../artifacts/$BIN not found. Build it first:" >&2
	echo "         cd $HERE/.. && ./build_qnn.sh $SIZE" >&2
	exit 1
}

case "$MODE" in
	bench) ARGS="--frames 300 --no-display" ;;
	live)  ARGS="--frames 0" ;;
	display) ARGS="--frames 300" ;;
	*) echo "ERROR: mode must be display|bench|live (got '$MODE')" >&2; exit 2 ;;
esac

echo "==> building on $BOARD"
"$HERE/build.sh" "$BOARD" >/dev/null

echo "==> staging $BIN"
scp -q -o BatchMode=yes "$HERE/../artifacts/$BIN" "root@${BOARD}:/dev/shm/"

echo "==> running (${SIZE}px, $MODE)"
# XDG_RUNTIME_DIR: Weston's socket is at /run/user/root, NOT /run/user/0.
$SSH "bash -lc '
	source /opt/qcom/qirp-sdk/qirp-setup.sh >/dev/null 2>&1
	export LD_LIBRARY_PATH=/opt/qcom/qirp-sdk/lib/aarch64-oe-linux-gcc11.2:\$LD_LIBRARY_PATH
	export XDG_RUNTIME_DIR=/run/user/root WAYLAND_DISPLAY=wayland-1
	cd /dev/shm && ./depth_cam --model $BIN $ARGS
'"
