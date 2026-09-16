#!/usr/bin/env bash
# Build the real-time depth pipeline ON THE BOARD (the board has gcc, so this
# compiles natively -- no cross-compile toolchain needed).
#
# Usage:  ./build.sh [board-ip]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=board.env
source "$HERE/board.env"
BOARD="${1:-$BOARD_DEFAULT}"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 root@$BOARD"

if ! $SSH true 2>/dev/null; then
	echo "ERROR: no SSH to the board at $BOARD" >&2
	echo "The board's IP has moved before (.63 -> .80 -> .67) and looks just" >&2
	echo "like a hung board when it does. See board.env for how to find it." >&2
	exit 1
fi

scp -o BatchMode=yes "$HERE/depth_cam.c" "root@$BOARD:/dev/shm/"
# shellcheck disable=SC2087
$SSH 'bash -s' <<'REMOTE'
source /opt/qcom/qirp-sdk/qirp-setup.sh >/dev/null 2>&1
export LD_LIBRARY_PATH=/opt/qcom/qirp-sdk/lib/aarch64-oe-linux-gcc11.2:$LD_LIBRARY_PATH
cd /dev/shm
gcc -O3 -march=armv8.2-a+fp16 -Wall -Wextra -o depth_cam depth_cam.c \
    -I/opt/qcom/qirp-sdk/include -ldl -lm
echo "built /dev/shm/depth_cam"
REMOTE

echo
echo "Stage a context binary on the board once:"
echo "  scp $HERE/../artifacts/y26n_640_fp16_v73.bin root@$BOARD:/dev/shm/"
echo
echo "Run headless (benchmark):"
echo "  $SSH '. /opt/qcom/qirp-sdk/qirp-setup.sh >/dev/null 2>&1;"
echo "    export LD_LIBRARY_PATH=/opt/qcom/qirp-sdk/lib/aarch64-oe-linux-gcc11.2:\$LD_LIBRARY_PATH;"
echo "    cd /dev/shm && ./depth_cam --model y26n_640_fp16_v73.bin --frames 300 --no-display'"
echo
echo "Run with display (Weston socket lives at /run/user/root, not /run/user/0):"
echo "  ... && ./depth_cam --model y26n_640_fp16_v73.bin --frames 300'"
