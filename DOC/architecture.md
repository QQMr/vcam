# vcam 驅動程式架構分析

## 整體架構圖

```
使用者空間 (Userspace)
─────────────────────────────────────────────────────────────
  vcam-util        應用程式 (ffmpeg / GStreamer / webcam app)
      │                              │
      │ ioctl                        │ read / mmap / poll
      ▼                              ▼
 /dev/vcamctl                   /dev/videoX
      │                              │
      │                              │
核心空間 (Kernel)
─────────────────────────────────────────────────────────────
  control.c                     device.c
  cdev (字元裝置)               video_device (V4L2)
      │                              │
      │ 建立 / 刪除 / 設定          │ V4L2 ioctl ops
      │                              │
      ▼                              ▼
  vcam_device ────────────── vb2_queue (videobuf2)
      │                              │
      │                              │ submitter_thread (kthread)
      ▼                              │
  vcam_in_queue  ◄──────────────────┘
  (雙緩衝 pending / ready)
      ▲
      │ write
      │
  /dev/fbX 或 /proc/vcamfbX
  fb.c (輸入端)
─────────────────────────────────────────────────────────────
  使用者空間輸入 (寫入原始影格)
```

---

## 各模組職責

### `module.c` — 模組入口

```
module_init
  └─ create_control_device()   建立 /dev/vcamctl
  └─ request_vcam_device()     預設建立 N 個虛擬攝影機
module_exit
  └─ destroy_control_device()  清理所有裝置
```

模組參數 (`modparam`)：

| 參數 | 預設值 | 說明 |
|------|--------|------|
| `devices_max` | 8 | 最多幾個虛擬攝影機 |
| `create_devices` | 1 | 載入時自動建立幾個 |
| `allow_pix_conversion` | 0 | 是否允許 RGB24 ↔ YUYV 轉換 |
| `allow_scaling` | 0 | 是否允許解析度縮放 |
| `allow_cropping` | 0 | 是否允許裁切 |

---

### `control.c` — 控制裝置 (`/dev/vcamctl`)

以 `cdev` 實作字元裝置，負責管理所有 `vcam_device` 的生命週期。

```
control_ioctl()
  ├─ VCAM_IOCTL_CREATE_DEVICE   → request_vcam_device()
  ├─ VCAM_IOCTL_DESTROY_DEVICE  → control_iocontrol_destroy_device()
  ├─ VCAM_IOCTL_GET_DEVICE      → control_iocontrol_get_device()
  └─ VCAM_IOCTL_MODIFY_SETTING  → control_iocontrol_modify_input_setting()
```

`vcam_device` 指標陣列由 `spinlock_t vcam_devices_lock` 保護，上限為 `devices_max`。

---

### `device.c` — V4L2 裝置核心

實作兩大部分：

#### ① V4L2 ioctl ops

```
vcam_ioctl_ops
  ├─ querycap              → 回報驅動名稱、能力
  ├─ enum / g / s_fmt      → 格式協商 (RGB24 / YUYV)
  ├─ enum_framesizes       → 支援的解析度清單
  ├─ enum_frameintervals   → 支援的 FPS 範圍
  ├─ g / s_parm            → 取得 / 設定 FPS
  ├─ reqbufs / qbuf / dqbuf → 委派給 vb2_ioctl_*
  └─ streamon / streamoff  → 委派給 vb2_ioctl_*
```

#### ② `submitter_thread` (kthread)

驅動的核心迴圈，在 streaming 期間持續運行：

```
while (!kthread_should_stop())
  ├─ 從 vcam_out_vidq.active 取出輸出 buffer
  ├─ 若無輸入 (fb_isopen == false)
  │     └─ submit_noinput_buffer()  填入漸層測試圖
  └─ 若有輸入
        └─ submit_copy_buffer()
              ├─ 同格式同解析度  → memcpy
              ├─ 同格式不同解析度 → copy_scale()
              ├─ 不同格式同解析度 → convert_rgb24/yuyv_buf()
              └─ 不同格式不同解析度
                    ├─ copy_scale_rgb24_to_yuyv()
                    └─ copy_scale_yuyv_to_rgb24()
  └─ schedule_timeout_interruptible()  依 output_fps 睡眠
```

#### 格式轉換矩陣

| 輸入 ↓ \ 輸出 → | RGB24 | YUYV |
|----------------|-------|------|
| **RGB24** | memcpy / scale | rgb24_to_yuyv + scale |
| **YUYV** | yuyv_to_rgb24 | memcpy / scale |

---

### `videobuf.c` — videobuf2 橋接

```
vcam_vb2_ops
  ├─ queue_setup     → 至少 2 個 buffer，大小 = sizeimage
  ├─ buf_prepare     → 驗證 buffer 大小，設定 payload
  ├─ buf_queue       → 加入 vcam_out_vidq.active list
  ├─ start_streaming → 啟動 submitter_thread (kthread_create)
  ├─ stop_streaming  → kthread_stop + 清空 active list
  ├─ wait_prepare    → mutex_unlock (允許 vb2 等待)
  └─ wait_finish     → mutex_lock
```

---

### `fb.c` — 輸入端 (framebuffer)

雙緩衝 ping-pong 機制：

```
vcam_in_queue
  ├─ buffers[0]  ◄── pending (userspace 正在寫入)
  ├─ buffers[1]  ◄── ready   (submitter_thread 正在讀取)
  └─ dummy       (備用)

vcamfb_write()
  ├─ 累積寫入 pending buffer
  ├─ 若超過 1 秒沒寫 → 重置 pending.filled = 0
  └─ 寫滿一幀後 → swap_in_queue_buffers()
                   (pending ↔ ready，持 in_q_slock)
```

提供兩種輸入介面：

| 介面 | 實作 | 說明 |
|------|------|------|
| `/proc/vcamfbX` | `proc_ops` | 透過 procfs 寫入 |
| `/dev/fbX` | `fb_ops` | 透過 Linux framebuffer 子系統 |

---

## 同步機制整理

| 鎖 | 保護對象 |
|----|----------|
| `spinlock_t in_q_slock` | `vcam_in_queue`（pending/ready swap） |
| `spinlock_t in_fh_slock` | `fb_isopen` flag |
| `spinlock_t out_q_slock` | `vcam_out_vidq.active` list |
| `spinlock_t vcam_devices_lock` | `control_device` 的 `vcam_devices[]` |
| `struct mutex vcam_mutex` | `vb2_queue` 及 `video_device` 的大鎖 |

---

## 已知限制

| 問題 | 位置 | 說明 |
|------|------|------|
| 縮放使用最近鄰插值 | `copy_scale()` | 畫質差，但 kernel 內不適合複雜插值 |
| 裁切大小固定為 3/4 | `vcam_try_fmt_vid_cap()` | `min_r = width * 3/4`，無法自由設定 |
| `vcamfb_mmap` 雙重 remap | `fb.c` | 對同一區塊 remap 兩次，第二次邏輯可疑 |
| `VCAM_IOCTL_ENUM_DEVICES` 未實作 | `vcam.h` | ioctl 號碼 `0x444` 定義了但無對應 handler |
