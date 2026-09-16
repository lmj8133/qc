# START HERE —— YOLO26-depth on QCS8550

| | |
|---|---|
| 狀態 | ✅ **完成且可運作**。相機 → NPU → HDMI 即時執行中 |
| 日期 | 2026-09-15（本文與所有工作均已 commit） |
| 主機 | x86 Ubuntu 24.04，`/home/mjl/qc`（git，branch `master`） |
| 板子 | `root@192.168.3.67`（金鑰登入，`BatchMode=yes` 可用）—— **IP 會變，見〈IP 是 DHCP〉** |

> **這份文件是索引，不是教學。** 專案已有六份文件共約 2000 行，
> 本文的主要工作是告訴你「哪個問題該去看哪一份」——**不要從頭讀完它們**。

---

## 一分鐘摘要

Ultralytics YOLO26n-depth（單目深度估計）已轉成 QNN context binary，
在 QCS8550 的 Hexagon V73 NPU 上即時執行，深度圖經 HDMI 輸出。

看它跑起來，一行就夠：

```bash
cd /home/mjl/qc/yolo-depth/pipeline && ./run.sh 384 live
```

384px、相機與深度左右並排、跑到 Ctrl-C 為止、30 FPS
（並排顯示下 384 才跑得滿 30 FPS，理由見〈現況〉）。
`artifacts/` 內的 `.bin` 已建好，**不需要先跑轉換**。

前提：板子已開機（見〈板子須知〉）、HDMI 已接、USB 相機在 `/dev/video2`。

---

## 現況（已實測）

端到端 pipeline，300+ frames，已釘核心：

| size | preprocess | inference | 無畫面 FPS | 並排顯示 FPS | 瓶頸 |
|---|---|---|---|---|---|
| **384** ⬅ 展示建議 | ~7 ms | 11.3 ms | **30.1** | **30.0** | **相機** |
| **512** ⬅ 無畫面建議 | 7.79 ms | 19.4 ms | **30.1** | 25.7（`--depth-only` 29.5） | 顯示頻寬 |
| 640 | 11.6 ms | 30.9 ms | 22.4 | 20.3 | 算力 |
| 768 | ~15 ms | 42.0 ms | 16.5 | — | 算力 |

- **無畫面時 512 是甜蜜點**（已頂到相機上限）；**但要開並排顯示，用 384**：
  預設的左右並排會吃顯示頻寬，512px 並排只剩 25.7 FPS，384px 才維持 30.0 FPS。
  （`--depth-only` 時 512px 可回到 29.5 FPS。）
- 加上顯示後 640px 為 20.3 FPS（display 階段 8.26 ms）。
- 精度：與同尺寸 ONNX 參考值 corr **0.9997**、MAE 0.013 m（640px）。
  注意這衡量的是 **FP16 轉換誤差，不是深度絕對精度**。
- 329 個 op **全部**進 NPU，零 CPU fallback。
- 四種尺寸的 `.bin` 都已建好在 `yolo-depth/artifacts/`。

---

## 檔案地圖 —— 哪個問題看哪一份

**依你的問題挑一份，不要全部讀。**

| 你想問的 | 去看 | 行數 |
|---|---|---|
| 為什麼 `yolo export format=qnn` 不能用？模型怎麼轉？各層延遲？ | `HANDOVER-yolo26-depth.md` | ~530 |
| 板子怎麼燒錄、UART、SSH 金鑰、HDMI、音訊、相機、SDK 交叉編譯 | `VERIFICATION-2026-09-14.md` | ~915 |
| 怎麼把 858 MB 的 QAIRT 工具鏈從板子撈回來架好 | `yolo-depth/README.md` | 111 |
| 常駐推論為什麼是必要的？各尺寸純推論數字 | `yolo-depth/bench/README.md` | 35 |
| pipeline 怎麼用、有哪些 flag、為什麼不做 double-buffer | `yolo-depth/pipeline/README.md` | 284 |
| 相機／顯示／前處理的個別量測與量測陷阱 | `yolo-depth/pipeline/NOTES-capture-display.md` | 132 |

**值得直接跳過去看的四節**：`HANDOVER §6`（延遲拆解與避障取捨）、
`HANDOVER §7`（連線資訊與陷阱總表）、`VERIFICATION §15`（上電不開機）、
`pipeline/README.md` 的 `## CPU pinning is load-bearing`（5 倍效能差）。

