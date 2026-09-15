# QCS8550 燒錄與外設驗證記錄

| | |
|---|---|
| 日期 | 2026-09-14 |
| 板子 | aida3_RDK_SOCKET（SN: 6B22BE40） |
| 映像 | qcs8550_ubuntu_flatten_V00.00.03 |
| 主機 | Ubuntu 24.04.4 LTS x86_64 |
| 依據文件 | SOP-QCS8550-01、Release_README_V00.00.03 |

每項驗證含：**驗什麼 → 怎麼驗 → 結果 → 卡住時怎麼解**。

---

## 總表

| § | 項目 | 驗什麼 | 結果 | 關鍵註記 |
|---|---|---|---|---|
| 0 | 主機端前置設定 | 工具、udev 規則、群組權限 | ✅ | `usermod` 需重新登入才生效 |
| 1 | 燒錄 | flatten 映像寫入 UFS | ✅ | 1m09.856s，802 partitions，0 錯誤 |
| 2 | 燒錄後驗證 | 版本正確且與 SDK 同源 | ✅ | kernel hash `g576728c775df` 吻合 |
| 3 | UART console | 序列埠可觀察 kernel log | ✅ | `/dev/ttyUSB0`、115200/8N1 |
| 4 | HDMI 顯示 | 顯示輸出 | ✅ | SW1701 pin 8 = ON |
| 5 | 影片播放 | H.264 硬解 + waylandsink | ✅ | 720p / 1080p / 4K 皆通過 |
| 6 | 音訊 | playback 與 capture 路徑 | ✅ | ⚠️ 需 `PULSE_SERVER`，非文件寫的 `XDG_RUNTIME_DIR` |
| 7 | SSH 登入 | 網路登入板子 | ✅ | ⚠️ 預設擋 root 密碼登入，本次改用金鑰 |
| 8 | Application SDK | 交叉編譯至板上執行 | ✅ | 版本 hash 三方一致；SDK 不自帶編譯器 |
| 9 | 長指令貼上截斷 | （非驗證項，除錯指南） | — | 本次最耗時的問題，共發生 4 次 |
| 10 | 板載 Camera ×5 | MIPI camera preview | ⚠️ 未測 | 板上無 module；且 DT sensor 型號與文件不符 |
| 11 | USB Camera | UVC 擷取與 HDMI 預覽 | ✅ | AVerMedia PW310P；反證影像鏈路正常 |
| 15 | 開機行為 | 上電後為何不自己開機 | ⚠️ 設計行為 | **需插 USB 觸發 PON**；非故障，見 §15 |

**環境**

| | |
|---|---|
| OS | Ubuntu 22.04.2 LTS (Jammy) |
| kernel | `5.15.170-qki-consolidate-android13-8-00002-g576728c775df-dirty` |
| adb product | `kalama-qti-distro-ubuntu-fullstack-debug` |
| 音訊 | card `pal.audio.primary`；sink `low-latency0` / `offload0`；source `regular0` |
| 網路 | eth0 `192.168.3.63/24`（DHCP）；wlan0 為 DOWN |

**未完成**（詳見 §12）：Camera、麥克風、AI — 前兩項待硬體到貨，AI 缺模型檔。

**應回報文件維護者**（詳見 §13）：§6 音訊環境變數（高）、§7 SSH 密碼登入（高）、§10 sensor 型號不符（中）、§8 SDK README 兩處（低）、燒錄耗時差異（低）。

---

## 0. 主機端前置設定

**驗什麼**：主機具備燒錄與連線所需的工具和權限。

**怎麼驗**

```bash
sudo apt install -y p7zip-full android-tools-adb picocom

sudo tee /etc/udev/rules.d/51-qualcomm-edl.rules <<'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="05c6", MODE="0660", GROUP="plugdev"
EOF
sudo udevadm control --reload-rules && sudo udevadm trigger

sudo usermod -aG dialout $USER    # UART 需要；plugdev 供 qdl raw USB
```

確認：

```bash
cat /etc/udev/rules.d/51-qualcomm-edl.rules   # 應只有一行
id -nG | tr ' ' '\n' | grep -E 'plugdev|dialout'
```

**結果**：✅ 通過。

**問題與解法**

| 症狀 | 原因 | 解法 |
|---|---|---|
| `id -nG` 看不到剛加的群組 | `usermod` 對已存在的工作階段無效 | 重啟 Ubuntu ；或 `newgrp dialout` 開新 shell（僅該 shell 有效） |
| heredoc 把後續指令一起寫進檔案 | `EOF` 結束標記前面有空白 | 結束標記必須頂格、獨占一行。或改用單行：<br>`echo '...' \| sudo tee /etc/udev/rules.d/51-qualcomm-edl.rules` |
| 規則建立後權限仍不足 | 板子在建立規則前就已插著 | 拔掉 USB 重插（`udevadm trigger` 對已列舉裝置不一定重設權限） |

---

## 1. 燒錄

**驗什麼**：flatten 映像完整寫入 UFS，板子能開進新系統。

**怎麼驗**

```bash
cd /home/agmis/Documents/qc
7z x qcs8550_ubuntu_flatten_V00.00.03.7z
cd flatten_image

lsusb | grep -i qualcomm                                          # 確認板子在線
grep 'label="cdt"' rawprogram3.xml | grep -o 'filename="[^"]*"'   # 必須是 filename=""
./qdl_flash.sh rdk 2>&1 | tee flash.log
```

