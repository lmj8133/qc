# 交接文件：YOLO26-depth 於 QCS8550 的可行性驗證

| | |
|---|---|
| 日期 | 2026-09-15（§1、§4、§5 於同日實測後改寫） |
| 目標 | 在 QCS8550 (aida3_RDK_SOCKET) 上執行 Ultralytics YOLO26-depth |
| 執行環境 | 模型轉換在 **x86 server**；推論在板子 |
| 前置狀態 | 板子已完成 bring-up，見 `VERIFICATION-2026-09-14.md` |

> **閱讀說明**：本文件初版為「可行性推測」，後續整條路徑已實測打通。
> 標記 ✅ 者為實測結論；標記 ⛔ 者為初版的推測，實測後證實**錯誤**，保留是為了讓後續接手者不要重走。

---

## 1. 結論

**✅ 可行，且已完整驗證。YOLO26n-depth 已在板上 Hexagon V73 NPU 跑出正確深度圖。**

| 項目 | 實測結果 |
|---|---|
| 模型轉換 | ✅ 成功，329 個 op **全部**進 NPU，零 CPU fallback |
| 板上推論 | ✅ 正確，與 ONNX 參考值相關係數 **0.9997** |
| 延遲 | **~40 ms/frame（~25 FPS）**，20 次平均、Hexagon V73 |
| 產出 | `yolo-depth/build_qnn.sh` 一鍵重現；`artifacts/*.bin` 可直接上板 |

精度對照（NYU 室內實拍，768×768）：

```
板上 NPU (FP16) : min=2.4121 max=3.8867 mean=3.1068 std=0.3474
ONNX 參考值      : min=2.4426 max=3.8946 mean=3.1188 std=0.3460
```

### ⛔ 初版結論已作廢的部分

初版寫「有三個未知數需實測」、「不要直接搬上板，先驗證 runtime 基礎（階段一）」。
三個未知數現已全部有答案（見 §6），且 **QNN 路徑整條打通後，階段一的 TFLite 前置驗證已無必要** ——
它原本的用途是「先排除 runtime 問題」，但 QNN 路徑已自證可用。TFLite 路徑目前無人驗證，也不再是建議方向。

---

## 2. 目標平台事實（已驗證）

這些是實測結果，不是推測：

| 項目 | 值 |
|---|---|
| SoC | QCS8550 (kalama) |
| OS | Ubuntu 22.04.2 LTS (Jammy), aarch64 |
| Kernel | `5.15.170-qki-consolidate-android13-8-00002-g576728c775df-dirty` |
| 映像版本 | V00.00.03 / Metadata Revision `576728c775dfa24956eb041f8ea23242f468a6aa` |
| 連線 | SSH `root@192.168.3.80`（金鑰登入）、ADB、UART |
| 交叉編譯 SDK | 已安裝於 `/home/agmis/Documents/qc/sdk`，版本 hash 與板子一致 |

### 板上既有的推論資源

| 項目 | 路徑 | 說明 |
|---|---|---|
| TFLite runtime | `/usr/lib/libtensorflowlite_c.so` | C API |
| `label_image` | `/usr/bin/` | 影像分類範例 |
| `benchmark_model` | `/usr/bin/` | 效能量測，**驗證 delegate 的主要工具** |
| `multimodel_label_image` | `/usr/bin/` | |
| SNPE 範例 | `gst-snpe-yolo-ssd-display-example`<br>`gst-snpe-yolo-ssd-encode-example` | 表示 SNPE 鏈路存在 |
| TFLite 範例 | `gst-tflite-yolo-ssd-display-example`<br>`gst-tflite-posenet-display-example` | |
| labels | `/opt/qcom/qirp-sdk/data/model/labels.txt` | |

**`label_image` 支援的 delegate**（實測 `--help` 輸出）：
`--use_gpu`、`--use_xnnpack`、`--hexagon_delegate`、`--accelerated`(NNAPI)、`--gpu_backend=cl|gl`、`--allow_fp16`

### ⚠️ 關鍵缺口