**程式碼**：

| 檔案 | 行數 | 作用 |
|---|---|---|
| `yolo-depth/build_qnn.sh` | 68 | 模型轉換，PyTorch → ONNX → FP16 DLC → V73 binary |
| `yolo-depth/pipeline/depth_cam.c` | 948 | 即時 pipeline，單一 C 程式 |
| `yolo-depth/pipeline/run.sh` | 69 | 一鍵啟動（build + stage + run） |
| `yolo-depth/bench/qnn_bench.c` | 284 | 常駐推論參考實作 |

> `HANDOVER` 與 `VERIFICATION` 內標 ⛔ 的段落是**初版推測，實測後證實錯誤**，
> 刻意保留是為了讓你不要重走。看到 ⛔ 直接跳過，除非你正想做那件事。

---

## 環境重建 —— 7.2 GB 不在版控裡

Fresh clone 後這四樣都不存在。前三項列在 `yolo-depth/.gitignore`；
`.venv/` 則是單純未被追蹤（uv 自行產生，不在 ignore 規則內）：

| 目錄 | 大小 | 怎麼還原 | 必要性 |
|---|---|---|---|
| `yolo-depth/qairt-2.32/` | 858 MB | 從板子 scp（見下） | **只有要重新轉模型才需要** |
| `yolo-depth/.venv/` | 6.2 GB | `cd yolo-depth && uv sync` | 同上 |
| `yolo-depth/artifacts/` | 182 MB | `./build_qnn.sh <size>` | 跑 pipeline 需要 `.bin` |
| `yolo-depth/weights/` | 38 MB | curl（見下） | 同 `artifacts` |

**最重要的一句話**：只要「讓它跑起來」，你需要的只有 `artifacts/*.bin`（約 10.5 MB／個）。
`qairt-2.32/` 與 6.2 GB 的 `.venv` 僅在**重新轉模型**時才需要——
單獨備份那幾顆 `.bin`，遠比重建整條工具鏈便宜。

### 還原順序

```bash
# 1. weights（URL 記於 HANDOVER-yolo26-depth.md:314，實測今日仍 HTTP 200）
cd /home/mjl/qc/yolo-depth && mkdir -p weights
curl -sSL -o weights/yolo26n-depth.pt \
  https://github.com/ultralytics/assets/releases/download/v8.4.0/yolo26n-depth.pt

# 2. .venv —— 完全由已版控的 uv.lock 還原（72 套件，全部 hash-pinned 到 pypi.org，
#    不需要自訂 torch index）。已實測 `uv sync --dry-run` 回報 "Would make no changes"。
uv sync

# 3. qairt-2.32/ —— 完整步驟見 yolo-depth/README.md 的「Setup: staging the toolchain」
#    三個 scp 來源路徑今日已逐一確認仍存在於板上。

# 4. artifacts/
./build_qnn.sh 512          # 或 384 / 640 / 768
```

### ⚠️ `qairt-2.32/` 的兩個非顯而易見之處

1. **不要向 Qualcomm 索取 QAIRT SDK。** 你會拿到更新的版本，然後撞上版本閘（見坑 #1）。
   板子自己就帶著一套 **dormant 的 x86_64 轉換工具鏈**在 `/opt/qcom/qirp-sdk/`——
   它在 aarch64 板子上執行會回 `Exec format error`，因為它本來就是給 x86 主機用的。
   把它撈回來，產出才會被蓋上 2.32.0 的版本戳記。

2. **光 scp 三個目錄還不夠。** Ubuntu 24.04 上還要三件 user-level 的東西
   （CPython 3.10、用 `dpkg-deb -x` 解出來的 LLVM-18 libc++/libunwind、一個 3.10 venv），
   全部不需要 root。**完整可複製貼上的指令在 `yolo-depth/README.md`，
   這是最近才補上的（commit `a46c4cb`）——在那之前只有一句散文描述，沒有指令。**

驗證方式（需先設好 `LD_LIBRARY_PATH`，直接執行會找不到動態函式庫）：

```bash
cd yolo-depth/qairt-2.32
PY310LIB=$(ls -d ~/.local/share/uv/python/cpython-3.10.*/lib | head -1)
LD_LIBRARY_PATH=$PWD/libs/x86_64-linux-clang:$PWD/cxx/usr/lib/llvm-18/lib:$PY310LIB \
    ./x86_64-linux-clang/qnn-context-binary-generator --version
```