`rdk` 對應 aida3_RDK_SOCKET（Board ID 0x03）。**不可用 `grizzly`**。

燒錄期間不可拔線、斷電或讓主機睡眠。

**結果**：✅ 通過，耗時 1m 09.856s。

| 檢查項 | 預期 | 實際 |
|---|---|---|
| log 結尾 | `[Step 5] Flashing completed!` | 相同 |
| error/fail/abort | 0 | 0 |
| log 行數 | ~818 | 819 |
| partition 寫入 | — | 802 筆（`system`×760、`PrimaryGPT`×6、`BackupGPT`×6、`xbl_ramdump_a` 等） |

**問題與解法**

| 症狀 | 原因 | 解法 |
|---|---|---|
| `lsusb` 看不到裝置 | 沒供電、USB 埠錯、充電線 | 先接 12V 再接 Type-C。確認 `lsusb` 有 `05c6` 才往下 |
| qdl `Permission denied` | udev 規則或群組未生效 | 回 §0 |
| 卡在 `waiting for programmer...` | 進了 EDL 但未接受 programmer | 板子斷電重來，等它回到 EDL 再重跑 |
| 中途失敗 | — | 板子多半仍在 EDL（`05c6:9008`）。清掉 `rawprogram3.xml` 的殘留 CDT 檔名，重跑同一行 |
| 完全不上 USB | — | 硬體強制 EDL：**SW1701 Pin 1 撥 ON** → 接 12V → 接 Type-C → 燒錄 |

> 燒錄走 EDL，該協定在 SoC 的 mask ROM 內，寫入儲存永遠無法破壞它 —— 燒失敗的板子一定救得回來。

---

## 2. 燒錄後驗證

**驗什麼**：板子跑的是預期版本，且與 application SDK 同源。

**怎麼驗**

主機端：

```bash
lsusb | grep -i qualcomm    # 應回到 05c6:901d
adb devices -l

cd flatten_image
tail -3 flash.log
grep -icE 'error|fail|abort' flash.log
grep 'label="cdt"' rawprogram3.xml | grep -o 'filename="[^"]*"'   # 應還原成 ""
```

板子端（UART 或 `adb shell`）：

```bash
lsb_release -a
uname -a
```

**結果**：✅ 八項全過。

| 檢查項 | 預期 | 實際 |
|---|---|---|
| USB | `05c6:901d` | 相同 |
| adb product | `kalama-qti-distro-ubuntu-fullstack-debug` | 相同 |
| OS | Ubuntu 22.04.2 LTS (Jammy) | 相同 |
| kernel | 5.15.170 … `g576728c775df` | `5.15.170-qki-consolidate-android13-8-00002-g576728c775df-dirty` |
| CDT 還原 | `filename=""` | 相同 |

kernel hash `576728c775df` 與 Release Notes 的 `Pega_Build_Commit: 576728c775dfa24956eb041f8ea23242f468a6aa` 前 12 碼一致 → 映像與 SDK 同源，交叉編譯的執行檔可正確連結板上函式庫。**這項不符就不要開始寫應用程式。**

**問題與解法**

| 症狀 | 原因 | 解法 |
|---|---|---|
| 燒完沒重新列舉 | 首次開機慢 | **先等滿一分鐘**，再斷電重來。仍無反應走 §1 的硬體 EDL |
| `adb devices` 空的 | udev 規則缺失 | 回 §0 |
| 板子不會自己重開？ | 不需要 | 正常燒完會自動重開（約 20 秒）。**不必拔 DC jack** |

---

## 3. UART console

**驗什麼**：序列埠可觀察開機過程與 kernel log。

**怎麼驗**

接線：RDK CON1301，USB2.0（USBA-MicroUSB）。

```bash
ls -l /dev/ttyUSB*                  # FTDI FT232 會出現為 /dev/ttyUSB0
picocom -b 115200 /dev/ttyUSB0      # 8N1、no flow control 為預設值
```

接上後按幾次 Enter。離開：`Ctrl+A` 然後 `Ctrl+X`。

存 log：`picocom -b 115200 --logfile ~/uart-$(date +%H%M).log /dev/ttyUSB0`

**結果**：✅ 通過。

**問題與解法**

| 症狀 | 原因 | 解法 |
|---|---|---|
| `Permission denied` | 不在 `dialout` 群組 | 回 §0 |
| 不用輸入帳密，prompt 是 `console:/ $` | 這是**燒錄前**的 Android-style 映像，debug console 直接開 shell | 正常。燒錄後改為 Ubuntu，會出現 `login:`，帳密 `root` / `oelinux123` |
| 貼上長指令被截斷 | 見 §9 | 見 §9 |

> 建議燒錄時開兩個視窗：一個跑 `qdl_flash.sh`，一個掛 picocom。燒錄期間 UART 會安靜一大段時間（板子在 EDL），這是正常的，不是當機。

---

## 4. HDMI 顯示

**驗什麼**：顯示輸出正常。

**怎麼驗**：SW1701 **pin 8 撥 ON** → 接 HDMI 線 → `adb reboot`。

**結果**：✅ 通過。後續影片播放與 camera preview 皆依賴此項。

---

## 5. 影片播放（H.264 硬解）

**驗什麼**：`qtic2vdec` 硬體解碼 + `waylandsink` 輸出到 HDMI。

**怎麼驗**（板子端，需先通過 §4）