**板上沒有任何 `.tflite` 或 `.dlc` 模型檔** —— 只有 `labels.txt`。
TFLite runtime 從未被實際驗證過。這是所有後續工作的前提。

### QNN / QAIRT 執行環境（已確認）

**板上有完整的 QNN runtime**，位於 `/opt/qcom/qirp-sdk/`（QIRP SDK）：

```bash
. /opt/qcom/qirp-sdk/qirp-setup.sh    # 設定 PATH / LD_LIBRARY_PATH / ADSP_LIBRARY_PATH
```

可用的 QNN backend（`lib/aarch64-oe-linux-gcc11.2/`）：

| Backend | 函式庫 | 對應硬體 |
|---|---|---|
| **HTP** | `libQnnHtp.so` + `libQnnHtpPrepare.so`、`libQnnHtpV68/V69/V73*` | **Hexagon Tensor Processor（NPU）— 效能最佳** |
| GPU | `libQnnGpu.so` | Adreno GPU |
| DSP | `libQnnDsp.so` | Hexagon DSP（舊版介面） |
| CPU | `libQnnCpu.so` | 參考用 |

Hexagon skel 位於 `lib/hexagon-v73/unsigned` 與 `hexagon-v68/unsigned`，由 `ADSP_LIBRARY_PATH` 指向。

**這使 QNN 成為優先路徑** —— HTP backend 可利用 NPU，效能遠高於 TFLite 的 GPU delegate。

### QNN 工具鏈與 NPU（已實測）

**QNN SDK v2.32.0**（2025-02）。工具位於 `/opt/qcom/qirp-sdk/bin/aarch64-oe-linux-gcc11.2/`，`qirp-setup.sh` 會加入 PATH。

| 工具 | 用途 |
|---|---|
| **`qnn-net-run`** | **主要推論執行器** |
| **`qnn-context-binary-generator`** | **在板上產生 HTP context binary** |
| `qnn-throughput-net-run` | 吞吐量量測 |
| `qnn-platform-validator` | backend 可用性檢查 |
| `qnn-profile-viewer` | 解析 profiling 輸出 |
| `snpe-net-run`、`snpe-diagview`、`snpe-parallel-run` | SNPE 路徑（舊版 API） |
| `genie-t2t-run`、`genie-t2e-run` | LLM 文字生成 |

**NPU 實測結果**（`qnn-platform-validator --backend dsp --coreVersion`）：

```
Backend Hardware  : Supported
Backend Libraries : Found
Core Version      : Hexagon Architecture V73
```

`libcdsprpc.so`（fastRPC）載入成功。

> `Library Version: Not Found` 是該工具未實作的查詢（`The fastRPC library version is not implemented yet`），非缺件 —— Prerequisites 明確回報 `Present`。
>
> `--backend` 只接受 `gpu` / `dsp` / `all`；**HTP 歸類在 `dsp` 之下**，傳 `htp` 會被拒絕。

### ⛔ 已作廢：「context binary 在板上編譯」

初版建議「server 端產出未編譯的 QNN model，再到板上編譯成 context binary」。
**方向對（在目標機器編譯，版本必然正確），但前提不成立，實測行不通：**

`qnn-context-binary-generator --model` 要吃的是 `qnn_model_name.so`，
而**板上的 aarch64 工具集沒有任何 converter 能從 ONNX 產出那個 `.so`** ——
`qairt-converter` / `qnn-onnx-converter` 在 aarch64 的 bin 目錄裡並不存在。

板上 aarch64 可用的工具只有這些：

```
genie-t2e-run  genie-t2t-run  qnn-context-binary-generator  qnn-net-run
qnn-platform-validator  qnn-profile-viewer  qnn-throughput-net-run  qtld-net-run
snpe-diagview  snpe-net-run  snpe-parallel-run  snpe-platform-validator  snpe-throughput-net-run
```

**✅ 實際可行的做法正好相反**：把板子自帶的 **x86_64 轉換工具鏈搬到 server**，全程在 server 編譯。
版本一樣必然正確（都是 2.32.0），而且 server 算力遠高於板子。詳見 §4。

### 典型推論指令