必須印出 `QNN SDK v2.32.0.250228225014_116386`（今日已實測相符）。
注意 glob 要寫 `cpython-3.10.*`——少了那個點會匹配到 symlink，
`env` 無法穿越而報出令人困惑的 `Permission denied`。
但真正的測試是 `./build_qnn.sh` 跑得過。

---

## 會靜默坑你的事

依「浪費時間的程度」排序。**前兩名完全不會報錯**，這正是它們排前面的原因。

### 1. QNN context binary 版本閘 —— `format="qnn"` 永遠不可能成功

- **症狀**：主機端轉換成功（7 秒、329 op 全進 NPU、零 fallback，看起來完美無缺），
  然後板子拒絕載入：
  ```
  <E> Using newer context binary on old SDK
  <E> Failed to create context from binary with err 0x1388
  ```
- **原因**：QNN context binary **只向後相容**。板上 runtime 是 2.32.0，
  而所有已發布的 `onnxruntime-qnn` x86_64 wheel 都綁 QAIRT ≥ 2.47。
  **降版 wheel 沒用**——最舊可用的仍是 2.47。這是設計行為，不是 bug。
- **解法**：用板子自帶的 x86 工具鏈轉，也就是 `build_qnn.sh` 做的事。
- **文件**：`HANDOVER §2`、`§5`；`yolo-depth/README.md`「Why not `yolo export format=qnn`」

### 2. `--config_file` 需要 `backend_extensions` 外層包裝 —— 最陰險

- **症狀**：**完全沒有錯誤**。generator exit 0、無警告，產出的 binary 在 V73 上
  正常載入、正常執行，然後**每個 pixel 都是同一個值**（4.441，std=0），
  換任何輸入都一樣。
- **原因**：`--config_file` 收到無法辨識的頂層結構時，**什麼都不套用**，
  預設落到 dsp arch 68 / O0 / vtcm 4 / soc 0。
- **哪個欄位決定正確性**（A/B 實測，非推測）：**device 層的 `dsp arch`**。
  `graph_names` / `O` / `vtcm` **只影響速度**。
- **解法**：兩層 JSON。`build_qnn.sh` 已內建守門——binary 不是 `"dsp arch": 73` 就 `exit 2`。
  目前 `artifacts/ctxinfo.json` 實測為 arch 73 / O3 / soc 43 / vtcm 8，正確。
- **已否證的推論**：曾懷疑是「舊 artifact 殘留」，但用 bare config 重建的 binary
  與原本壞掉那顆 **MD5 完全相同**——是確定性行為，不是殘留。
- **文件**：`HANDOVER §5`；`build_qnn.sh` 守門碼

### 3. big.LITTLE：不釘核心，吞吐量無聲崩掉

- **症狀**：沒有錯誤，就只是跑到該有速度的 ~65%。
- **原因**：3+4+1 異質架構（cpu0-2 @2.02 / cpu3-6 @2.80 / cpu7 @3.19 GHz），
  scheduler 會把迴圈漂到小核。同一段 resize：cpu0 **16.40 ms** vs cpu7 **3.32 ms**，
  **5 倍差距**，是本專案找到的最大單一調校槓桿。
- **實測**：640px 未釘 14.5 FPS → 釘 cpu7 **22.4 FPS**。
- **解法**：`depth_cam.c` 啟動時自行釘到最高頻核心（`--no-pin` 可關閉）。
- **二階效應**：此現象曾讓一次量測失真——`NOTES` 裡「0.75 ms 前處理」只量了
  已是正確尺寸 buffer 的 HWC→CHW，且剛好落在快核上。
- **文件**：`pipeline/README.md`「CPU pinning is load-bearing」

### 4. `XDG_RUNTIME_DIR` —— 同一個路徑，兩條相反的規則

**這一條特別容易被自己的經驗坑到：學會一邊的規則然後推廣，就會弄壞另一邊。**

| 用途 | `/run/user/root` | 正確做法 |
|---|---|---|
| **Wayland / waylandsink** | **必須設定** | `export XDG_RUNTIME_DIR=/run/user/root`（**不是** `/run/user/0`） |
| **PulseAudio（以 root）** | **必須不要設定** | `export PULSE_SERVER=unix:/run/pulse/native` |