```bash
export WAYLAND_DISPLAY=wayland-1
export XDG_RUNTIME_DIR=/run/user/root

gst-launch-1.0 filesrc location=/data/test/720p.mp4 ! qtdemux ! h264parse ! qtic2vdec ! video/x-raw\(memory:GBM\) ! waylandsink fullscreen=true enable-last-sample=false
```

換 `/data/test/1080p.mp4`、`/data/test/4k.mp4` 各測一次。

**結果**：✅ 720p / 1080p / 4K 全部通過。

**問題與解法**

| 症狀 | 原因 | 解法 |
|---|---|---|
| `syntax error` + `No such file or directory` | 指令被換行截斷 | 見 §9 |
| 括號報錯 | `(memory:GBM)` 未跳脫 | 寫成 `video/x-raw\(memory:GBM\)` |

> `waylandsink` 走 `/dev/dri/card0`，**不檢查 `XDG_RUNTIME_DIR` 的擁有權**，因此不受 §6 的問題影響。

---

## 6. 音訊

**驗什麼**：playback 與 capture 路徑。

**怎麼驗**（板子端）

```bash
export PULSE_SERVER=unix:/run/pulse/native    # 見下方「問題與解法」，這行是必要的

pactl list sinks short
pactl set-sink-port 1 speaker                 # 或 headset

# 播放
paplay -v /data/test/1khz.wav
gst-launch-1.0 filesrc location=/data/test/1khz.wav ! wavparse ! audioconvert ! pulsesink volume=0.5
gst-launch-1.0 filesrc location=/data/test/1khz_320k_48000.mp3 ! mpegaudioparse ! mpg123audiodec ! pulsesink volume=0.5

# 錄音（Ctrl+C 停止）
parec -v --rate=48000 --format=s16le --channels=1 --file-format=wav /tmp/rec.wav
paplay -v /tmp/rec.wav
```

**結果**：✅ 播放（WAV/MP3）通過；✅ 錄音路徑通過，**因板上無麥克風硬體，錄得無聲但檔案正常**。

裝置：card `pal.audio.primary`，sink `low-latency0` / `offload0`，source `regular0`。

**問題與解法**

| 症狀 | 原因 | 解法 |
|---|---|---|
| `XDG_RUNTIME_DIR (/run/user/root) is not owned by us (uid 0), but by uid 1000!` | **文件的音訊指令在 root 身分下不可用** — 見下方說明 | `export PULSE_SERVER=unix:/run/pulse/native` |
| `Failed to create secure directory (/home/adb/.config/pulse)` | 以 `adb shell` 執行，`adb` 帳號無家目錄 | 改在 UART 的 root shell 執行 |
| `option '--channel=1' is ambiguous` | 少了 s | `--channels=1` |
| `Invalid sample specification` | `s161e` 打成數字 1 | `s16le`（小寫 L）。該值為預設，也可整個省略 |
| `Too many arguments` | `==channels=1` 打成兩個等號 | `--channels=1` |

### ⚠️ `XDG_RUNTIME_DIR` 陷阱

Release Notes 的音訊章節要求 `export XDG_RUNTIME_DIR=/run/user/root`，但板子上該目錄屬於 **uid 1000（`system`）**，不是 root：

```
/run/user/0     drwx------  0     0     ← 真正 root 的 runtime dir
/run/user/root  drwxrwx---  1000  44    ← 名為 "root" 但屬於 uid 1000
```

以 root 執行 `pactl` / `paplay` / `parec` 會被 PulseAudio 拒絕。

PulseAudio 跑在 system mode（`/usr/bin/pulseaudio --system`），socket 在 `/run/pulse/native` 且權限 `srwxrwxrwx`，因此**不需要** `XDG_RUNTIME_DIR`，直接指定 server 即可。

**照文件以 root 操作音訊必定踩到這一點，應回報文件維護者。**

---

## 7. SSH 登入

**驗什麼**：可透過網路以 SSH 登入板子。

**怎麼驗**

板子端先確認網路與服務：

```bash
ip -br addr                  # eth0 應有 IP
systemctl status ssh
```

主機端：

```bash
ping <板子 IP>
ssh root@<板子 IP>
```

**結果**：✅ 通過（金鑰登入）。本次板子 IP `192.168.3.63`（eth0，DHCP）。

### ⚠️ 預設擋掉 root 密碼登入

Release Notes 提供帳密 `root` / `oelinux123`，但**該密碼無法用於 SSH**。板子的 `/etc/ssh/sshd_config` 中：

```
#PermitRootLogin prohibit-password
```

此行被註解，因此套用 OpenSSH 內建預設值 —— 正好就是 `prohibit-password`：**允許 root 以金鑰登入，拒絕密碼登入**。`/etc/ssh/sshd_config.d/` 為空，無其他覆寫。

症狀為輸入正確密碼後仍得到：

```
Permission denied, please try again.
```

**這不是密碼錯誤。** UART 可用同一組帳密登入，SSH 不行。

### 解法 A：改用金鑰（建議，本次採用）

主機端產生金鑰並取得公鑰：

```bash
ssh-keygen -t ed25519 -N '' -f ~/.ssh/id_ed25519    # 已有金鑰則跳過
cat ~/.ssh/id_ed25519.pub                            # 複製整行
```

板子端（UART）寫入：