```bash
. /opt/qcom/qirp-sdk/qirp-setup.sh

qnn-net-run \
  --backend libQnnHtp.so \
  --model <model.so>            # 或 --retrieve_context <context.bin>
  --input_list input_list.txt \
  --output_dir /data/output
```

`--input_list` 為文字檔，每行一個 raw tensor 檔路徑。**輸入須預先前處理為 raw float32/uint8，不可直接餵圖片。**

backend 退階順序用於定位問題層級：`libQnnHtp.so`（NPU）→ `libQnnGpu.so` → `libQnnCpu.so`。

### ⛔ 版本相容性 —— 不是「可能」，是**必定失敗**（已實測）

初版寫「版本落差過大**可能**產生板子無法載入的模型格式」。實測結果比這更硬：

**QNN context binary 只向下相容。舊 runtime 永遠無法載入新 SDK 編出的 binary。**

板上 runtime 為 `v2.32.0.250228225014`。餵給它一顆 2.50.40 編的 binary，錯誤訊息非常明確：

```
<E> Using newer context binary on old SDK
<E> Fail to get context blob with err 5000
<E> Failed to create context from binary with err 0x1388   # QNN_CONTEXT_ERROR_BINARY_VERSION
```

板上 `libQnnHtp.so` 內含明確的版本閘門字串，證實這是設計行為而非個案：

```
<E> Can't read future blob. Newest blob version supported: %d.%d.%d. Current blob version: %d.%d.%d.
```

**這直接判了 `onnxruntime-qnn` 路徑死刑**（含 Ultralytics 的 `format="qnn"`，見 §4/§5）：

| onnxruntime-qnn | 內建 QAIRT | x86_64 wheel |
|---|---|---|
| 2.1.1 / 2.2.0 | 2.45 / 2.46 | ❌ 僅 aarch64/Windows |
| 2.3.0 | **2.47.0** | ✅（最舊可用者） |
| 2.4.0 / 2.5.0 / 2.6.0 | 2.48.40 / 2.49.40 / 2.50.40 | ✅ |

**降版本無解** —— 最舊的 x86_64 wheel 也是 2.47，仍遠新於板上 2.32.0。實測 2.3.0 編的 binary 在板上得到位元完全相同的拒絕訊息。

### 其他已確認項目

| 項目 | 值 |
|---|---|
| TFLite runtime 版本 | **2.11.1** |
| `benchmark_model` 支援的 delegate | `use_gpu`、`use_xnnpack`、`external_delegate` |
| GPU 節點 | `/sys/class/kgsl/kgsl-3d0`（Adreno） |
| QIRP SDK | `/opt/qcom/qirp-sdk`、`/opt/qcom/qirf-sdk` |

⚠️ **TFLite 2.11.1 偏舊**（2022 年版本）。server 端用新版 Ultralytics/TensorFlow 匯出的 `.tflite`，可能因 flatbuffer schema 或 op 版本過新而無法載入。若遇載入失敗，需降低匯出端的 TF 版本。

⚠️ `benchmark_model` **無 `use_nnapi` / `use_hexagon`**，但有 **`external_delegate`** —— 這正是掛載 QNN delegate 的介面。TFLite 路徑若要用上 NPU，走的是 `--external_delegate_path=<QNN TFLite delegate>` 而非內建旗標。

## 3. 模型資訊（來自官方文件）

來源：https://docs.ultralytics.com/tasks/depth

單目深度估計，輸出每像素公尺深度值。

| 模型 | 輸入 | Delta1 NYU | Abs_Rel | RMSE | T4+TensorRT |
|---|---|---|---|---|---|
| YOLO26n-depth | 768px | 0.882 | 0.109 | 0.414m | 2.7ms |
| YOLO26s-depth | 768px | 0.896 | 0.104 | 0.399m | 3.8ms |
| YOLO26m-depth | 768px | 0.921 | 0.089 | 0.364m | 6.0ms |
| YOLO26l-depth | 768px | 0.930 | 0.083 | 0.351m | 7.7ms |
| YOLO26x-depth | 768px | 0.933 | 0.080 | 0.344m | 13.6ms |

