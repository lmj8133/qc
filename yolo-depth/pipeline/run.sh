#!/usr/bin/env bash
# One-shot: build, stage the model, and run the depth pipeline on the board.
#
# /dev/shm is a tmpfs, so everything staged there is lost on reboot -- this
# script re-stages each time rather than assuming previous state survives.
#
# Usage:
#   ./run.sh                 # 512px with display (side by side)
#   ./run.sh 384 live        # 384px, runs until Ctrl-C -- best for demos (30 FPS)
#   ./run.sh 512 bench       # 512px headless benchmark, 300 frames
#   ./run.sh 512 live --depth-only    # extra flags pass through to depth_cam
# Exit code 0 on success, non-zero on failure.
set -euo pipefail

SIZE="${1:-512}"
MODE="${2:-display}"
shift $(( $# > 2 ? 2 : $# ))
# Anything after the mode is passed through to depth_cam verbatim, e.g.
#   ./run.sh 512 live --depth-only
EXTRA="$*"   # plain string: safe under `set -u` when empty
BOARD="${BOARD:-192.168.3.80}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="y26n_${SIZE}_fp16_v73.bin"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 root@${BOARD}"
# -t allocates a TTY so Ctrl-C reaches the remote process group. Without it the
# local ssh dies on Ctrl-C and depth_cam keeps running on the board forever.
SSH_TTY="ssh -tt -o BatchMode=yes -o ConnectTimeout=10 root@${BOARD}"

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

echo "==> running (${SIZE}px, $MODE)   [Ctrl-C to stop]"

# Clean up on the board even if the SSH connection itself is lost (dropped
# link, closed laptop, killed terminal) -- in that case no signal is delivered
# remotely and depth_cam would otherwise keep holding the camera.
cleanup() {
	$SSH 'pkill -x depth_cam; pkill -f "gst-launch-1.0 -q fdsrc"' >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

# XDG_RUNTIME_DIR: Weston's socket is at /run/user/root, NOT /run/user/0.
#
# -tt gives the remote command a TTY so Ctrl-C is delivered as SIGINT to the
# remote process group; depth_cam traps SIGINT/SIGTERM and shuts down cleanly.
# The remote shell also traps EXIT so the gst-launch child dies with it.
$SSH_TTY "bash -lc '
	trap \"pkill -P \\\$\\\$ 2>/dev/null; exit\" EXIT INT TERM
	source /opt/qcom/qirp-sdk/qirp-setup.sh >/dev/null 2>&1
	export LD_LIBRARY_PATH=/opt/qcom/qirp-sdk/lib/aarch64-oe-linux-gcc11.2:\$LD_LIBRARY_PATH
	export XDG_RUNTIME_DIR=/run/user/root WAYLAND_DISPLAY=wayland-1
	cd /dev/shm && exec ./depth_cam --model $BIN $ARGS $EXTRA
'"
