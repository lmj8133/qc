# yolo-depth — YOLO26-depth → QCS8550 (Hexagon V73)

Converts Ultralytics YOLO26-depth into a QNN context binary the QCS8550 board can load.

## Quick start

```bash
uv sync
./build_qnn.sh          # 768px (default); ./build_qnn.sh 640 for another size
```

Output: `artifacts/y26n_<imgsz>_fp16_v73.bin` — copy to the board and run with `qnn-net-run`.

Note: the build rewrites `graph_names` in `artifacts/htp_cfg_v73.json` to match the size
being built, so that file shows up as modified after a non-768 build. That is expected churn,
not a real edit — `git checkout` it if you do not intend to commit a new default.

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

On Ubuntu 24.04 it also needs three user-level pieces — **no root, nothing installed
system-wide**. These were previously described but not spelled out; the exact steps are:

```bash
cd qairt-2.32

# 1. CPython 3.10 -- the native converter modules link libpython3.10.so.1.0,
#    so the host's 3.12 cannot load them.
uv python install 3.10

# 2. LLVM-18 C++ runtime. Ubuntu 24.04 ships libunwind.so.8, but the SDK wants
#    libunwind.so.1, so extract rather than install.
mkdir -p /tmp/llvm18 cxx
for pkg in libc++1-18 libc++abi1-18 libunwind-18; do
    ( cd /tmp/llvm18 && apt-get download "$pkg" && dpkg-deb -x "$pkg"*.deb . )
done
cp -a /tmp/llvm18/usr cxx/

# 3. A Python 3.10 venv for the converter CLIs (they fail at `import numpy` otherwise).
uv venv --python 3.10 venv310
VIRTUAL_ENV=venv310 uv pip install \
    numpy==1.26.4 onnx==1.17.0 onnxruntime==1.23.2 onnxsim protobuf PyYAML sympy pandas
```

Verify the result — this must print the board's own SDK version,
`QNN SDK v2.32.0.250228225014_116386`:

```bash
# note cpython-3.10.* -- the bare cpython-3.10 name is a symlink that env cannot traverse
PY310LIB=$(ls -d ~/.local/share/uv/python/cpython-3.10.*/lib | head -1)
LD_LIBRARY_PATH=$PWD/libs/x86_64-linux-clang:$PWD/cxx/usr/lib/llvm-18/lib:$PY310LIB \
    ./x86_64-linux-clang/qnn-context-binary-generator --version
```

If that prints a version, the native half works. For the Python converter half:

```bash
QNN_SDK_ROOT=$PWD PYTHONPATH=$PWD \
LD_LIBRARY_PATH=$PWD/libs/x86_64-linux-clang:$PWD/cxx/usr/lib/llvm-18/lib:$PY310LIB \
    ./venv310/bin/python -c "from qti.aisw.converters.common import ir_graph; print('converter OK')"
```

Both are exercised for real by `../build_qnn.sh`, which is the definitive test.

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
