#!/usr/bin/env bash
# Build a QCS8550-loadable QNN context binary for YOLO26-depth.
#
# Why not `yolo export format=qnn`: that path compiles the context binary with the QAIRT
# bundled in onnxruntime-qnn (>= 2.45). The board runs QAIRT 2.32.0 and rejects any newer
# binary with "Using newer context binary on old SDK". We therefore drive the board-exact
# 2.32.0 x86 converter staged in qairt-2.32/ instead.
#
# Usage: ./build_qnn.sh [imgsz] [weights]
# Exit codes: 0 success, non-zero on any stage failure.
set -euo pipefail

IMGSZ="${1:-768}"
WEIGHTS="${2:-weights/yolo26n-depth.pt}"
PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK="$PROJ/qairt-2.32"
OUT="$PROJ/artifacts"
STEM="y26n_${IMGSZ}_fp16"

[ -d "$SDK" ] || { echo "ERROR: $SDK missing. Copy the board's x86_64-linux-clang toolchain first." >&2; exit 1; }

PY310LIB="$(ls -d "$HOME"/.local/share/uv/python/cpython-3.10*/lib 2>/dev/null | head -1)"
export QNN_SDK_ROOT="$SDK" SNPE_ROOT="$SDK" PYTHONPATH="$SDK"
export LD_LIBRARY_PATH="$SDK/libs/x86_64-linux-clang:$SDK/cxx/usr/lib/x86_64-linux-gnu:$SDK/cxx/usr/lib/llvm-18/lib:$PY310LIB"

mkdir -p "$OUT"

echo "[1/3] PyTorch -> ONNX (NCHW, opset 17, imgsz=$IMGSZ)"
uv run python -c "
from ultralytics import YOLO
import shutil
p = YOLO('$WEIGHTS').export(format='onnx', imgsz=$IMGSZ, opset=17, simplify=True)
shutil.copy(p, '$OUT/${STEM}.onnx')
"

echo "[2/3] ONNX -> FP16 DLC (QAIRT 2.32.0 converter)"
"$SDK/venv310/bin/python" "$SDK/x86_64-linux-clang/qairt-converter" \
    -i "$OUT/${STEM}.onnx" --float_bitwidth 16 --output_path "$OUT/${STEM}.dlc"

echo "[3/3] DLC -> HTP context binary (Hexagon V73)"
# The graph name inside the DLC is the --output_path basename; keep htp_cfg graph_names in sync.
sed -i "s/\"graph_names\": \[\"[^\"]*\"\]/\"graph_names\": [\"${STEM}\"]/" "$OUT/htp_cfg_v73.json"
# NOTE: --config_file needs the backend_extensions wrapper. A bare HTP config is silently
# ignored, yielding a V68/O0 binary that loads but returns a constant, input-independent map.
"$SDK/x86_64-linux-clang/qnn-context-binary-generator" \
    --backend "$SDK/libs/x86_64-linux-clang/libQnnHtp.so" \
    --model "$SDK/libs/x86_64-linux-clang/libQnnModelDlc.so" \
    --dlc_path "$OUT/${STEM}.dlc" \
    --config_file "$OUT/htp_backend_ext.json" \
    --binary_file "${STEM}_v73" --output_dir "$OUT"

echo "--- verifying the backend config was applied ---"
# `dsp arch` is the correctness-critical field: a V68-targeted binary loads on this V73 board
# and reports no error, but never propagates its input (constant output, std=0). The per-graph
# fields (vtcmSize/optimizationLevel) only affect performance, so they warn rather than fail.
"$SDK/x86_64-linux-clang/qnn-context-binary-utility" \
    --context_binary "$OUT/${STEM}_v73.bin" --json_file "$OUT/ctxinfo.json" >/dev/null
if ! grep -q '"dsp arch": 73' "$OUT/ctxinfo.json"; then
    echo "ERROR: context binary is not targeting V73 - the backend_extensions wrapper was ignored." >&2
    grep -oE '"(dsp arch|soc model|vtcmSize|optimizationLevel)"[^,}]*' "$OUT/ctxinfo.json" >&2
    exit 2
fi
if ! grep -q '"optimizationLevel": 3' "$OUT/ctxinfo.json"; then
    echo "WARNING: optimizationLevel != 3 - graph_names likely does not match the DLC graph." >&2
    echo "         Output will be correct but slower than measured." >&2
fi
grep -oE '"(dsp arch|soc model|vtcmSize|optimizationLevel)"[^,}]*' "$OUT/ctxinfo.json" | sort -u
echo "OK -> $OUT/${STEM}_v73.bin"