原因：`/run/user/root` 這個目錄名字叫 root，但屬於 **uid 1000**。
Wayland socket 在那裡，所以顯示需要它；PulseAudio 跑 system mode，
以 root 指過去會被拒絕。`run.sh` 已內建正確的 Wayland 設定。

- **文件**：`VERIFICATION §6`「⚠️ `XDG_RUNTIME_DIR` 陷阱」

### 5-8. 其餘四個（症狀 → 原因 → 解法 → 文件）

| # | 症狀 | 原因與解法 | 文件 |
|---|---|---|---|
| 5 | 啟動報 `ERROR: VIDIOC_S_FMT: Device or resource busy` | 前一次 Ctrl-C 沒收乾淨，`depth_cam` 還佔著 `/dev/video2`。`run.sh` 已有三重保險（`ssh -tt` + 遠端 EXIT trap + 本地 pkill）；真殘留時手動清（見下） | `pipeline/README.md`「Stopping it」 |
| 6 | capture 時間趨近 0（640px 報 0.04 ms，512px 卻是 4.63 ms） | **不是變快，是掉幀**——pipeline 比相機慢，buffer 永遠已填好在等，驅動正在丟幀 | `NOTES-capture-display.md` |
| 7 | `No such file or directory`，但檔名長得像指令片段 | 從 markdown 貼多行指令被截斷。**這個錯誤訊息會誤導你去找不存在的檔案**。解法：寫成腳本再執行，或整條貼成一行 | `VERIFICATION §9`（該次驗證重複發生四次，最耗時的單一問題） |
| 8 | `which $CC` 指向 `/usr/bin` 而非 SDK，看起來像環境沒設好 | **正常**。此版 SDK 只給 target sysroot，編譯器用系統的 `aarch64-linux-gnu-gcc`。判斷請用 `type -a` 而非 `which` | `VERIFICATION §8`「⚠️ 與 SDK README 不符之處」 |

殘留 process 手動清除：

```bash
ssh root@192.168.3.67 'pkill -x depth_cam; pkill -f gst-launch-1.0'
```

---

## 下一步（刻意未做的事，以及為什麼）

**最重要的前提：這個平台最終用於避障，所以延遲比幀率重要。**
避障關心的是「進光 → 決策」的時間，不是每秒幾張。
**「更快但更晚到」對這個應用是退步。**

| 項目 | 狀態 | 理由 |
|---|---|---|
| **double-buffer / pipelining** | **刻意否決，非待辦** | 能把 640px 從 22.4 推到 ~32 FPS，但每幀多等一輪。吞吐量↑、**單幀延遲↑**。且 512px 已頂到相機上限，做了也無增益 |
| 端到端延遲量測 | **未做，且是最該做的一項** | 目前只量各階段耗時與吞吐量。「進光→決策」還包含相機曝光、USB 傳輸、驅動緩衝，全未納入量測 |
| 完整精度評估 | 未做 | 只比對 2 張圖。要對照官方 Delta1 0.882 / RMSE 0.414m 需跑 NYU val 654 張 |
| 640px 達 30 FPS | 不打算做 | 需 double-buffer 或把 resize 移到 GPU，違反上述取捨 |
| 量化（INT8） | 未評估 | 目前 FP16 已達即時，且 FP16 精度 corr 0.9997 |
| `--perf_profile` 調校 | **此路不通（已實測）** | 五種設定差異 **<0.05%**。NPU 本來就跑滿，不存在 DVFS 節流 |

若要優化，**量的是端到端延遲，不是 FPS**。

---

## 板子須知

| 項目 | 值 |
|---|---|
| SSH | `root@192.168.3.67`（**僅金鑰**，`oelinux123` 不能用於 SSH） |
| UART | `/dev/ttyUSB0`, 115200/8N1，帳密 `root` / `oelinux123` |
| 相機 | `/dev/video2`（USB UVC，640x480 YUYV，30 fps 上限） |
| 板上工作區 | pipeline 用 `/dev/shm`（tmpfs，**重開機即失**）；`/` 可寫且重開存活（`/dev/sda2`，70G 可用） |
| Wayland | `XDG_RUNTIME_DIR=/run/user/root`、`WAYLAND_DISPLAY=wayland-1` |

**⛔ 硬規則：絕不燒錄、抹除、重開機，或改動板子的 `/opt` 與非揮發性設定。**

### 上電後不會自己開機 —— 這是正常的

