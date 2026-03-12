# vcam Driver Testing Guide

This document describes how to test the `vcam` virtual V4L2 camera driver, covering
environment setup, compliance validation, functional tests, and optional feature tests.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Build and Load](#2-build-and-load)
3. [Identifying Device Nodes](#3-identifying-device-nodes)
4. [V4L2 Compliance Test](#4-v4l2-compliance-test)
5. [Device Capability Inspection](#5-device-capability-inspection)
6. [Framebuffer Inspection](#6-framebuffer-inspection)
7. [vcam-util Functional Tests](#7-vcam-util-functional-tests)
8. [Frame Input Test](#8-frame-input-test)
9. [Optional Feature Tests](#9-optional-feature-tests)
   - [9.1 Pixel Format Conversion](#91-pixel-format-conversion)
   - [9.2 Image Scaling](#92-image-scaling)
   - [9.3 Image Cropping](#93-image-cropping)
10. [Multi-Device Test](#10-multi-device-test)
11. [Unload and Cleanup](#11-unload-and-cleanup)
12. [Expected Results Summary](#12-expected-results-summary)

---

## 1. Prerequisites

Install required packages (versions must match the running kernel):

```sh
sudo apt install linux-headers-$(uname -r) v4l-utils
```

Verify tools are available:

```sh
v4l2-compliance --version
v4l2-ctl --version
fbset --version
```

---

## 2. Build and Load

### Build

```sh
make
```

Expected output: `vcam.ko` and `vcam-util` are produced without errors.

### Load dependencies

```sh
sudo modprobe -a videobuf2_vmalloc videobuf2_v4l2
```

### Load the module (default: 1 device, no conversions)

```sh
sudo insmod vcam.ko
```

Verify the module loaded:

```sh
lsmod | grep vcam
```

Expected: `vcam` appears in the list.

Check kernel log for errors:

```sh
dmesg | tail -20
```

Expected: no `ERROR` or `BUG` lines related to `vcam`.

---

## 3. Identifying Device Nodes

After loading, three device nodes are created:

| Node | Purpose |
|------|---------|
| `/dev/videoX` | V4L2 capture device |
| `/dev/vcamctl` | Control device for `vcam-util` |
| `/dev/fbX` | Framebuffer input device |

Find the exact node numbers:

```sh
ls /dev/video* /dev/vcamctl /dev/fb*
```

Set shell variables for use in subsequent tests (replace `X` with actual numbers):

```sh
VIDEO=/dev/video0
FB=/dev/fb0
CTL=/dev/vcamctl
```

---

## 4. V4L2 Compliance Test

```sh
sudo v4l2-compliance -d $VIDEO -f
```

### Expected result

```
...
Total for device /dev/videoX: N, N succeeded, 1 failed, 0 warnings
```

- **1 failure** is expected (known limitation: `VIDIOC_EXPBUF` not supported).
- **0 warnings** is required for a passing result.
- Any additional failures or warnings indicate a regression.

---

## 5. Device Capability Inspection

```sh
sudo v4l2-ctl -d $VIDEO --all
```

### Expected key fields

```
Driver Info:
    Driver name   : vcam
    Card type     : vcam
    Bus info      : platform: virtual
    Capabilities  : 0x85200001
        Video Capture
        Read/Write
        Streaming
        Extended Pix Format
        Device Capabilities
```

Verify the following formats are listed:

```sh
sudo v4l2-ctl -d $VIDEO --list-formats
```

Expected minimum output:

```
ioctl: VIDIOC_ENUM_FMT
    Type: Video Capture

    [0]: 'RGB3' (24-bit RGB 8-8-8)
```

---

## 6. Framebuffer Inspection

```sh
sudo fbset -fb $FB --info
```

### Expected output

```
mode "640x480"
    geometry 640 480 640 480 24
    timings 0 0 0 0 0 0 0
    rgba 8/0,8/8,8/16,0/0
endmode

Frame buffer device information:
    Name        : vcamfb
    Size        : 921600
    Type        : PACKED PIXELS
    Visual      : TRUECOLOR
```

Key checks:
- Resolution is `640x480`.
- Depth is `24` bits (RGB24).
- Size is `921600` bytes (640 × 480 × 3).

---

## 7. vcam-util Functional Tests

### 7.1 List devices

```sh
sudo ./vcam-util -l
```

Expected:

```
Available virtual V4L2 compatible devices:
1. fb0(640,480,rgb24) -> /dev/video0
```

### 7.2 Create a new device

```sh
sudo ./vcam-util -c
```

Expected: `A new device will be created`

Verify with:

```sh
sudo ./vcam-util -l
```

Expected: two entries (indices 1 and 2).

### 7.3 Create a device with custom resolution and pixel format

```sh
sudo ./vcam-util -c -s 1280x720 -p rgb24
```

Verify:

```sh
sudo ./vcam-util -l
```

Expected: new entry with `1280x720,rgb24`.

### 7.4 Modify an existing device

Change device 1 to 320x240:

```sh
sudo ./vcam-util -m 1 -s 320x240
```

Expected: `Setting modified`

Verify:

```sh
sudo ./vcam-util -l
```

Expected: device 1 now shows `320x240`.

### 7.5 Remove a device

```sh
sudo ./vcam-util -r 2
```

Expected: `Device removed`

Verify with `-l` that the device count decreased by one.

### 7.6 Use alternate control device path

```sh
sudo ./vcam-util -d /dev/vcamctl -l
```

Expected: same output as the default `-l`.

---

## 8. Frame Input Test

This test writes a raw RGB24 frame to the framebuffer and captures it from the V4L2 device.

### 8.1 Generate a test frame

Create a solid-color 640x480 RGB24 frame (red):

```sh
# 640 * 480 * 3 = 921600 bytes: R=0xFF G=0x00 B=0x00
python3 -c "
import sys
w, h = 640, 480
data = bytes([0xFF, 0x00, 0x00]) * (w * h)
sys.stdout.buffer.write(data)
" > /tmp/red_frame.raw
```

### 8.2 Write the frame to the framebuffer

```sh
sudo dd if=/tmp/red_frame.raw of=$FB bs=921600 count=1
```

Expected: `1+0 records in / 1+0 records out / 921600 bytes copied`

### 8.3 Capture one frame from the V4L2 device

```sh
sudo v4l2-ctl -d $VIDEO --stream-mmap --stream-count=1 --stream-to=/tmp/captured.raw
```

Expected: no errors; `/tmp/captured.raw` is created with size 921600 bytes.

### 8.4 Verify captured content

```sh
ls -la /tmp/captured.raw
```

Expected: size `921600`.

Optionally inspect the first bytes to confirm the red channel:

```sh
xxd /tmp/captured.raw | head -4
```

Expected: first bytes show `ff 00 00` pattern (RGB24 red).

---

## 9. Optional Feature Tests

Reload the module with the desired feature enabled for each test below.

```sh
sudo rmmod vcam
```

### 9.1 Pixel Format Conversion

Load with pixel conversion enabled:

```sh
sudo insmod vcam.ko allow_pix_conversion=1
```

Verify YUYV format is available:

```sh
sudo v4l2-ctl -d $VIDEO --list-formats
```

Expected: both `RGB3` (RGB24) and `YUYV` appear in the format list.

Capture a YUYV frame:

```sh
sudo v4l2-ctl -d $VIDEO \
    --set-fmt-video=width=640,height=480,pixelformat=YUYV \
    --stream-mmap --stream-count=1 --stream-to=/tmp/captured_yuyv.raw
```

Expected: no errors; output file size is `614400` bytes (640 × 480 × 2).

```sh
ls -la /tmp/captured_yuyv.raw
```

### 9.2 Image Scaling

Load with scaling enabled:

```sh
sudo insmod vcam.ko allow_scaling=1
```

Modify the device to 1280x720 (720p):

```sh
sudo ./vcam-util -m 1 -s 1280x720
```

Write a 640x480 source frame:

```sh
sudo dd if=/tmp/red_frame.raw of=$FB bs=921600 count=1
```

Capture a 1280x720 frame:

```sh
sudo v4l2-ctl -d $VIDEO \
    --set-fmt-video=width=1280,height=720,pixelformat=RGB3 \
    --stream-mmap --stream-count=1 --stream-to=/tmp/captured_720p.raw
```

Expected: output file size is `2764800` bytes (1280 × 720 × 3).

```sh
ls -la /tmp/captured_720p.raw
```

### 9.3 Image Cropping

Load with cropping enabled:

```sh
sudo insmod vcam.ko allow_cropping=1
```

Check that crop selection is reported:

```sh
sudo v4l2-ctl -d $VIDEO --get-selection=target=crop
```

Expected: returns crop rectangle within the active area (Four-Thirds system ratio).

Capture a frame and verify the output dimensions match the crop:

```sh
sudo v4l2-ctl -d $VIDEO --stream-mmap --stream-count=1 --stream-to=/tmp/captured_crop.raw
ls -la /tmp/captured_crop.raw
```

---

## 10. Multi-Device Test

Load the module creating 3 devices up front:

```sh
sudo insmod vcam.ko create_devices=3
```

List all devices:

```sh
sudo ./vcam-util -l
```

Expected: 3 entries.

Run compliance test on each video node:

```sh
for dev in /dev/video0 /dev/video1 /dev/video2; do
    echo "=== Testing $dev ==="
    sudo v4l2-compliance -d $dev -f 2>&1 | grep "Total for"
done
```

Expected: each shows `1 failed, 0 warnings`.

Test the maximum device limit (default `devices_max=8`):

```sh
# With 3 already loaded, create 5 more (total = 8)
for i in $(seq 1 5); do sudo ./vcam-util -c; done
sudo ./vcam-util -l | wc -l   # should be 9 lines (header + 8 devices)

# Attempt to exceed the limit
sudo ./vcam-util -c
```

Expected: the 9th create attempt fails with an error (device limit reached).

---

## 11. Unload and Cleanup

Remove all virtual devices before unloading:

```sh
# Remove devices by index (highest first to avoid re-indexing issues)
sudo ./vcam-util -r 8
sudo ./vcam-util -r 7
# ... repeat down to 1, or just rmmod if none are streaming
```

Unload the module:

```sh
sudo rmmod vcam
```

Verify device nodes are gone:

```sh
ls /dev/video* /dev/vcamctl /dev/fb* 2>&1
```

Expected: the vcam-created nodes (`vcamctl`, and the vcam `videoX`/`fbX`) are no longer present.

Check dmesg for clean shutdown:

```sh
dmesg | tail -10
```

Expected: no `BUG`, `WARNING`, or `use-after-free` messages.

---

## 12. Expected Results Summary

| Test | Pass Criterion |
|------|---------------|
| Module load | `lsmod` shows `vcam`; no dmesg errors |
| V4L2 compliance | Exactly 1 failure, 0 warnings |
| Device capability | `Video Capture`, `Read/Write`, `Streaming` all reported |
| Framebuffer info | `640x480`, depth 24, size 921600 |
| `vcam-util -l` | Lists device with correct fb node and video node |
| `vcam-util -c` | New device appears in `-l` output |
| `vcam-util -m` | Resolution change reflected in `-l` output |
| `vcam-util -r` | Device removed from `-l` output |
| Frame capture | Captured file size matches `width × height × bytes_per_pixel` |
| Pixel conversion | YUYV listed in formats; captured size = `w × h × 2` |
| Scaling | Captured size = `1280 × 720 × 3` when 720p configured |
| Multi-device | All devices pass compliance; limit enforced at `devices_max` |
| Module unload | No dmesg BUG/WARNING; device nodes removed |
