# KMStream

Ultra-low-latency Linux screen streaming tailored for:

- **Server (PC):** AMD RX 580 (`radeonsi` VAAPI H.264), KDE Plasma Wayland, Arch Linux (`x86_64`)
- **Client (Raspberry Pi 3B):** Raspberry Pi OS 64-bit (Debian 13 `trixie`, `aarch64`), `h264_v4l2m2m` (`bcm2835-codec`), KMS/DRM atomic plane scanout (`vc4-kms-v3d`)
- **Link:** Direct 100 Mbps USB-Ethernet (`10.42.0.1` <-> `10.42.0.92`, ~`0.30 ms` RTT)

## Zero-Copy Pipeline

```text
PC KMS Primary + Cursor Planes (DMA-BUF + Format Modifiers)
  -> EGL/GLES2 RGB-to-NV12 (BT.709 Limited Range) + Cursor Compositing + EGLSyncKHR Fence
  -> VAAPI H.264 Encoder (Constrained Baseline, CQP / VBR, Zero B-frames, Trailing AUD)
  -> SPS VUI Bitstream Rewriter (BT.709, max_num_reorder_frames=0, max_dec_frame_buffering=1)
  -> Paced UDP Video Chunks (Port 5000) + Control Back-Channel (Port 5001: PING/PONG & IDR Recovery)
  -> Pi Dedicated RX Thread (8 MB SO_RCVBUF, 8-Slot Chunk-Bitmap Ring)
  -> Pi V4L2 M2M Hardware Decoder (/dev/video10, DRM PRIME DMA-BUF)
  -> Persistent 16-Entry DRM Framebuffer Cache + Non-Blocking Atomic KMS Commit (/dev/dri/card0)
```

## Project Layout

```text
common/
  protocol.h        # Wire protocol v1 (32-byte video header + UDP:5001 control messages)
  stats.h           # Zero-allocation rolling average and p99 latency statistics
server/
  kmstreamd.c       # Server CLI, main cadence loop, and telemetry
  capture_kms.c/.h  # KMS plane discovery, atomic cursor coordinates, DMA-BUF export
  convert_egl.c/.h  # GBM/EGL/GLES2 NV12 shader pipeline + modifier import + EGLSyncKHR
  encode_vaapi.c/.h # VAAPI H.264 encoding, trailing AUD injection, on-demand IDR
  sps_patch.c/.h    # Exp-Golomb H.264 SPS VUI rewriter (BT.709 + zero-reorder restrictions)
  net_tx.c/.h       # Paced UDP transmitter (~80 Mbps burst ceiling) + control socket handler
client/
  kmstream_pi.c     # Client main loop and end-to-end telemetry
  net_rx.c/.h       # Dedicated RX thread, 8-slot bitmap reassembly, clock sync, IDR requests
  decode_v4l2.c/.h  # h264_v4l2m2m DRM PRIME zero-copy decoder wrapper
  display_kms.c/.h  # Persistent dmabuf_fd -> fb_id cache + DRM_MODE_ATOMIC_NONBLOCK display
scripts/
  deploy_pi.sh      # Rsync client sources to Raspberry Pi and build remotely
  pi_tuning.sh      # CPU governor, UDP sysctls, and optional VPU clock profile
docs/
  latency_baseline.md # Detailed per-stage latency benchmarks across Phases 1-3
```

## Dependencies

### PC (Arch Linux)
```bash
sudo pacman -S base-devel pkgconf libdrm mesa libva libva-mesa-driver ffmpeg
```

### Raspberry Pi 3B (Raspberry Pi OS 64-bit)
```bash
sudo apt install build-essential pkg-config rsync libdrm-dev libavcodec-dev libavutil-dev
```

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

# Deploy, build, and launch immediately:
./scripts/deploy_pi.sh --run

# Deploy and build with telemetry enabled (must match `make stats=1` on PC):
./scripts/deploy_pi.sh --stats
```

## Usage

### 1. Prepare and start the client on the Raspberry Pi
```bash
# Apply runtime tuning (CPU governor 'performance' + 4 MB UDP socket buffer sysctls)
sudo ./scripts/pi_tuning.sh

# Optional: append VPU 1080p60 clock profile (core_freq=450, h264_freq=333) to /boot/firmware/config.txt
sudo ./scripts/pi_tuning.sh --overclock-vpu

# Start the client (from a TTY / without a desktop compositor holding DRM master)
# Automatically blanks the KMS plane after 2s idle and reconfigures on 720p <-> 1080p switches.
./build/kmstream-pi
```

### 2. Start the server on the PC
```bash
# 720p @ 60 FPS (default for 720p — lowest latency: ~14.5 ms E2E, 0 held frames):
./build/kmstreamd 10.42.0.92 /dev/dri/card1 720

# 1080p @ 50 FPS (default for 1080p at stock Pi 3B clocks):
./build/kmstreamd 10.42.0.92 /dev/dri/card1 1080

# 1080p @ 54 FPS (maximum sustained rate near the 55.5 FPS stock VideoCore IV limit):
./build/kmstreamd --fps 54 10.42.0.92 /dev/dri/card1 1080

# Optional: use capped VBR (e.g. 20 Mbps) instead of default CQP (24 for 720p, 26 for 1080p):
./build/kmstreamd --bitrate 20 10.42.0.92 /dev/dri/card1 1080

# Debug (requires `make stats=1`): dump patched H.264 Annex-B bitstream for N frames:
./build/kmstreamd --dump sample.h264 --frames 120 10.42.0.92 /dev/dri/card1 720
```

## Measured Latency (PC RX 580 -> Pi 3B)

Full methodology and per-phase breakdown are documented in [`docs/latency_baseline.md`](docs/latency_baseline.md).

| Mode | FPS (Pi) | Packet Loss | PC `Conv+Enc` | Link RTT | Pi `Dec` | Pi `KMS` | `Hold` | **Total Software E2E (avg / p99)** |
|---|---|---|---|---|---|---|---|---|
| **720p @ 60 FPS** | **60.0 FPS** | `0%` | `1.33 ms` | `0.30 ms` | `11.28 ms` | `0.18 ms` | **`0f`** | **`14.45 / 16.39 ms`** |
| **1080p @ 30 FPS** | **30.0 FPS** | `0%` | `2.11 ms` | `0.30 ms` | `0.26 ms` | `0.32 ms` | `1f` | **`37.69 / 38.68 ms`** |
| **1080p @ 50 FPS** | **50.0 FPS** | `0%` | `2.88 ms` | `0.31 ms` | `0.17 ms` | `0.22 ms` | `2f` | **`44.45 / 46.63 ms`** |
| **1080p @ 54 FPS** | **54.0 FPS** | `0%` | `2.36 ms` | `0.29 ms` | `0.26 ms` | `0.32 ms` | `2f` | **`41.67 / 42.97 ms`** |