```bash
mkdir -p /root/.ssh && chmod 700 /root/.ssh
echo '貼上公鑰整行' > /root/.ssh/authorized_keys
chmod 600 /root/.ssh/authorized_keys
wc -l /root/.ssh/authorized_keys    # 應為 1，確認未被截斷
```

此法無須更動 sshd 設定 —— `prohibit-password` 本來就允許金鑰登入。

### 解法 B：開放密碼登入（快，但安全性較低）

板子端：

```bash
echo 'PermitRootLogin yes' > /etc/ssh/sshd_config.d/99-local.conf
systemctl restart ssh
```

用獨立檔案而非直接改 `sshd_config`，移除時只需刪檔。

> **選擇建議**：內網開發板用 B 即可（`oelinux123` 是公開的原廠預設密碼，安全邊際本就有限）。但若板子會接公司網路或有外部存取，務必用 A —— 預設密碼加開放 root 密碼登入是容易被掃描到的組合。

**問題與解法**

| 症狀 | 原因 | 解法 |
|---|---|---|
| 輸入正確密碼仍 `Permission denied` | `PermitRootLogin prohibit-password`（預設值） | 見解法 A 或 B |
| `REMOTE HOST IDENTIFICATION HAS CHANGED` | 燒錄後板子 host key 改變 | `ssh-keygen -R <板子 IP>` |
| `mkdir: cannot create directory '/root': Permission denied` | `adb shell` 以 uid 2000 執行 | 先 `adb root`，或改在 UART 操作 |
| `cat: /root/.ssh/id_ed25519.pub: No such file` | 在**板子**上找主機的金鑰 | 金鑰在主機端產生，板子只放公鑰 |
| 設定金鑰後所有機器都無法登入 | 用 `>` 覆蓋了 `authorized_keys` | 見下方「佈署金鑰時的兩個陷阱」；改用 `ssh-copy-id` |
| 金鑰已加入卻仍 `Permission denied` | 貼成私鑰、內容遭截斷，或權限過寬 | 用 `awk` 檢查欄位數應為 3；`chmod 700 /root/.ssh && chmod 600 authorized_keys` |
| `setlocale: LC_ALL: cannot change locale` | SSH 帶入主機 locale，板上未安裝 | 警告非錯誤，可忽略 |

### ⚠️ 佈署金鑰時的兩個陷阱（本次皆實際踩到）

**陷阱一：`>` 覆蓋既有金鑰**

```bash
echo '<公鑰>' >  /root/.ssh/authorized_keys    # ✗ 覆蓋，原有金鑰全失
echo '<公鑰>' >> /root/.ssh/authorized_keys    # ✓ 追加
```

一字之差導致**所有既有機器同時被鎖在外面**，只能改由 UART 修復。

**陷阱二：貼成私鑰而非公鑰** 🔴 安全事故

| 檔案 | 內容開頭 | 用途 |
|---|---|---|
| `id_ed25519` | `-----BEGIN OPENSSH PRIVATE KEY-----`<br>（去掉包裝後為 `b3BlbnNzaC1rZXktdjEA...`） | **私鑰，永不離開本機** |
| `id_ed25519.pub` | `ssh-ed25519 AAAA...` | 公鑰，可自由散佈 |

本次誤將**兩台機器的私鑰**貼入 `authorized_keys`。後果有二：

1. SSH 無法運作 —— sshd 解析不了該格式
2. **私鑰明文留存於板子檔案系統**，構成外洩

處置：刪除檔案、將兩把金鑰視為已洩漏並重新產生、檢查並移除其在所有外部服務（Git 託管、其他伺服器、CI）上的授權。

**判別方式**：base64 開頭為 `b3BlbnNzaC1rZXktdjEA` 即 `openssh-key-v1`，是私鑰檔頭。

### 建議做法：一律使用 `ssh-copy-id`

```bash
ssh-copy-id root@<板子 IP>      # 每台來源機器各執行一次
```

自動選用 `.pub`、正確追加、建立目錄並設定權限。**上述兩個陷阱在此皆不存在。**

需佈署多把金鑰時，每台機器各跑一次即可 —— `authorized_keys` 一行一把，無數量限制。

### 驗證與權限

```bash
awk '{print NR": "$1" | 欄位數="NF" | "$NF}' /root/.ssh/authorized_keys
stat -c '%a %n' /root/.ssh /root/.ssh/authorized_keys
```

每行應為：欄位數 **3**、首欄 `ssh-ed25519`（或 `ssh-rsa`）、末欄為註解。**欄位數非 3 即遭截斷。**

權限須為 `.ssh` **700**、`authorized_keys` **600** —— 過於寬鬆時 sshd 會靜默忽略整個檔案。

> 註解欄建議寫明來源機器（`agmis@workstation`、`ci@buildserver`），日後撤銷單一機器權限時可用 `grep -v` 精準移除。

### 備註：板上內建一對金鑰

`/root/.ssh/` 內含映像預載的 `id_ed25519` / `id_ed25519.pub`（時間戳 1980，非使用者產生）。
**燒錄同一映像的所有板子共用這把私鑰**，不具身分唯一性。該金鑰供板子對外連線時使用，與登入板子無關；若需讓板子 SSH 至其他主機，應另行產生金鑰。

### SSH 相對 adb 的優勢

登入後為 uid 0（adb 為 uid 2000，讀不到 `/etc/shadow`、`/etc/ssh/sshd_config` 等）。另外 heredoc 可**從根本避開 §9 的貼上截斷問題** —— 整段以 stdin 餵給遠端 bash，不經終端機逐行解析：

