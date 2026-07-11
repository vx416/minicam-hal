# MiniCam HAL Roadmap

這個 side project 的目標是用小而完整的 C++ camera stack，理解 Pixel Camera 團隊會碰到的核心系統問題：request pipeline、buffer ownership、camera processing、Linux driver interface、Android HAL integration。

不建議第一天就編完整 AOSP。完整 Camera HAL 牽涉 vendor partition、VINTF、SELinux、AIDL/HIDL、CTS 等大量平台設定，容易讓時間花在 build configuration，而不是核心系統設計。

## Phase 1: C++ Pipeline

先完成：

```text
CaptureRequest
-> bounded queue
-> virtual sensor
-> processing worker
-> callback
```

這一階段要展示 C++ concurrency 能力。

核心練習：

- request/result model
- producer-consumer queue
- worker thread lifecycle
- timeout and cancellation
- callback ownership
- deterministic shutdown

可交付成果：

- `minicam_capture --frames 100`
- 每個 request 都會產生一個 result callback
- 可以正常啟動、停止、flush
- 有單元測試覆蓋 queue、worker、shutdown

面試可講：

- 為什麼 camera stack 通常是 asynchronous pipeline
- 如何避免 callback race condition
- 如何處理 queue 滿、sensor 慢、processing 慢
- 如何設計 thread-safe API

## Phase 2: Buffer Management

加入：

```text
buffer pool
ownership state machine
leak detection
backpressure
```

這是履歷價值最高的部分。

核心練習：

- buffer lifecycle
- acquire/release ownership
- in-flight request tracking
- zero-copy mindset
- bounded memory usage
- leak and double-release detection

建議狀態機：

```text
Free -> AcquiredBySensor -> AcquiredByProcessor -> AcquiredByClient -> Free
```

可交付成果：

- 固定數量 buffer pool
- request 數量大於 buffer 數量時會 backpressure
- stress test 後確認沒有 buffer leak
- metrics 顯示 queue depth、in-flight buffers、dropped frames

面試可講：

- camera pipeline 為什麼不能無限制 allocate frame buffer
- backpressure 跟 frame drop 的 tradeoff
- buffer ownership bug 為什麼常造成 stability issue
- 如何 debug leak、double free、use-after-release

## Phase 3: Camera Processing

加入：

```text
AE
AWB
crop
resize
noise reduction
```

這能補 camera domain knowledge。

核心練習：

- frame metadata
- exposure and gain simulation
- auto-exposure loop
- auto-white-balance loop
- image transform pipeline
- per-stage latency measurement

可交付成果：

- synthetic sensor frame 或 sample image input
- 輸出 processed RGB/PNG
- 每個 pipeline stage 有 latency metrics
- 可用 command line 控制 resolution、fps、exposure、gain

面試可講：

- AE/AWB 大致如何根據 frame statistics 調整 metadata
- camera processing pipeline 為什麼要分 stage
- image quality 與 latency 的 tradeoff
- 為什麼 mobile camera 很重視 per-frame metadata

## Phase 4: Linux Integration

加入：

```text
V4L2 loopback
mmap
ioctl
poll
```

這能補 Linux / driver knowledge。

核心練習：

- V4L2 device model
- buffer queue/dequeue
- memory mapped IO
- blocking vs non-blocking capture
- poll/select event loop
- error handling around device IO

可交付成果：

- 從 V4L2 loopback 或 real webcam 讀 frame
- 用 `ioctl` query capability / format / buffer
- 用 `mmap` 管理 capture buffers
- 用 `poll` 等待 frame ready

面試可講：

- userspace HAL 如何跟 kernel driver 溝通
- `ioctl` 在 driver interface 裡扮演什麼角色
- 為什麼 camera buffer 通常用 mmap/DMABUF 這類機制
- device IO failure 要如何 propagate 到上層

## Phase 5: Android Integration

最後才考慮：

```text
AIDL interface
Native C++ service
Binder callback
Android Camera client demo
```

這一階段不是第一優先。除非時間充足，否則先用文件或 prototype 表達理解即可。

核心練習：

- AIDL service boundary
- Binder IPC
- native service lifecycle
- client callback
- Android camera framework high-level mapping

可交付成果：

- 一個簡化的 AIDL camera service
- Android client 可以 request capture
- service 回傳 metadata + image path 或 buffer handle
- 文件說明這個 mini service 如何對應到 Android Camera HAL3

面試可講：

- framework、HAL、driver 的分層
- Binder callback 的 ownership 和 threading 問題
- Android integration 為什麼比純 C++ prototype 多很多平台約束
- 如果進 Pixel Camera team，如何逐步 ramp up AOSP/HAL work

## Suggested Timeline

```text
Week 1: Phase 1
Week 2: Phase 2
Week 3: Phase 3
Week 4: Phase 4
Optional: Phase 5
```

如果 team match 時間很近，優先完成 Phase 1 + Phase 2，再準備 Phase 3/4 的設計說明。這會比半成品 Android HAL 更能展示系統設計判斷。

## Project Pitch

一句話版本：

```text
I built a small C++ camera HAL simulator to understand Android-style camera request pipelines, bounded buffer ownership, per-frame metadata, and Linux camera device integration.
```

較完整版本：

```text
The project models a simplified camera stack: capture requests enter a bounded asynchronous pipeline, frames are produced by a virtual or V4L2 sensor, buffers move through an explicit ownership state machine, processing stages attach metadata and latency metrics, and clients receive completion callbacks. I used it to study the stability and performance concerns that show up in camera HAL and driver-facing software.
```
