# yolo-depth — YOLO26-depth → QCS8550 (Hexagon V73)

Converts Ultralytics YOLO26-depth into a QNN context binary the QCS8550 board can load.

## Quick start

```bash
uv sync
./build_qnn.sh          # 768px (default); ./build_qnn.sh 640 for another size
```

Output: `artifacts/y26n_<imgsz>_fp16_v73.bin` — copy to the board and run with `qnn-net-run`.

## Why not `yolo export format=qnn`

That path compiles the context binary with the QAIRT bundled in `onnxruntime-qnn` (>= 2.45).
The board runs QAIRT **2.32.0** and rejects anything newer:

```
<E> Using newer context binary on old SDK
```

QNN context binaries are backward compatible only, and no published `onnxruntime-qnn` wheel
bundles a QAIRT old enough. `build_qnn.sh` therefore drives the board's **own** x86_64
converter toolchain instead, so the output is stamped 2.32.0 and loads.

`export_qnn.py` keeps the stock Ultralytics path for reference — **its output cannot run on
this board**.

## Setup: staging the toolchain

`build_qnn.sh` expects `qairt-2.32/` (gitignored, ~858 MB), copied from the board:

```bash
scp -r root@<board>:/opt/qcom/qirp-sdk/bin/x86_64-linux-clang qairt-2.32/
scp -r root@<board>:/opt/qcom/qirp-sdk/lib/x86_64-linux-clang qairt-2.32/libs/
scp -r root@<board>:/opt/qcom/qirp-sdk/lib/python/qti         qairt-2.32/
```

On Ubuntu 24.04 it also needs, all user-level: CPython **3.10** (the native modules link
`libpython3.10.so.1.0`), LLVM-18 `libc++`/`libc++abi`/`libunwind` under `qairt-2.32/cxx`
(Ubuntu ships only `libunwind.so.8`), and a venv at `qairt-2.32/venv310` with
numpy/onnx/protobuf.

## Board-side inference

```bash
. /opt/qcom/qirp-sdk/qirp-setup.sh
echo /data/input.raw > /data/l.txt
qnn-net-run --backend libQnnHtp.so \
  --retrieve_context /data/y26n_768_fp16_v73.bin \
  --input_list /data/l.txt --output_dir /data/out
```

Input is NCHW `[1,3,768,768]` float32 in `[0,1]`, **RGB** (cv2 loads BGR — convert).
Output is `589824` float32 depth values in meters.

Measured: ~40 ms/frame on Hexagon V73; corr 0.9997 vs the ONNX reference.

## The silent footgun

`qnn-context-binary-generator --config_file` needs a `backend_extensions` wrapper. Handed a
bare HTP config it applies nothing, warns nothing, and exits 0 — producing a **V68/O0** binary
that loads and runs on V73 with no error but emits a constant, input-independent depth map.

`build_qnn.sh` fails hard if the built binary is not `"dsp arch": 73`.

See `../HANDOVER-yolo26-depth.md` for the full record.