```bash
ssh root@192.168.3.63 'bash -s' <<'EOF'
export PULSE_SERVER=unix:/run/pulse/native
paplay -v /data/test/1khz.wav
EOF
```

傳檔用 `scp hello root@192.168.3.63:/data/`。

---

## 8. Application SDK（交叉編譯）

**驗什麼**：交叉編譯環境可產出板子實際執行得動的 aarch64 執行檔。

驗證鏈五環，任一環斷則後續無意義。

### 安裝

```bash
cd /home/agmis/Documents/qc
7z x qcs8550_ubuntu_application_sdk_V00.00.03.7z     # 12s（壓縮方法為 Copy，等同搬檔）

sudo apt install -y gcc-aarch64-linux-gnu            # 必要前置，見下方說明
cd application_sdk
./ubuntu-fullstack-debug-x86_64-...-toolchain-02-2-g576728c775.sh -y -d /home/agmis/Documents/qc/sdk
```

`-y -d <dir>` 可非互動安裝，不必回答提示。耗時 4m36s。

安裝後產生：

```
sdk/
├── environment-setup-aarch64-oe-linux
├── environment-setup-aarch64-oe-linux-sdllvm
├── site-config-aarch64-oe-linux
├── sysroots/
└── version-aarch64-oe-linux
```

### 驗證步驟與結果

| # | 驗什麼 | 怎麼驗 | 結果 |
|---|---|---|---|
| 1 | SDK 安裝完成 | `ls sdk/` | ✅ 目錄結構如上 |
| 2 | **版本與板子相符** | `cat sdk/version-aarch64-oe-linux` | ✅ 見下 |
| 3 | 工具鏈可用 | `. environment-setup-aarch64-oe-linux; type -a ${CC%% *}` | ✅ |
| 4 | 產物架構正確 | `file hello` | ✅ `ELF 64-bit LSB pie, ARM aarch64` |
| 5 | **板上實際執行** | `scp` 後執行 | ✅ `Hello World!`、exit 0 |

**第 2 項是整條鏈的核心**：

```
SDK Metadata Revision: 576728c775dfa24956eb041f8ea23242f468a6aa
板子 kernel:            5.15.170-...-g576728c775df-dirty
Release Notes:          576728c775dfa24956eb041f8ea23242f468a6aa
```

三方一致 → 交叉編譯的執行檔可正確連結板上函式庫。**此項不符就不要開始開發。**

**第 5 項才是真正的驗證** —— 第 4 項的 `file` 只證明「編出來是 aarch64」，不證明「能在這塊板子上跑」。

### 編譯與部署

```bash
mkdir -p ~/Documents/qc/sdk-test && cd ~/Documents/qc/sdk-test
# 建立 hello.c 與 CMakeLists.txt（內容見 SDK README）

. /home/agmis/Documents/qc/sdk/environment-setup-aarch64-oe-linux
cmake . && cmake --build .
file hello

scp hello root@192.168.3.63:/data/
ssh root@192.168.3.63 'chmod +x /data/hello && /data/hello && ldd /data/hello'
```

本次輸出：

```
Hello World!
	linux-vdso.so.1 (0x0000007f8b46f000)
	libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6 (0x0000007f8b260000)
	/lib/ld-linux-aarch64.so.1 (0x0000007f8b436000)
```

> `ldd` 全部解析成功（無 `not found`）才是「SDK 與映像同源」的實證，比單純比對 hash 更直接。建議納入日後的驗收步驟。

### ⚠️ 與 SDK README 不符之處

**1. SDK 不自帶編譯器**

README 稱 `which $CC` 應指向：

```
sysroots/x86_64-qtisdk-linux/usr/bin/aarch64-oe-linux/aarch64-oe-linux-gcc
```

實際上此版 SDK **僅提供 target sysroot，編譯器用系統的**：

```
$ type -a aarch64-linux-gnu-gcc
aarch64-linux-gnu-gcc is /usr/bin/aarch64-linux-gnu-gcc     # Ubuntu 13.3.0

$ echo $CC
aarch64-linux-gnu-gcc ... --sysroot=/home/agmis/Documents/qc/sdk/sysroots/aarch64-oe-linux
```

這正是安裝腳本會自動安裝 `gcc-aarch64-linux-gnu` 的原因（未裝時腳本會嘗試 `sudo apt install`）。**依 README 預期去 `which` 會誤判環境設定失敗，實則正常。**

判斷方法用 `type -a` 而非 `which` —— 前者列出完整解析順序，後者只給第一個結果。

**2. 環境設定檔名**

README 的「Results」段落寫 `environment-setup-armv8a-oe-linux`，實際為 `environment-setup-aarch64-oe-linux`。README 內文後續用的也是 `aarch64`，應為該段落筆誤。

### 問題與解法

| 症狀 | 原因 | 解法 |
|---|---|---|
| 安裝腳本要求 sudo 密碼 | 缺 `gcc-aarch64-linux-gnu`，腳本嘗試自動安裝 | 先 `sudo apt install -y gcc-aarch64-linux-gnu` |
| `which $CC` 指向 `/usr/bin` 而非 SDK | **正常** — 見上方說明 | 無須處理 |
| 板上執行時 `ldd` 出現 `not found` | SDK 與映像版本不符 | 回第 2 項比對 Metadata Revision |

### 專案範本

