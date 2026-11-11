# KMStream

Ultra-low-latency Linux screen streaming tailored for:

- **Server (PC):** AMD RX 580 (`radeonsi` VAAPI H.264), KDE Plasma Wayland, Arch Linux (`x86_64`)
- **Client (Raspberry Pi 3B):** Raspberry Pi OS 64-bit (Debian 13 `trixie`, `aarch64`), direct V4L2 M2M (`bcm2835-codec-decode`, zero FFmpeg on client), KMS/DRM `NV12` primary-plane scanout (`vc4-kms-v3d`)
- **Link:** Direct 100 Mbps USB-Ethernet (`10.42.0.1` <-> `10.42.0.92`, ~`0.30 ms` RTT)

## Zero-Copy Pipeline

```text
PC KMS Primary + Cursor Planes (DMA-BUF + Format Modifiers, 100Hz -> 60FPS Monotonic Pacing)
  -> EGL/GLES2 RGB-to-NV12 (BT.709 Limited Range + 2-Tap Chroma Filter) + Cursor Compositing + EGLSyncKHR
  -> VAAPI H.264 Encoder (High Profile CABAC, speed=32, AMD_DEBUG=lowlatencyenc, CBR/VBR/CQP, Zero B-frames)
  -> SPS VUI Bitstream Rewriter (BT.709, max_num_reorder_frames=0, max_dec_frame_buffering=1)
  -> UDP Video Chunks (Port 5000) + Session & Back-Channel Control (Port 5001)
  -> Pi Dedicated RX Thread (4 MB SO_RCVBUF, 8-Slot Chunk-Bitmap Ring, Zero-Copy Trailing AUD)
  -> Pi Direct V4L2 M2M Decoder (/dev/video10, 4 OUTPUT Buffers -> NV12 CAPTURE DMA-BUFs)
  -> Pi SCHED_FIFO (45) Event Thread + DRM_MODE_PAGE_FLIP_EVENT Staged Mailbox (/dev/dri/card0)
```

## Project Layout

```text
common/
  protocol.h        # Wire protocol v3 (16-byte release / 32-byte stats header + UDP:5001 session control)
  stats.h           # Zero-allocation rolling average and p99 latency statistics
server/
  kmstreamd.c       # Server daemon (0% CPU/GPU when idle, wakes on client KMS_CTRL_START)
  capture_kms.c/.h  # KMS plane capture, connector/Hz discovery, atomic cursor coordinates, DMA-BUF export
  convert_egl.c/.h  # GBM/EGL/GLES2 NV12 shader (2-tap chroma filter) + modifier import + EGLSyncKHR
  encode_vaapi.c/.h # VAAPI H.264 High Profile encoding (CBR/VBR/CQP, speed=32, on-demand IDR)
  sps_patch.c/.h    # Exp-Golomb H.264 SPS VUI rewriter (BT.709 + zero-reorder restrictions)
  net_tx.c/.h       # Monotonic frame pacing + UDP video transmitter + control socket handler
client/
  kmstream_pi.c     # Client CLI, SCHED_FIFO V4L2/DRM event thread, and end-to-end telemetry
  net_rx.c/.h       # Session handshake (START/KEEPALIVE/STOP), RX thread, 8-slot bitmap ring
  decode_v4l2.c/.h  # Direct /dev/video10 V4L2 M2M ioctl engine (NV12, 4 OUTPUT buffers, zero FFmpeg)
  display_kms.c/.h  # Pre-imported NV12 DRM FBs + non-blocking DRM_MODE_PAGE_FLIP_EVENT mailbox
scripts/
  deploy_pi.sh      # Rsync client sources to Raspberry Pi and build remotely
  pi_tuning.sh      # CPU governor, UDP sysctls, and optional VPU clock profile
docs/
  latency_baseline.md # Per-stage latency benchmarks
```

## Dependencies

### PC (Arch Linux)
```bash
sudo pacman -S base-devel pkgconf libdrm mesa libva libva-mesa-driver ffmpeg
```

### Raspberry Pi 3B (Raspberry Pi OS 64-bit)
```bash
sudo apt install build-essential pkg-config rsync libdrm-dev
```
*(No FFmpeg libraries are required on the Raspberry Pi — `kmstream-pi` talks directly to `/dev/video10` and `/dev/dri/card0`.)*

## Build

The `Makefile` auto-detects the host architecture (`x86_64` builds `server`, `aarch64` builds `client`).
By default, `make` builds the **lean Release profile** (`-O3 -DNDEBUG -flto -s`, 16-byte wire header, zero periodic stdout/telemetry overhead).
Pass `stats=1` (or `--stats` to `deploy_pi.sh`) to enable per-stage microsecond telemetry, `PING`/`PONG` clock sync, and `--dump`/`--frames` flags.

```bash
# On PC -> builds lean Release build/kmstreamd
make

# Or build with full latency telemetry enabled (32-byte wire header):
make stats=1

# Grant CAP_SYS_ADMIN so kmstreamd can read KMS framebuffers without sudo
make caps

# On Raspberry Pi -> builds build/kmstream-pi
make client
```

### Deploy & Build on the Pi from PC
```bash
# Deploy and build lean Release client on Pi:
./scripts/deploy_pi.sh

# Deploy and build with telemetry enabled (must match `make stats=1` on PC):
./scripts/deploy_pi.sh --stats
```

## Usage

### 1. Start the Server Daemon on the PC
`kmstreamd` runs idle at **0% CPU / 0% GPU** listening on UDP port `5001` until a client connects and requests a stream:
```bash
sudo ./build/kmstreamd
# Or, after running `make caps`:
./build/kmstreamd
```

### 2. Connect from the Raspberry Pi (`kmstream-pi`)
All stream parameters (resolution, FPS, bitrate, rate control, and server IP) are controlled from the client:
```bash
# Optional runtime tuning on Pi (CPU governor 'performance' + 4 MB UDP socket buffer sysctls):
sudo ./scripts/pi_tuning.sh

# 720p @ 60 FPS, 10 Mbps CBR (default):
./build/kmstream-pi 10.42.0.1 -720 -fps 60 -bitrate 10

# 1080p @ 60 FPS, 15 Mbps CBR:
./build/kmstream-pi 10.42.0.1 -1080 -fps 60 -bitrate 15

# Rate control modes: CBR (default), VBR, or CQP:
./build/kmstream-pi 10.42.0.1 -1080 -fps 60 -bitrate 15 -rc vbr
./build/kmstream-pi 10.42.0.1 -1080 -fps 60 -rc cqp -qp 20

# Optional strict 1-frame VBV cap for CBR/VBR:
./build/kmstream-pi 10.42.0.1 -720 -fps 60 -bitrate 10 -rc cbr -strict-rc
```
Press `Ctrl+C` on the Pi at any time to stop streaming: `kmstream-pi` automatically restores the Linux TTY (`fbcon`) on the HDMI output and sends `KMS_CTRL_STOP` so `kmstreamd` returns to idle on the PC.