接上 12V 後板子**毫無反應是正常的**。QCS8550 的 PMIC 沿用手機平台行為，
把 **Type-C 的 VBUS 當成合法開機事件源**——Type-C 才是實際的「電源鍵」。

**順序：先接 12V，再接 Type-C。** 插上 USB 仍無反應時，才需要懷疑硬體。
詳見 `VERIFICATION §15`。

### IP 是 DHCP，會變動

IP 已變動三次：`192.168.3.63` → `.80` → `.67`（2026-09-16）。
**每一次都被誤判為當機**，包括最後這次。
**連不上時先找出新 IP**，不要先懷疑板子壞了：

> ⚠️ **陷阱：ARP 快取會騙你。** 舊 IP 的 `ip neigh show` 仍會顯示
> `REACHABLE` 並掛著板子的 MAC（`a0:36:bc:3c:ab:10`），但 SSH 與 ping 都 timeout
> —— 看起來完全像 kernel hang。**不要拿 ARP 狀態判斷板子生死。**
>
> 另外：IP 變動若伴隨重開機，`/dev/shm`（tmpfs）裡 staged 的 `.bin` 也沒了。
> `run.sh` 每次都會重新 stage，但手動跑 `depth_cam` 不會。
>
> `yolo-depth/pipeline/board.env` 是 `build.sh` / `run.sh` 共用的預設值，改一處即可。

```bash
# 1. 掃網段找開著 22 port 的機器（純 bash，不需安裝任何東西；已實測可找到板子）
for i in $(seq 1 254); do
    (timeout 1 bash -c "echo > /dev/tcp/192.168.3.$i/22" 2>/dev/null \
        && echo "192.168.3.$i") &
done; wait

# 2. 確認哪一台是板子
ssh root@<候選 IP> 'cat /etc/hostname'      # 應為 kalama

# 3. 網路完全不通時，從 UART 問（唯一可靠的方法）
#    /dev/ttyUSB0, 115200/8N1, 帳密 root / oelinux123
#    登入後: ip -br addr show eth0
```

> ⚠️ `HANDOVER §7` 給的是 `ssh root@<新 IP> 'ip -br addr show eth0'`——那是循環論證
> （要先知道 IP 才能連）。IP 未知時請用上面兩種方式。

找到後用 `BOARD=<ip> ./run.sh` 覆寫，或直接改 `run.sh` 的預設值。

### 其他

- **RTC 無電池**：每次開機時間歸零到 1970，`journalctl --list-boots` 只看得到本次開機；
  要查前幾次開機原因只能靠 `/sys/kernel/debug/ipc_logging/pmic_pon/log`。
- `/data/coredump` 持續累積 `core.sensor_service.*`（**2026-09-15 實測 4013 個 / 2.6 GB**，
  `VERIFICATION` 當時記錄的是 1226 個 / 794 MB——它還在長），推測與板上缺少
  camera/sensor 硬體有關。`/data` 尚有 73 GB，不影響運作。

---

## 本文件的誠實聲明

**MEASURED（撰寫時實際執行確認）**：git 狀態與 commit 歷史、`artifacts/` 內容與
`ctxinfo.json` 欄位、四個目錄大小、`uv sync --dry-run`、板子 SSH 可連且 `/dev/video2`
與 `/run/user/root` 存在、weights URL 回 HTTP 200、`run.sh` 與 `build_qnn.sh` 的所有
flag、各文件行數。

**REPORTED（引用既有文件，本次未重新量測）**：所有效能數字（FPS、延遲、相關係數）、
錯誤訊息原文、big.LITTLE 各核心耗時。均來自上述文件中標記為實測的記錄。

**數字對不上時以哪份為準**：本文件是索引，效能數字轉引自各專門文件。
若發現不一致，**以專門文件為準**（`pipeline/README.md` 管 pipeline、
`bench/README.md` 管純推論、`HANDOVER §6` 管轉換與延遲拆解），
並請回報這裡，索引過期比專門文件過期更危險。

**已知的文件缺口**：

- **端到端「進光→決策」延遲從未被量測過**，這是避障目標下最重要的缺口。
- `qairt-2.32/` 的完整重建指令直到 commit `a46c4cb` 才寫下來。
  若你的 clone 早於該 commit，`yolo-depth/README.md` 只有一句散文描述，沒有指令。
- 精度只比對 2 張圖，無完整 NYU val 評估。
- USB VBUS 開機是否只需一般充電器（而非筆電）**尚未實測**。