**輸出**：`DepthMap`，`(H, W)` float32，單位公尺，範圍約 0.02–150m（unbounded log-depth head，`exp(logit)`）。

**⚠️ 上表速度是 T4 GPU + TensorRT**，與 QCS8550 的 Adreno GPU / Hexagon DSP 是完全不同的硬體。**不可作為板上延遲的預估依據**，必須實測。

輸入 768px 比常見的 640 更重（像素數約 1.44 倍）。

---

## 4. 轉換路徑（已定案）

**✅ 採用 QNN，並且用「板子自帶的 x86_64 工具鏈」在 server 上轉換。**

### ⛔ 已作廢：「server 端須另行取得 QAIRT SDK」

初版寫「server 端仍需安裝 Qualcomm AI Engine Direct SDK（QAIRT）…須另行取得」。
**不需要，也不該去申請** —— 申請到的多半是新版，反而會撞上 §2 的版本閘門。

**板子自己就帶著一整套 x86_64 轉換工具**，位於 `/opt/qcom/qirp-sdk/`：

```
bin/x86_64-linux-clang/     44 個工具，47 MB
  qairt-converter  qairt-quantizer  qairt-dlc-info  qnn-onnx-converter
  qnn-model-lib-generator  qnn-context-binary-generator  qnn-net-run  snpe-onnx-to-dlc ...
lib/x86_64-linux-clang/    187 MB
lib/python/qti/            363 MB（含 linux-x86_64 原生 .so）
```

這些在 aarch64 板子上是**休眠**的（直接執行得到 `Exec format error`），
但搬到 x86 server 上就能跑，而且版本天生是 `2.32.0.250228225014` —— 與板上 runtime 完全一致。

工具鏈已 staged 於 `yolo-depth/qairt-2.32/`（858 MB，已 gitignore）。

### 在 Ubuntu 24.04 跑 2.32.0 工具鏈需要的四件事

SDK 是 2023 年為 Ubuntu 22.04 打包的，在 24.04 上需要補（全部 user-level，**不需 root**）：

1. **CPython 3.10**（uv 安裝）—— 原生模組 `libPyIrGraph.so` 連結 `libpython3.10.so.1.0`，host 的 3.12 跑不動
2. **LLVM-18 的 `libc++.so.1` / `libc++abi.so.1` / `libunwind.so.1`** —— Ubuntu 只提供 `libunwind.so.8`，用 `dpkg-deb -x` 解出即可，不必安裝
3. `LD_LIBRARY_PATH` 需含 SDK 自己的 `libs/x86_64-linux-clang`
4. 一個帶 numpy/onnx/protobuf 的 venv —— 否則 CLI 進入點會在 `import numpy` 失敗

> `qairt-converter` / `qairt-quantizer` 是 **Python 腳本**（需用 venv 的直譯器跑）；
> `qnn-context-binary-generator` / `qnn-net-run` 是原生 ELF（直接執行）。

以上都已封裝進 `yolo-depth/build_qnn.sh`。

### 兩條路徑的現況

| 路徑 | 狀態 |
|---|---|
| **QNN + 板載 x86 工具鏈** ⬅ 採用 | ✅ 已驗證，40 ms/frame，精度 corr 0.9997 |
| **QNN + `onnxruntime-qnn`**（Ultralytics `format="qnn"`） | ⛔ **死路** —— 轉換會成功，但板子必定拒收（§2） |
| **LiteRT** (`.tflite`) | 未驗證。板上 runtime 2.11.1（2022）偏舊，且 depth task 支援未明載 |

### ⚠️ 關於 FP16 vs 量化

採用 **FP16**：不需校準資料，精度更貼近參考值。

- HTP **沒有 FP32 執行路徑**，因此 FP32 DLC 無法 finalize（錯誤 `q::flat_from_vtcm`）。必須 FP16 或量化。
- W8A16 也實測可行（`qairt-quantizer --act_bitwidth 16 --weights_bitwidth 8` + 8 張校準圖），MAE 0.049 m、corr 0.9911，binary 較小；但需要校準資料，且對校準集外的輸入會被 clamp 在校準範圍上緣。

