#!/usr/bin/env bash
# Build and run the persistent-inference benchmark ON THE BOARD.
# The board has gcc, so this compiles natively -- no cross-compile needed.
#
# Usage:  ./build.sh [board-ip]
set -euo pipefail
BOARD="${1:-192.168.3.80}"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 root@$BOARD"

scp -o BatchMode=yes qnn_bench.c "root@$BOARD:/dev/shm/"
# shellcheck disable=SC2087
$SSH 'bash -s' <<'REMOTE'
source /opt/qcom/qirp-sdk/qirp-setup.sh >/dev/null 2>&1
export LD_LIBRARY_PATH=/opt/qcom/qirp-sdk/lib/aarch64-oe-linux-gcc11.2:$LD_LIBRARY_PATH
cd /dev/shm
gcc -O2 -o qnn_bench qnn_bench.c -I/opt/qcom/qirp-sdk/include -ldl -lm
echo "built /dev/shm/qnn_bench"
REMOTE
echo
echo "Run it with, e.g.:"
echo "  ssh root@$BOARD '. /opt/qcom/qirp-sdk/qirp-setup.sh;"
echo "    export LD_LIBRARY_PATH=/opt/qcom/qirp-sdk/lib/aarch64-oe-linux-gcc11.2:\$LD_LIBRARY_PATH;"
echo "    cd /dev/shm && ./qnn_bench y26n_640_fp16_v73.bin in_640.raw 200'"
echo
echo "Stage the context binary and a raw NCHW float32 input into /dev/shm first."
echo "Set QNN_BENCH_DUMP=<path> to write the last output for correctness checking."
