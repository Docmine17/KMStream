# KMStream — Latency Baseline & Measurement Guide

## 1. Software Pipeline Stages

Every frame carries a 32-byte `struct kms_video_hdr` with timestamps sampled from `CLOCK_MONOTONIC_RAW`:

1. **`Conv` (PC):** KMS primary/cursor plane query, DMA-BUF import into EGL (`EXT_image_dma_buf_import_modifiers`), GLES2 RGB-to-NV12 (BT.709 limited-range) + cursor compositing shaders, and `EGLSyncKHR` fence wait.
2. **`Enc` (PC):** `h264_vaapi` hardware encoding (`avcodec_send_frame` -> `avcodec_receive_packet`), trailing AUD (`00 00 00 01 09 F0`) injection, and SPS VUI bitstream rewrite on IDR frames (`BT.709`, `max_num_reorder_frames=0`, `max_dec_frame_buffering=1`).
3. **`TX` (PC):** Packetization into 1400-byte payload chunks and paced `sendto()` syscalls.
4. **`Net+Rx` (Link + Pi):**
   - `Net`: One-way wire + kernel RX wakeup delay (`t_dec_start + clock_offset_us - capture_ts_us - Conv - Enc`).
   - `Rx` (Reassembly): Time between the first chunk and the last chunk of a frame arriving in userspace (`span`).
5. **`Dec` (Pi):** `h264_v4l2m2m` (`/dev/video10`) hardware decode wait time.
6. **`KMS` (Pi):** Persistent 16-entry DRM FB cache lookup by `dmabuf_fd` (`0` DRM ioctls on cache hit) + non-blocking atomic KMS plane commit (`DRM_MODE_ATOMIC_NONBLOCK`).
7. **`E2E` (Total Software Glass-to-Glass Estimate):**
   - `t_kms_done (Pi, converted to PC clock) - capture_ts_us (of the actual displayed frame PTS)`.
   - Automatically accounts for any decoder frame holding (`Hold: Nf`).

Clock synchronization between PC and Pi runs continuously over `KMS_CTRL_PORT` (UDP 5001) using a minimum-RTT + EWMA filter (`ppoll()`-driven immediate PONG on the server).

---

## 2. Optional Physical Camera Glass-to-Glass Procedure

To measure true photon-to-photon latency (including monitor scanout and HDMI input lag):

1. Connect the PC and Raspberry Pi to two monitors side-by-side (or Picture-by-Picture on the same display).
2. Run a high-refresh millisecond counter on the PC screen, e.g.:
   ```bash
   python3 -c '
   import tkinter as tk, time
   r = tk.Tk(); r.attributes("-fullscreen", False)
   l = tk.Label(r, font=(" monospace", 96, "bold"), fg="white", bg="black")
   l.pack(expand=True, fill="both")
   t0 = time.monotonic()
   def tick():
       l.config(text=f"{(time.monotonic()-t0)*1000:08.1f} ms")
       r.after(1, tick)
   tick(); r.mainloop()
   '
   ```
3. Record both screens simultaneously using a smartphone slow-motion camera (240 fps = 4.16 ms resolution).
4. Step through 10–20 frames of the video and subtract the timestamp visible on the Pi monitor from the timestamp visible on the PC monitor.

---

## 3. Recorded Baseline Numbers (Phase 1 — 720p60 Pre-Optimization)

Captured over a 13-second run (780 frames) at `1280x720 @ 60 fps` (from `1920x1080` primary KMS plane):