### ✅ 初版的「不支援 op」風險未發生

初版預期「depth 的 decoder 常含 upsample/interpolate 這類在量化或 delegate 上表現不佳的運算」，
可能遇到不支援的 op。**實測未發生** —— 329 個 op 全數轉換成功並進入 NPU。

圖中確實含 5 個 `Resize`、1 個 `ConvTranspose`、`Softmax` 與 log-depth head 的 `Exp`/`Log`，
但 QNN 全部支援。唯一與 `Resize` 有關的錯誤訊息出現在 FP32 finalize 失敗時
（`q::flat_from_vtcm`），且該錯誤歸咎的節點會隨 `vtcm_mb` 改變 —— 那是 VTCM 溢出的通用症狀，
**不是 Resize 本身不被支援**，改用 FP16 即解決。

---

## 5. 實際執行流程（已驗證）

### 一鍵重現

```bash
cd yolo-depth
./build_qnn.sh                    # 預設 768px / weights/yolo26n-depth.pt
./build_qnn.sh 640                # 換 imgsz
```

三階段：**PyTorch → ONNX(NCHW, opset 17) → FP16 DLC → V73 context binary**，
最後自動驗證 `dsp arch == 73`，不符就 `exit 2`。

### server 端環境

```bash
uv init yolo-depth && cd yolo-depth
uv add ultralytics onnx onnxslim        # ultralytics 8.4.152 / torch 2.14.0+cu130
```

權重在 assets 的 **v8.4.0** tag（不是 v8.3.0，該 tag 下沒有 depth 權重）：

```bash
curl -sSL -o weights/yolo26n-depth.pt \
  https://github.com/ultralytics/assets/releases/download/v8.4.0/yolo26n-depth.pt
```

### ⛔ 已作廢：階段一（TFLite runtime 前置驗證）

初版要求「先用 MobileNet 之類的小模型驗證 TFLite runtime，不要跳過」。
該步驟的目的是「先排除 runtime 本身的問題」，但 **QNN 路徑已整條自證可用**，此前置驗證已無必要。
只有在未來要改走 TFLite 路徑時才需要回頭做。

### ⛔ 已作廢：`model.export(format="qnn")`

初版階段二建議：

```python
model.export(format="qnn")    # ⛔ 不要用
```

**這行會成功執行**（7 秒，產出 `yolo26n-depth_qnn.onnx`，329 op 全進 NPU、零 fallback），
**但產出的 binary 板子必定拒收** —— 它用 `onnxruntime-qnn` 內建的 QAIRT 2.50.40 編譯（見 §2）。

保留 `yolo-depth/export_qnn.py` 供參考，但它的產出**不能上板**。

另外兩點與初版描述不同：
- QNN 匯出**強制量化**為 `w8a16`（`qnn` 列在 `FP32_UNSUPPORTED_FORMATS`），不存在「FP32 先試」
- 匯出的輸入是 **channel-last `[1,768,768,3]`**（`QNNModel` 包裝所致）

### ⚠️⚠️ 最陰險的坑：`--config_file` 需要 `backend_extensions` 外層包裝

**這個坑不會有任何錯誤訊息。**

`qnn-context-binary-generator --config_file` 期待的是 backend-extensions 外層結構。
直接餵裸的 HTP config（`graphs`/`devices`/`context`）會被**靜默忽略、exit 0、無警告**，
編出 **dsp arch 68 / O0 / vtcm 4** 的 binary。

該 binary 在 V73 板上**載入成功、執行成功、零錯誤**，但**每個像素輸出同一個值**
（log-depth 模型：全圖 4.441，std=0），且對任何輸入都一樣。

正確寫法要兩層：

```jsonc
// htp_backend_ext.json —— 傳給 --config_file 的是這個
{"backend_extensions": {"shared_library_path": "libQnnHtpNetRunExtensions.so",
                        "config_file_path": "artifacts/htp_cfg_v73.json"}}

// htp_cfg_v73.json —— 真正的 HTP 設定
{"graphs":  [{"graph_names": ["<DLC graph 名稱>"], "vtcm_mb": 8, "O": 3, "fp16_relaxed_precision": 1}],
 "devices": [{"dsp_arch": "v73", "soc_model": 43, "pd_session": "unsigned"}],
 "context": {"weight_sharing_enabled": false}}
```