位於 `/home/agmis/Documents/qc/template/`，已端到端測試通過。

```
template/
├── CMakeLists.txt      # 含註解掉的 GStreamer / TFLite / Threads 區塊
├── build.sh            # 自動 source SDK 環境 + 驗證產物架構
├── deploy.sh           # scp 至板子並執行
├── src/main.c
├── include/
└── .gitignore          # build/
```

**用法**

```bash
cp -r /home/agmis/Documents/qc/template ~/myproject
cd ~/myproject
./build.sh
./deploy.sh
```

**無須手動 source** —— `build.sh` 已包含。可用環境變數覆寫：

```bash
BOARD=root@192.168.3.100 ./deploy.sh       # 換板子
./deploy.sh --no-run                        # 只推不執行
./build.sh -DCMAKE_BUILD_TYPE=Release       # 額外參數傳給 cmake
```

**兩道護欄**（皆已實測）

| 護欄 | 防範什麼 |
|---|---|
| 陳舊快取自動清除 | 若 `CMakeCache.txt` 不含 `aarch64`（忘記 source 就先跑過 `cmake` 所致），自動刪除 `build/` 重新設定。否則快取會一直記著主機 gcc，事後補 source 也無效 |
| 產物架構驗證 | 編譯後檢查 `file` 輸出含 `ARM aarch64`，不符即報錯退出，避免推送板子跑不動的執行檔 |

SDK 路徑錯誤時明確報錯，並提示以 `SDK_ENV` 覆寫。

**函式庫連結方式（已確認 sysroot 實際情況）**

| 函式庫 | 有 `.pc` 檔 | 寫法 |
|---|---|---|
| GStreamer、GLib、GTK+ 等 | ✅ | `pkg_check_modules(GST REQUIRED gstreamer-1.0)` |
| TensorFlow Lite | ❌ | `target_link_libraries(myapp PRIVATE tensorflowlite_c)` |

`environment-setup` 已將 `PKG_CONFIG_PATH` 指向 sysroot，因此 `pkg_check_modules` 找到的是**板子的**函式庫而非主機的。

> 更改專案名稱時需同步三處：`CMakeLists.txt` 的 `project()` 與 `add_executable()`、`build.sh` 的 `BIN` 變數（或以 `BIN=build/<name> ./deploy.sh` 覆寫）。

### 常見錯誤

| 症狀 | 原因 | 解法 |
|---|---|---|
| 板上 `cannot execute binary file: Exec format error` | 忘記 source，編出 x86_64 執行檔 | 用 `build.sh`；或編譯前 `echo $CC` 確認有值 |
| 補 source 後仍編出 x86_64 | `CMakeCache.txt` 記著主機 gcc | `rm -rf build` 重新設定（`build.sh` 會自動處理） |
| `pkg_check_modules` 找到主機的函式庫 | 未 source，`PKG_CONFIG_PATH` 未指向 sysroot | 同上 |

---

### 磁碟佔用

| 項目 | 大小 |
|---|---|
| SDK 安裝後（`sdk/`） | ~22 GB |
| `application_sdk/` 安裝檔 | 3.1 GB（安裝後可刪） |
| 原始 `.7z` | 3.1 GB（可刪或移至備份） |

---

## 9. ⚠️ 長指令貼上被截斷

本次驗證過程中重複發生四次，浪費最多時間的問題。

**症狀**

```
ERROR: pipeline could not be constructed: syntax error.
-bash: video/x-raw,format=NV12,...: No such file or directory
```

**原因**：從 markdown 複製多行程式碼區塊時，縮排與換行一併帶入。shell 在第一段結尾（如 `!`）即認為指令完整而送出，剩餘各行被當成獨立指令執行。

**判斷方式**：出現 `No such file or directory` 且內容是指令的一部分 → 是截斷，不是功能故障。

**解法（可靠度由高至低）**

1. **寫成腳本再執行** — 最可靠，繞開所有貼上問題：

   ```bash
   printf 'export PULSE_SERVER=unix:/run/pulse/native\npaplay -v /data/test/1khz.wav\n' > /tmp/t.sh
   adb push /tmp/t.sh /data/t.sh && adb shell "chmod +x /data/t.sh"
   adb shell /data/t.sh
   ```

2. **整條貼成一行**，中間不留任何換行。

3. **反斜線續行** — 反斜線後不可有空格。

另請留意易混淆字元：`sync`/`sunc`、`s16le`/`s161e`（L 與 1）、`--`/`==`。

---

## 10. 板載 Camera ×5（未完成）

**驗什麼**：五顆 MIPI camera 的 preview。

**怎麼驗**（板子端，需先通過 §4）

```bash
export WAYLAND_DISPLAY=wayland-1
export XDG_RUNTIME_DIR=/run/user/root

gst-launch-1.0 qtiqmmfsrc name=camsrc camera=0 ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! waylandsink fullscreen=true async=true sync=false
```

`camera=0` 換成 0–4 各測一次。`Ctrl+C` 停止。

| camera | 接頭 | 文件記載 sensor |
|---|---|---|
| 0 | CON5102 | IMX577 |
| 1 | CON5002 | IMX586 |
| 2 | CON5001 | IMX586 |
| 3 | CON5101 | IMX686 |
| 4 | CON5103 | IMX586 |

**結果**：⚠️ 未測 — 板上未安裝 camera module。

**軟體側診斷（全部正常，可排除）**