| Metric | Avg | p99 | Notes |
|---|---|---|---|
| **PC `Conv` (KMS + EGL/GLES RGB->NV12)** | `0.62 ms` | `1.24 ms` | Includes `glFinish()` |
| **PC `Enc` (`h264_vaapi` + SPS patch)** | `0.21 ms` | `0.55 ms` | CQP 24, Constrained Baseline |
| **PC `TX` (`sendto` burst)** | `0.03 ms` | `0.11 ms` | ~2.5 KB P-frames, ~24.2 KB IDR |
| **Link Minimum RTT (`KMS_CTRL_PING`)** | `0.38 ms` | — | Direct 100 Mbps USB-Ethernet (`~0.19 ms` one-way wire) |
| **Pi `Net+Rx` (Queue wait + Reassembly)** | `0.04–10.72 ms` | — | Drifts up to `~10.7 ms` when single-threaded loop blocks in KMS/decode |
| **Pi `Dec` (`h264_v4l2m2m` wait)** | `4.61 ms` | `4.66 ms` | Hits the hardcoded `4.5 ms` poll deadline on every frame |
| **Pi `KMS` (`drmModeAddFB2` + `drmModeSetPlane`)** | `5.50–11.96 ms` | `12.02 ms` | **Legacy `drmModeSetPlane` blocks on HDMI vblank!** |
| **Pi `Hold` (V4L2 frame lag)** | `1 frame` (`16.67 ms`) | `1 frame` | Displayed PTS is `cur_frame_id - 1` |
| **Total Software E2E Latency** | `27.75–44.75 ms` | `27.95–44.83 ms` | Dominated by `Hold: 1f` (`16.7 ms`) + blocking `drmModeSetPlane` (`5.5–12.0 ms`) + single-thread queue stall (`0–10.7 ms`) |

---

## 4. Recorded Numbers After Phase 2 (Network & Pipeline Robustness)

Changes in Phase 2:
- Dedicated `rx_thread` on the Pi (`SO_RCVBUF = 8192 KB` effective) with 8-slot ring + chunk bitmap + automatic `KMS_CTRL_IDR_REQUEST` recovery.
- Server UDP packet pacing (~80 Mbps ceiling for bursts `> 6` chunks) + CQP24 (720p) / CQP26 (1080p) with long GOP (`600`) and on-demand IDR.
- V4L2 M2M buffers increased (`num_output_buffers=4`, `num_capture_buffers=6`), immediate exit from decode poll once a frame is dequeued (`0.02 ms` wait), and `frame_ring[3]` keeping `AVFrame` alive across KMS scanout.

| Mode | FPS (Pi) | Packet/Frame Loss (`Inc` / `rcvbuf`) | Link RTT | Pi `Net+Rx` | Pi `Dec` Wait | Pi `KMS` | `Hold` | **Software E2E (avg / p99)** |
|---|---|---|---|---|---|---|---|---|
| **720p @ 60 FPS** | **60.0 FPS** | `0 / 0` | `0.33 ms` | `0.56 + 0.50 ms` | **`0.02 / 0.04 ms`** | `9.20 / 10.05 ms` | `1f` | **`28.65 / 28.82 ms`** |
| **1080p @ 30 FPS** | **30.0 FPS** | `0 / 0` | `0.44 ms` | `1.09 + 2.59 ms` | **`0.02 / 0.04 ms`** | `9.68 / 11.89 ms` | `1f` | **`49.40 / 49.71 ms`** |
| **1080p @ 48 FPS** | **48.0 FPS** | `0 / 0` | `0.30 ms` | `1.70 + 1.65 ms` | **`0.02 / 0.04 ms`** | `9.22 / 16.92 ms` | `2f` | **`56.74 / 67.20 ms`** |
| **1080p @ 54 FPS** | **54.0 FPS** | `0 / 0` | `0.42 ms` | `4.87 + 2.53 ms` | **`0.12 / 2.11 ms`** | `9.49 / 17.06 ms` | `2f` | **`57.56 / 67.69 ms`** |

---

## 5. Recorded Numbers After Phase 3 (Fine Latency Tuning & Modular Architecture)