**哪個欄位決定正確性**（A/B 實測）：

| 設定 | 結果 |
|---|---|
| 裸 config（無 wrapper） | arch **68** → 輸出常數 ⛔ |
| wrapper + **故意寫錯** `graph_names` | arch **73**，但 O0/vtcm4 → 輸出**正確** ✅ |

→ **`dsp arch`（device 層）決定正確性**；`graph_names` 與 O/vtcm 只影響**效能**。
`graph_names` 必須等於 DLC 的 graph 名稱（即 `qairt-converter --output_path` 的 basename）才能拿到 O3/vtcm8。

> 註：曾有「舊 binary 是殘檔」的說法，已由 A/B 測試否證 ——
> 用裸 config 重新編譯出的新檔與當初那顆壞檔 **MD5 完全相同**，是可重現的行為，不是殘檔。

**出貨前務必檢查**（`build_qnn.sh` 已內建）：

```bash
qnn-context-binary-utility --context_binary X.bin --json_file i.json
grep -E '"dsp arch"|"optimizationLevel"|"vtcmSize"|"soc model"' i.json
# 必須是 arch 73 / O 3 / vtcm 8 / soc 43
```

### 板上執行

```bash
. /opt/qcom/qirp-sdk/qirp-setup.sh
echo /data/input.raw > /data/l.txt
qnn-net-run --backend libQnnHtp.so \
  --retrieve_context /data/y26n_768_fp16_v73.bin \
  --input_list /data/l.txt --output_dir /data/out
# 輸出：/data/out/Result_0/output0.raw，589824 個 float32（768×768），單位公尺
```

**輸入格式**：NCHW `[1,3,768,768]` float32、範圍 [0,1]、**RGB**（cv2 讀進來是 BGR，須轉換）。
DLC 邊界保持 NCHW，內部自行插 Transpose 轉 NHWC，所以餵 NCHW 是對的。

> `qnn-net-run` 預設以浮點解析輸入檔並自行轉換，因此餵 float32 即可，
> 不需要配合 graph 的 Float_16 而預先轉半精度（除非加 `--use_native_input_files`）。

---

## 6. 三個未知數 —— 已全部回答

| # | 問題 | ✅ 實測答案 |
|---|---|---|
| 1 | 能否匯出 QNN / TFLite？ | **能**。QNN 轉換成功，無不支援的 op。（TFLite 未測） |
| 2 | HTP（NPU）能接管多少 op？ | **100%** —— 329 個 op 融成單一 partition，零 CPU fallback |
| 3 | 實際延遲？ | **~40 ms/frame（~25 FPS）** @768px；已拆解，見下表 |

初版警告「不可用 T4 的 2.7ms 推估」是對的：實測 40 ms，約為 T4+TensorRT 的 **15 倍**。

### 延遲拆解（100 次，檔案置於 tmpfs 以排除磁碟 I/O）

| 層級 | 時間 | 增量 |
|---|---|---|
| **Accelerator（不含等待）** | **39.4 ms** | ← **純 NPU 運算，真正的瓶頸** |
| Accelerator | 39.6 ms | +0.2 ms 等待 |
| RPC | 41.0 ms | +1.4 ms fastRPC |
| QNN 總計 | 41.9 ms | +0.9 ms 框架 |

**overhead 僅 2.5 ms（6%）**，分布極穩（min 39.4 / max 41.0 ms）。

**`--perf_profile` 無效**：`default` / `balanced` / `high_performance` /
`sustained_high_performance` / `burst` 五者差異 **<0.05%**（39.61–39.63 ms）——
NPU 本來就跑滿，不存在 DVFS 節流，此路不通。

> 首次推論含 **HVX + HMX power-on 約 23 ms** 的暖機成本，之後降至約 5 ms。量測穩態延遲時應捨棄前幾次。

### 解析度是唯一有效的槓桿

