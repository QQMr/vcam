# vcam_out_videobuf2_setup 逐步解析

位於 `videobuf.c:184`，負責填寫 `vb2_queue` 所有設定欄位，並呼叫 kernel 完成初始化。

## 函數原型

```c
int vcam_out_videobuf2_setup(struct vcam_device *dev)
```

## 步驟拆解

### 1. 取得 queue 指標

```c
struct vb2_queue *q = &dev->vb_out_vidq;
```

`vb_out_vidq` 直接嵌在 `vcam_device` 結構內（`device.h:65`），不需額外分配。

---

### 2. 宣告 buffer 類型

```c
q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
```

告訴 vb2 這是 capture（讀取影像）佇列，必須與 userspace 呼叫 `VIDIOC_REQBUFS` 時傳入的 `type` 欄位一致。

---

### 3. 允許的 I/O 模式

```c
q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_READ;
```

| 模式 | 說明 |
|------|------|
| `VB2_MMAP`    | userspace 以 `mmap()` 直接映射 kernel buffer |
| `VB2_USERPTR` | userspace 傳入自己分配的記憶體位址 |
| `VB2_READ`    | 以 `read()` syscall 逐次讀取資料 |

---

### 4. 反向指標（back-pointer）

```c
q->drv_priv = dev;
```

讓所有 callback 函式可用 `vb2_get_drv_priv(vq)` 取回 `vcam_device`，是 vb2 傳遞 driver context 的標準慣例。

---

### 5. 自訂 buffer 結構大小

```c
q->buf_struct_size = sizeof(struct vcam_out_buffer);
```

`vcam_out_buffer` 以 `vb2_v4l2_buffer` 為首欄位（embedded），vb2 按此大小分配記憶體，使 driver 可透過 `container_of` 存取附加的私有欄位（`filled`、`list`）。

```
vcam_out_buffer
├─ struct vb2_v4l2_buffer vb   ← vb2 標準欄位
├─ struct list_head list        ← 掛入 active list
└─ size_t filled                ← 已填入的資料量
```

---

### 6. 時間戳類型

```c
q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
```

使用單調時鐘（`CLOCK_MONOTONIC`），保證時間戳不會因系統時間調整而跳動，適合影像同步場景。

---

### 7. 掛載 driver callbacks

```c
q->ops = &vcam_vb2_ops;
```

綁定 driver 實作的六個操作：

| Callback | 觸發時機 | 說明 |
|----------|----------|------|
| `queue_setup`     | `VIDIOC_REQBUFS`   | 決定 buffer 數量（最少 2）與每個 buffer 的大小 |
| `buf_prepare`     | 每次 `VIDIOC_QBUF` | 驗證 buffer 空間 ≥ `output_format.sizeimage`，並設定 payload 大小 |
| `buf_queue`       | buffer 進入佇列時  | 將 buffer 加入 `vcam_out_vidq.active` list（spinlock 保護） |
| `start_streaming` | `VIDIOC_STREAMON`  | 建立並喚醒 `submitter_thread` kthread |
| `stop_streaming`  | `VIDIOC_STREAMOFF` | 停止 kthread，將剩餘 buffer 以 `VB2_BUF_STATE_ERROR` 歸還 |
| `wait_prepare` / `wait_finish` | vb2 等待期間 | 釋放 / 重取 `vcam_mutex`，避免等待時持鎖死鎖 |

---

### 8. 記憶體分配器

```c
q->mem_ops = &vb2_vmalloc_memops;
```

使用 `vmalloc` 分配 buffer，適合不需要連續實體記憶體的虛擬裝置。
（實體硬體裝置通常改用 `vb2_dma_contig_memops`）

---

### 9. 最少 buffer 數

```c
q->min_buffers_needed = 2;
```

執行 `VIDIOC_STREAMON` 時，若 active buffer 數量少於 2，kernel 會拒絕啟動 streaming，防止 submitter_thread 無 buffer 可寫入。

---

### 10. 大鎖

```c
q->lock = &dev->vcam_mutex;
```

所有 vb2 的 ioctl 路徑都受此 mutex 保護，與 `wait_prepare` / `wait_finish` 搭配，實現「等待期間暫時釋鎖」的模式。

---

### 11. 正式初始化

```c
return vb2_queue_init(q);
```

kernel 驗證所有欄位的合法性，分配內部資料結構。回傳 0 表示成功，之後才能接受來自 userspace 的 `VIDIOC_REQBUFS` 等 ioctl。

---

## 總結流程圖

```
vcam_out_videobuf2_setup(dev)
  │
  ├─ [1] q = &dev->vb_out_vidq
  ├─ [2] q->type             = V4L2_BUF_TYPE_VIDEO_CAPTURE
  ├─ [3] q->io_modes         = VB2_MMAP | VB2_USERPTR | VB2_READ
  ├─ [4] q->drv_priv         = dev
  ├─ [5] q->buf_struct_size  = sizeof(vcam_out_buffer)
  ├─ [6] q->timestamp_flags  = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
  ├─ [7] q->ops              = &vcam_vb2_ops
  ├─ [8] q->mem_ops          = &vb2_vmalloc_memops
  ├─ [9] q->min_buffers_needed = 2
  ├─[10] q->lock             = &dev->vcam_mutex
  └─[11] return vb2_queue_init(q)
```

## 相關檔案

| 檔案 | 說明 |
|------|------|
| `videobuf.c:184` | 本函數實作 |
| `videobuf.c:73`  | `vcam_out_queue_setup` callback |
| `videobuf.c:174` | `vcam_vb2_ops` callback 表 |
| `device.h:65`    | `vb_out_vidq` 欄位定義 |
| `device.c:247`   | 呼叫端（格式切換時重新初始化） |