Changes in Phase 3:
- **Modular Split:** Server split into `capture_kms`, `convert_egl`, `encode_vaapi`, `net_tx`, `sps_patch`, `kmstreamd`; Client split into `net_rx`, `decode_v4l2`, `display_kms`, `kmstream_pi`.
- **BT.709 Color Accuracy:** SPS VUI rewriter (`server/sps_patch.c`) injects BT.709 limited-range (`primaries=1, transfer=1, matrix=1, full_range=0`) matching the GLES2 RGB->NV12 conversion coefficients.
- **Trailing AUD (`00 00 00 01 09 F0`):** Appended to every encoded frame on the server so `bcm2835-codec` flushes picture $N$ immediately without waiting for packet $N+1$ (`Hold: 0f` in 720p60).
- **Persistent DRM Framebuffer Cache (`client/display_kms.c`):** 16-entry cache indexed by `dmabuf_fd` with automatic invalidation on resolution/format change (`654 hits / 6 misses` in a 12s run, eliminating `drmPrimeFDToHandle` + `drmModeAddFB2` + `drmModeRmFB` + `DRM_IOCTL_GEM_CLOSE` per frame).
- **Atomic Non-Blocking KMS (`DRM_MODE_ATOMIC_NONBLOCK`):** Reduced Pi KMS commit time from `9.20–17.06 ms` down to **`0.18–0.33 ms`**.
- **EGLSync Fence + Modifier-Aware Import (`server/convert_egl.c`):** Replaced `glFinish()` with `EGL_KHR_fence_sync` and added `EGL_EXT_image_dma_buf_import_modifiers`.

| Mode | FPS (Pi) | Loss / IDR | Link RTT | Pi `Net+Rx` | Pi `Dec` | Pi `KMS` | `Hold` | **Phase 3 E2E (avg / p99)** | **Improvement vs Phase 2** |
|---|---|---|---|---|---|---|---|---|---|
| **720p @ 60 FPS** | **60.0 FPS** | `0 / 0` | `0.30 ms` | `0.81 / 0.97 ms` | `11.28 / 11.87 ms` | **`0.18 / 0.25 ms`** | **`0f`** | **`14.45 / 16.39 ms`** | **`-14.20 ms` (`-49.6%`)** |
| **1080p @ 30 FPS** | **30.0 FPS** | `0 / 0` | `0.30 ms` | `1.66 / 1.99 ms` | `0.26 / 0.34 ms` | **`0.32 / 0.42 ms`** | `1f` | **`37.69 / 38.68 ms`** | **`-11.71 ms` (`-23.7%`)** |
| **1080p @ 50 FPS** | **50.0 FPS** | `0 / 0` | `0.31 ms` | `1.39 / 2.07 ms` | `0.17 / 0.32 ms` | **`0.22 / 0.35 ms`** | `2f` | **`44.45 / 46.63 ms`** | **`-12.29 ms` avg / `-20.57 ms` p99** |
| **1080p @ 54 FPS** | **54.0 FPS** | `0 / 0` | `0.29 ms` | `1.70 / 1.93 ms` | `0.26 / 0.63 ms` | **`0.32 / 0.37 ms`** | `2f` | **`41.67 / 42.97 ms`** | **`-15.89 ms` avg / `-24.72 ms` p99** |

### Comparison vs Moonlight / Sunshine on Raspberry Pi 3B
- **720p60:** KMStream achieves **`14.45 ms` software E2E (`< 1 frame` at 60 Hz)** with zero packet loss, zero per-frame DRM ioctl allocations, and direct KMS plane scanout (`DRM_MODE_ATOMIC_NONBLOCK`), outperforming typical Sunshine + Moonlight-embedded/Qt pipelines on Pi 3B (`~22–30 ms` due to PipeWire/portal capture queueing on Wayland + MMAL/V4L2 buffer queueing).
- **1080p50/54:** Because VideoCore IV's H.264 decoder is a 2-stage hardware pipeline (`~18 ms` total across entropy + macroblock reconstruction at `1920x1080`), overlapping the 2 stages yields rock-solid **50–54 FPS** at **`41.67–44.45 ms` E2E** without overclocking (`scripts/pi_tuning.sh --overclock-vpu` configures `core_freq=450` / `h264_freq=333` for 1080p60 headroom).
