# 交接文件：YOLO26-depth 於 QCS8550 的可行性驗證

| | |
|---|---|
| 日期 | 2026-09-15 |
| 目標 | 在 QCS8550 (aida3_RDK_SOCKET) 上執行 Ultralytics YOLO26-depth |
| 執行環境 | 模型轉換在 **x86 server**；推論在板子 |
| 前置狀態 | 板子已完成 bring-up，見 `VERIFICATION-2026-09-14.md` |

---

## 1. 目標與結論預告

**結論：技術上可行，但尚未經證實，有三個未知數需實測。**

**板上已有完整的 QNN runtime（QNN SDK v2.32.0），且 Hexagon V73 NPU 經實測確認可用**，因此 **QNN 應為優先路徑**，而非 TFLite。詳見 §2、§4。

不要直接搬 YOLO26-depth 上板。先驗證 runtime 基礎（§5 階段一），再處理模型轉換。

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

### ⚠️ 重要：context binary 可在板上編譯

`qnn-context-binary-generator` **存在於板子上**，因此建議：

1. server 端只產出**未編譯的 QNN model**（`.so` 或 `.cpp`+`.bin`）
2. 在板子上編譯成 HTP context binary

**此法避開「server 端須指定正確 SoC ID 與 Hexagon 版本」的風險** —— 在目標機器上編譯，版本必然正確。

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

### ⚠️ 版本相容性

板上 QNN SDK 為 **v2.32.0**。**server 端的 QAIRT 轉換工具版本應與之相符** —— 版本落差過大可能產生板子無法載入的模型格式。這是開始轉換前要確認的第一件事。

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

## 4. 兩條可能路徑

Ultralytics 匯出格式中，與本平台相關的有兩條：

| 路徑 | 板上 runtime | 評估 |
|---|---|---|
| **Qualcomm QNN** ⬅ 優先 | `/opt/qcom/qirp-sdk`（**已在板上**，含 HTP/GPU/DSP/CPU backend） | 專為 Qualcomm 硬體設計。**HTP backend 可利用 NPU**，效能遠高於 TFLite GPU delegate |
| **LiteRT** (`.tflite`) | `libtensorflowlite_c.so` **2.11.1** | 工具鏈較單純，但 runtime 版本偏舊；depth task 支援未明載 |

**建議 QNN 優先。** 板上 runtime 已就緒，server 端仍需安裝 Qualcomm AI Engine Direct SDK（QAIRT）做模型轉換 —— 該 SDK 獨立於本專案的 application SDK，須另行取得。

TFLite 路徑若要用上 NPU，需透過 `--external_delegate_path=<QNN TFLite delegate>` 掛載 QNN delegate（`benchmark_model` 無內建的 `use_hexagon`/`use_nnapi` 旗標）。

### ⚠️ 已知風險

Ultralytics 的 TFLite/LiteRT 匯出文件**只明確提到 detection 與 classification**，未載明 depth task 的支援狀況。depth 是較新的 task，轉換時可能遇到不支援的 op（深度估計的 decoder 常含 upsample/interpolate 這類在量化或 delegate 上表現不佳的運算）。

**這需要實測，不能從文件推斷。**

---

## 5. 建議執行順序

### 階段一：驗證 TFLite runtime 基礎 ⬅ 先做這個

**不要跳過。** 用已知能動的小模型排除 runtime 本身的問題，否則之後 YOLO 失敗時無法分辨是模型問題還是環境問題。

```bash
# server 端：取得一個標準 TFLite 模型
# 例如 MobileNet v1/v2 quant，或任何已知可用的 .tflite

scp mobilenet_v1_1.0_224_quant.tflite root@192.168.3.80:/data/
```

板子端：