```bash
adb shell 'systemctl status qmmf-server'      # active (running)
adb shell 'ls -l /dev/video* /dev/media*'     # 節點齊全
adb shell 'ls -l /dev/dri/'                   # card0 存在
adb shell 'ps -ef | grep cam_cci'             # 6 個 kernel thread
```

**判斷 sensor 是否存在**

```bash
adb shell 'dmesg | grep -iE "cam_sensor|cci_"'
```

本次輸出：

```
cam_sensor_match_id: read id: 0x0  expected id 0x842
cam_cci_read: CCI0_I2C_M0_Q1 ERROR with Slave 0x5a
NACK ERROR: 0x10000000        rc: -22
```

`read id: 0x0` + I2C NACK + `-EINVAL`，且橫跨 CCI0/1/2 三條 bus 的 slot 0–3 全部相同 → **I2C 上無裝置回應，即模組未安裝**。

> 若模組已安裝但故障，通常讀回**非零但錯誤**的 ID，而非 `0x0`。以此區分「沒插」與「壞掉」。

### ⚠️ sensor 型號與文件不符

kernel 實際 probe 的是 **Samsung** sensor：

```
s5kgd2sp03  (expected id 0x842)   slot 0-3
s5k3m5      (expected id 0x30d5)  slot 3
```

但 Release Notes 記載 RDK 配置為 **Sony IMX577 / IMX586 / IMX686**。

**裝上 Sony 模組前務必先向硬體端確認 device tree 配置，否則裝了也不會被偵測到。**

---

## 11. USB Camera（UVC）

**驗什麼**：USB 視訊裝置可擷取影像並即時顯示至 HDMI。

本次使用 **AVerMedia PW310P**（`07ca:310b`）。

### 怎麼驗

**1. 偵測**

```bash
lsusb                                      # 應列出該裝置
dmesg | grep -i uvc                        # 應見 "Found UVC 1.00 device"
ls -l /dev/video*                          # 應新增節點（時間戳為插入時間）
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video2 --list-formats-ext # 支援的格式與解析度
```

**2. 靜態擷取**（不需顯示，可單獨驗證 camera 本身）

```bash
gst-launch-1.0 -q v4l2src device=/dev/video2 num-buffers=15 ! \
  image/jpeg,width=1280,height=720,framerate=30/1 ! \
  multifilesink location=/tmp/shot.jpg

file /tmp/shot.jpg    # 應為 "JPEG image data ... 1280x720"
```

取回主機檢視，確認**不是黑畫面或雜訊**：

```bash
scp root@192.168.3.63:/tmp/shot.jpg .
```

> `num-buffers=15` 而非 1 —— UVC 裝置前幾張影格常在自動曝光收斂前，可能過暗或全黑。

**3. HDMI 即時預覽**

```bash
export WAYLAND_DISPLAY=wayland-1
export XDG_RUNTIME_DIR=/run/user/root

gst-launch-1.0 v4l2src device=/dev/video2 ! \
  image/jpeg,width=1280,height=720,framerate=30/1 ! \
  jpegdec ! videoconvert ! waylandsink fullscreen=true sync=false
```

`Ctrl+C` 停止。pipeline 應達 `PLAYING` 狀態且 HDMI 出現即時畫面。

### 結果

| 項目 | 結果 |
|---|---|
| USB 偵測 | ✅ AVerMedia PW310P，UVC 1.00 |
| 驅動 | ✅ `uvcvideo`（kernel 5.15.170 內建） |
| 裝置節點 | ✅ `/dev/video2`（擷取）、`/dev/video3`（metadata） |
| 支援格式 | MJPG、YUYV；最高 1920x1080@30fps |
| 靜態擷取 | ✅ 80 KB JPEG，1280x720，畫面正常 |
| HDMI 即時預覽 | ✅ pipeline 達 PLAYING，螢幕有畫面 |

### 與板載 MIPI camera 的對照

兩者走**完全不同的路徑**：

| | 板載 MIPI（§10） | USB（本節） |
|---|---|---|
| 來源元件 | `qtiqmmfsrc` | `v4l2src` |
| 驅動層 | CCI/I2C + `qmmf-server` | `uvcvideo` + V4L2 |
| 結果 | ✗ `Failed to Open Camera` | ✅ 正常 |

**USB camera 能通，證明 GPU、Wayland、顯示輸出、GStreamer 全部正常**，反向佐證 §10 的失敗確實純粹是缺少 MIPI 模組，而非軟體或顯示鏈路問題。

### 問題與解法

| 症狀 | 原因 | 解法 |
|---|---|---|
| `lsusb` 無此裝置 | 供電不足或線材問題 | 換 USB 埠；高耗電相機建議用外部供電 hub |
| 擷取的影像全黑 | 自動曝光尚未收斂 | 加大 `num-buffers`（15 以上） |
| `Device or resource busy` | 另一程序佔用中 | `fuser -v /dev/video2` 查出後結束該程序 |
| `v4l2-ctl --list-devices` 報 `/dev/video0: Operation already in progress` | 板載 camera 節點被 `qmmf-server` 佔用 | 正常，與 USB camera 無關 |

---

## 12. 待辦