延遲與像素數近乎完全線性，且 FP16 精度在各尺寸皆保持 corr ≥ 0.9997：

| imgsz | 延遲 | FPS | 對照同尺寸 ONNX |
|---|---|---|---|
| 384 | **9.5 ms** | **105** | corr 0.99984 / MAE 0.030 m |
| 512 | 17.5 ms | 57 | corr 0.99968 / MAE 0.024 m |
| 640 | 28.6 ms | 35 | corr 0.99976 / MAE 0.013 m |
| 768 | 39.6 ms | 25 | corr 0.99976 / MAE 0.012 m |

四種尺寸的 binary 皆可用 `./build_qnn.sh <size>` 產生。

> 上表 MAE 衡量的是 **FP16 轉換誤差**（板上 vs 同尺寸 ONNX），
> **不是深度估計的絕對精度** —— 低解析度模型對細節的還原本就較差，那需跑 NYU val 654 張才能評估。

### 尚未驗證

- **精度僅比對 2 張圖** —— 未跑 NYU val 654 張完整 Delta1/RMSE，無法對照官方 0.882 / 0.414m
- **端到端 pipeline 未做**（原階段四）—— 板上**沒有 onnxruntime**、沒有 pip、Python 僅 3.10
  （`onnxruntime-qnn` wheel 從 cp311 起跳），因此要走 `qnn-net-run` 或 QNN C API 接 GStreamer

---

## 7. 板子連線資訊

| 項目 | 值 |
|---|---|
| SSH | `root@192.168.3.80`（金鑰，見下方注意事項） |
| ADB | `adb shell`（以 uid 2000 執行，權限受限） |
| UART | `/dev/ttyUSB0`, 115200/8N1；帳密 `root` / `oelinux123` |
| 測試檔 | `/data/test/`（影片、音訊） |

### ⚠️ 已知陷阱（詳見 `VERIFICATION-2026-09-14.md`）

| 項目 | 說明 |
|---|---|
| SSH 密碼登入 | 預設 `PermitRootLogin prohibit-password`，**`oelinux123` 無法用於 SSH**，須用金鑰 |
| 音訊環境變數 | 文件的 `XDG_RUNTIME_DIR=/run/user/root` 在 root 身分下不可用，改用 `PULSE_SERVER=unix:/run/pulse/native` |
| adb 權限 | uid 2000，讀不到 `/root`、`/etc/shadow` 等，需要 root 時用 SSH 或 `adb root` |
| 長指令貼上 | 從 markdown 複製多行指令易被截斷，建議寫成腳本或用 heredoc |
| **上電不會自己開機** | 接 12V 後板子無反應屬正常 —— PMIC 需 **Type-C 的 VBUS** 觸發 PON 才開機。別誤判為故障，詳見 `VERIFICATION-2026-09-14.md` §15 |

### ⚠️ IP 為 DHCP 配發，會變動

本文件撰寫期間板子 IP 由 `192.168.3.63` 變為 `192.168.3.80`，一度誤判為當機。
**連不上時先確認 IP**，不要直接假設板子故障：

```bash
ssh root@<新 IP> 'ip -br addr show eth0'    # 或從 UART 查
```

考慮在路由器設定 DHCP 保留，或改為靜態 IP，以免後續腳本反覆失效。

另注意：`/data/coredump` 累積了 **1226 個 `core.sensor_service.*`**（794 MB），為 sensor service 反覆崩潰所致，推測與板上缺少 camera/sensor 硬體有關。目前 `/data` 尚有 73 GB，不影響運作，但長時間測試前值得釐清。

---

## 8. 參考資料

- Ultralytics 深度估計：https://docs.ultralytics.com/tasks/depth
- Ultralytics LiteRT 匯出：https://docs.ultralytics.com/integrations/tflite/
- 板子完整驗證記錄：`VERIFICATION-2026-09-14.md`（同目錄，796 行）
- 轉換腳本與產出：`yolo-depth/`（`build_qnn.sh`、`artifacts/`、`qairt-2.32/`）
- 原廠文件：`Release_README_V00.00.03.pdf`、`qcs8550-flash-sop.html`
