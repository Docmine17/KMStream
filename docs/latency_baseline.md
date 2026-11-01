# KMStream — Latency Baseline & Measurement Guide

## 1. Software Pipeline Stages

Every frame carries a 32-byte `struct kms_video_hdr` with timestamps sampled from `CLOCK_MONOTONIC_RAW`:

1. **`Conv` (PC):** KMS primary/cursor plane query, DMA-BUF import into EGL, GLES2 RGB-to-NV12 + cursor compositing shaders, and `glFinish()`.
2. **`Enc` (PC):** `h264_vaapi` hardware encoding (`avcodec_send_frame` -> `avcodec_receive_packet`) + SPS VUI bitstream rewrite on IDR frames.
3. **`TX` (PC):** Packetization into 1400-byte payload chunks and `sendto()` syscalls.
4. **`Net+Rx` (Link + Pi):**
   - `Net`: One-way wire + kernel RX wakeup delay (`t_rx_first_us + clock_offset_us - capture_ts_us - Conv - Enc`).
   - `Rx` (Reassembly): Time between the first chunk and the last chunk of a frame arriving in userspace.
5. **`Dec` (Pi):** `h264_v4l2m2m` (`/dev/video10`) hardware decode wait time.
6. **`KMS` (Pi):** DRM PRIME FD-to-GEM handle import, `drmModeAddFB2`, and `drmModeSetPlane`.
7. **`E2E` (Total Software Glass-to-Glass Estimate):**
   - `t_drm_end (Pi, converted to PC clock) - capture_ts_us (of the actual displayed frame PTS)`.
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

### Key Findings from Phase 1 Telemetry
1. **PC pipeline is sub-millisecond (`~0.85 ms` total for capture + GLES + VAAPI + UDP TX)** and physical network one-way delay is **`~0.19 ms`** (`0.38 ms` RTT).
2. **Three bottlenecks on the Pi account for >95% of total latency (`27–44 ms`):**
   - **`Hold: 1f` (`+16.67 ms`):** `wait_deadline = 4.5 ms` expires just before `bcm2835-codec` finishes `cur_frame_id` (or V4L2 needs 1 extra output/capture buffer cue), so every iteration displays `cur_frame_id - 1`.
   - **Synchronous `drmModeSetPlane` (`+5.5 to 12.0 ms`):** blocks the single thread until the next 60 Hz HDMI vblank.
   - **Single-thread socket queueing (`+0 to 10.7 ms`):** while the thread is blocked for `4.6 ms (Dec) + 12.0 ms (KMS) = 16.6 ms`, the next frame arrives from the PC and sits in the kernel UDP socket buffer for up to `10.7 ms` before `recvfrom()` is called (also delaying `PONG` reception).

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

> **Hardware Note on 1080p60:** At default Raspberry Pi 3B clocks (`core_freq=400 MHz`, `h264_freq=250 MHz`), the VideoCore IV hardware H.264 block reaches a maximum physical throughput of **55.5 FPS** at `1920x1080` (~452k macroblocks/s). Default 1080p cadence is set to **50 FPS** so it runs indefinitely without queue buildup at stock clocks; 60 FPS at 1080p will be tested with VPU clock tuning (`h264_freq=333`, `core_freq=450`) in Phase 3.
