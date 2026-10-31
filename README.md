# KMStream

Ultra-low-latency Linux screen streaming tailored for:

- **Server (PC):** AMD RX 580 (`radeonsi` VAAPI H.264), KDE Plasma Wayland, Arch Linux (`x86_64`)
- **Client (Raspberry Pi 3B):** Raspberry Pi OS 64-bit (Debian 13 `trixie`, `aarch64`), `h264_v4l2m2m` (`bcm2835-codec`), KMS/DRM direct plane scanout
- **Link:** Direct 100 Mbps USB-Ethernet (`10.42.0.1` <-> `10.42.0.92`)

Zero-copy pipeline on both sides:

```
PC KMS Primary+Cursor Planes (DMA-BUF)
  -> EGL/GLES2 RGB-to-NV12 + Cursor Compositing
  -> VAAPI H.264 Encoder (Constrained Baseline, Zero B-frames)
  -> SPS VUI Bitstream Rewriter (max_num_reorder_frames=0, max_dec_frame_buffering=1)
  -> UDP Chunks (Port 5000)
  -> Pi V4L2 M2M Hardware Decoder (/dev/video10, DRM PRIME)
  -> KMS Plane Scanout (/dev/dri/card0)
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

The `Makefile` auto-detects the host architecture (`x86_64` builds `server`, `aarch64` builds `client`):

```bash
# On PC -> builds build/kmstreamd
make

# Optional: grant CAP_SYS_ADMIN so kmstreamd can run without sudo
make caps

# On Raspberry Pi -> builds build/kmstream-pi
make client
```

### Deploy & Build on the Pi from PC
```bash
./scripts/deploy_pi.sh
# Or deploy, build, and launch immediately:
./scripts/deploy_pi.sh --run
```

## Usage

1. **Start the client on the Raspberry Pi:**
   ```bash
   ./build/kmstream-pi
   ```
   Optional system tuning on the Pi (CPU governor `performance` + UDP buffer limits):
   ```bash
   sudo ./scripts/pi_tuning.sh
   ```

2. **Start the server on the PC:**
   ```bash
   # 720p60 mode:
   sudo ./build/kmstreamd 10.42.0.92 /dev/dri/card1 720

   # 1080p60 mode (default):
   sudo ./build/kmstreamd 10.42.0.92 /dev/dri/card1 1080

   # Debug: dump patched H.264 bitstream for N frames:
   sudo ./build/kmstreamd --dump sample.h264 --frames 120 10.42.0.92 /dev/dri/card1 720
   ```