```bash
# 1. runtime 能否載入並推論
benchmark_model --graph=/data/mobilenet_v1_1.0_224_quant.tflite --num_runs=50

# 2. delegate 是否真的生效 —— 比對延遲差異
benchmark_model --graph=/data/mobilenet_v1_1.0_224_quant.tflite --num_runs=50 --use_gpu=false
benchmark_model --graph=/data/mobilenet_v1_1.0_224_quant.tflite --num_runs=50 --use_gpu=true
benchmark_model --graph=/data/mobilenet_v1_1.0_224_quant.tflite --num_runs=50 --use_xnnpack=true
```

**判讀**：若 `--use_gpu=true` 與 `false` 的延遲差異不明顯，表示 **delegate 沒有真正掛上**，而非模型太小。這是必須先解決的問題 —— 記得看輸出中 delegate 實際接管了幾個 node。

`label_image` 額外需要一張 **BMP** 格式測試圖（`-i` 參數註明 `image_name.bmp`）。

### 階段二：YOLO26n-depth 匯出（先用最小變體）

server 端環境：

```bash
uv init yolo-depth && cd yolo-depth
uv add ultralytics
```

TFLite 路徑：

```python
from ultralytics import YOLO

model = YOLO("yolo26n-depth.pt")
model.export(format="litert", imgsz=768)          # FP32 先試
# model.export(format="litert", imgsz=768, quantize=8, data="<dataset.yaml>")  # INT8 需校準資料
```

> 注意：`format="tflite"` 已棄用，改用 `format="litert"`，產出同樣是 `.tflite`。

QNN 路徑（**建議優先**）：

```python
model.export(format="qnn")    # 需先在 server 安裝 Qualcomm AI Engine Direct SDK (QAIRT)
```

匯出後推到板子，執行前先設定環境：

```bash
. /opt/qcom/qirp-sdk/qirp-setup.sh    # PATH / LD_LIBRARY_PATH / ADSP_LIBRARY_PATH
```

backend 選擇 `libQnnHtp.so`（NPU）。若 HTP 無法載入模型，依序退回 `libQnnGpu.so`、`libQnnCpu.so` 以定位問題層級。

**匯出失敗時**，錯誤訊息通常會指出哪個 op 不支援 —— 記錄下來，那是判斷可行性的關鍵資訊，不要只回報「轉換失敗」。

### 階段三：板上實測

```bash
scp yolo26n-depth.tflite root@192.168.3.80:/data/

ssh root@192.168.3.80 '
  benchmark_model --graph=/data/yolo26n-depth.tflite --num_runs=20 --use_gpu=false
  benchmark_model --graph=/data/yolo26n-depth.tflite --num_runs=20 --use_gpu=true
'
```

QNN 路徑的量測需先 source `qirp-setup.sh`，並分別以 HTP / GPU / CPU backend 執行以比較。

量測項目：
- 單張推論延遲（CPU vs GPU vs HTP/NPU）
- delegate 接管的 node 比例 —— **YOLO 類模型常有部分 op fallback 回 CPU，實際加速比可能遠低於預期**
- 記憶體佔用

### 階段四：端到端 pipeline（選用）

板上有 USB camera（AVerMedia PW310P，`/dev/video2`）可作輸入源，已驗證可用：

```bash
gst-launch-1.0 v4l2src device=/dev/video2 ! image/jpeg,width=1280,height=720 ! jpegdec ! ...
```

板載 MIPI camera **無模組，不可用**。

---

## 6. 三個未知數（需實測回答）

| # | 問題 | 如何判定 |
|---|---|---|
| 1 | YOLO26-depth 能否成功匯出為 QNN / TFLite？ | 階段二。失敗時記錄具體不支援的 op |
| 2 | HTP（NPU）能接管多少比例的 op？ | 階段三。QNN 的 graph prepare 階段會報告 fallback 情形 |
| 3 | 實際延遲是否符合應用需求？ | 階段三實測，**不可用 T4 的 2.7ms 推估** |

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
- 原廠文件：`Release_README_V00.00.03.pdf`、`qcs8550-flash-sop.html`