- [ ] 板載 Camera：待 MIPI 模組到貨；裝前先釐清 §10 的 sensor 型號不一致
- [x] ~~USB Camera~~ — 已完成，見 §11
- [ ] 麥克風：待硬體到貨，以 `qtitinymix` 測 AMIC1/2、DMIC1/2/3、headset mic（指令見 Release Notes）
- [ ] AI（TensorFlow Lite）— 工具已預裝（`label_image`、`benchmark_model`、`libtensorflowlite_c.so`），但**板上無 `.tflite` 模型檔**，需先取得模型與 BMP 測試圖。端到端 pipeline（`gst-tflite-*-example`）現可改用 §11 的 USB camera 作為輸入源
- [x] ~~Application SDK 解壓與交叉編譯環境~~ — 已完成，見 §8

---

## 13. 應回報文件維護者

| # | 項目 | 影響 |
|---|---|---|
| 1 | §6 `XDG_RUNTIME_DIR=/run/user/root` 在 root 身分下不可用 | **高** — 照文件操作必定卡住且難以追因 |
| 2 | §7 文件提供的 `root`/`oelinux123` 無法用於 SSH（預設 `PermitRootLogin prohibit-password`） | **高** — 症狀為「密碼錯誤」，極易誤判 |
| 3 | §10 device tree 的 sensor 型號（Samsung）與文件（Sony IMX）不符 | **中** — 裝上模組後才會發現 |
| 4 | §8 SDK README：宣稱自帶編譯器（實際用系統的）、環境設定檔名寫成 `armv8a`（實際 `aarch64`） | **低** — 易誤判環境設定失敗 |
| 5 | 燒錄耗時 1m10s vs SOP 的 11m35s | **低** — 資訊性 |

第 5 項說明：吞吐量實測 122–255 MB/s（61 筆取樣），SOP 記載 25.8–32.8 MB/s。本次板子列舉於 USB 3.0 root hub（`Bus 002`，`1d6b:0003`），SOP 的數值屬 USB 2.0 區間。結果正確（0 錯誤、log 行數吻合、kernel hash 吻合）。建議 SOP 加註時間隨 USB 2.0/3.0 而異，避免後續操作者因「太快」而誤判未燒完。

---

## 14. 備忘

**硬體強制 EDL：RDK 的 SW1701 Pin 1 撥 ON。** 板子完全不上 USB 時唯一的救援手段，建議事先記下。

退出 EDL：斷 12V 與 USB → SW1701 Pin 1 撥回 OFF → **先接 12V，再接 Type-C**（順序不可顛倒）。

> 順序之所以重要，是因為 Type-C 的 VBUS 才是實際觸發開機的訊號 —— 原理見 **§15**。

| USB ID | 模式 | 意義 |
|---|---|---|
| `05c6:901d` | Mission | 正常開機，adb 可用，腳本會自動切 EDL |
| `05c6:9008` | EDL | 可直接燒。燒錄失敗的板子也是此狀態 |
| 無 | — | 無供電/連線，或需硬體強制 EDL |

UART 登入：`root` / `oelinux123`（燒錄後的 Ubuntu 映像）。

---

## 15. ⚠️ 上電後不會自己開機 —— 需 USB VBUS 觸發

**現象**：接上 12V 主電源後板子毫無反應（無 UART 輸出、網路不通、adb 看不到），
插上筆電（Type-C）才開機。容易誤判為板子故障或電源供應器問題。

**這是 PMIC 的設計行為，不是故障。** 板子自己的 PON log 寫得很明確：

```bash
dmesg | grep "PMIC PON log"
cat /sys/kernel/debug/ipc_logging/pmic_pon/log     # 完整開關機序列
```

```
State=OFF;  PON Trigger: USB_CHARGER      ← 開機由 USB VBUS 觸發
State=PON;  Begin PON Sequence
State=PON;  Waiting on PS_HOLD
State=ON;   PON Successful
```

觸發源是 **`USB_CHARGER`**，而非 `KPDPWR`（電源鍵）或 `SMPL`（掉電自動復電）。

**原因**：QCS8550 的 PMIC（pmk8550）沿用 Qualcomm 手機平台行為，
把 **Type-C 的 VBUS 視為合法的開機事件源**（手機插上充電器本來就該亮）。
在開發板上就表現為：

> 12V 上電 → PMIC 僅進入 OFF/待機（PS_HOLD 未拉起）→ 插入 USB → VBUS 觸發 PON → 才真正開機

這也解釋了 §14 為何強調「**先接 12V，再接 Type-C**，順序不可顛倒」 ——
Type-C 是實際按下的那顆「電源鍵」。

**排查順序**（板子沒反應時，先確認這個再懷疑硬體）：

| 現象 | 判讀 |
|---|---|
| 12V 已接、USB 未接、無反應 | 正常，尚未觸發 PON |
| 插上 USB 後開機 | 正常行為 |
| 插上 USB 仍無反應 | 才需懷疑供電/板子/EDL |

> **未驗證**：推論 USB 端只需提供 VBUS，與對端是不是筆電無關 ——
> 理論上一般 USB 充電器或行動電源即可。尚未實測，若成立則不必為了開機佔用筆電。
> 若要改成「上電即開」需調整 device tree 的 PMIC PON 設定，屬於改開機行為，需重新燒錄。

### 附帶發現：RTC 無電池

```
rtc-pm8xxx ... setting system clock to 1970-01-01T00:00:05 UTC
```

每次開機系統時間都歸零到 1970，需靠 NTP 或手動校時。副作用是
`journalctl --list-boots` 只認得到本次 boot，**歷史開機紀錄無法回溯** ——
要查前幾次開機原因，只能靠上面的 `pmic_pon` log。
