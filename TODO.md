# KMStream — TODO

Goal: lowest possible glass-to-glass latency, tailor-made for
**PC** (RX 580 / radeonsi VAAPI, KDE Plasma Wayland, Arch) → **Raspberry Pi 3B** (Pi OS 64-bit trixie, bcm2835-codec)
over a direct 100 Mbit USB-Ethernet link (10.42.0.1 ↔ 10.42.0.92).
Scope for now: **video only** (audio/input later).

Workflow per phase: plan → subagent review → approval → implement → test on hardware → check boxes.

---

## Phase 0 — Project organization (minimal behavior change)
- [x] `.gitignore` (binaries, build dirs), `.editorconfig` (LF, 4 spaces)
- [x] Convert sources CRLF → LF
- [x] Remove committed binary `server/kmstreamd-live` from git
- [x] `common/protocol.h` designed upfront: video header (frame_id, chunk idx/total, flags, capture timestamp, per-stage timings) + back-channel messages (IDR request, NACK, clock ping/pong, stats)
- [x] Light split only: extract `sps_patch.c` and `protocol.h`; full module split deferred until network is stable
- [x] Translate comments/log strings to English
- [x] `Makefile` with `pkg-config`, `-O2 -Wall -Wextra`, `-march=native` (PC) / `-mcpu=cortex-a53` (Pi), targets `server`, `client`
- [x] `scripts/deploy_pi.sh`: rsync client to Pi + remote build (SSH key auth)
- [x] `scripts/pi_tuning.sh`: CPU governor `performance`, `rmem_max`, check `core_freq`/CMA
- [x] `README.md`: dependencies, build, run
- [x] Error checking on all init calls (fail fast with clear messages)
- [x] Validate: 720p behaves exactly as before

## Phase 1 — Latency instrumentation (720p baseline)
- [x] Fill timestamps/timings in the video header
- [x] Clock offset estimation PC↔Pi (ping/pong over back-channel)
- [x] Per-stage stats: capture, convert, encode, send, network, reassembly, decode, display
- [x] Periodic stats log (avg / p99) on both sides + loss/drop counters
- [x] Optional: camera/photodiode glass-to-glass procedure in `docs/`
- [x] Baseline numbers recorded for 720p (1080p after Phase 2)

## Phase 2 — Network & pipeline robustness (expected to fix 1080p)
- [x] Client: dedicated high-priority RX thread + preallocated frame ring (decoupled from decode/display)
- [x] Client: chunk bitmap (ignore duplicates, detect loss incl. last chunk)
- [x] Client: `SO_RCVBUF` 2–4 MB, verified with `getsockopt`
- [x] Client: decode wait deadline adapted to resolution (1080p decode may exceed 4.5 ms)
- [x] Client: keep displayed `AVFrame` referenced until next flip; fix GEM handle lifetime
- [x] Client: `num_capture_buffers` ≥ 4–5 (KMS holds buffers)
- [x] Buffer limits: server `patched_buf` and client `MAX_CHUNKS` ≥ 2 MB or explicit overflow handling
- [x] Server: packet pacing computed from target rate (`chunk_bits / rate`, ~120 µs @ 85 Mbps)
- [x] Server: handle `sendto` errors (`ENOBUFS`), evaluate qdisc/`SO_PRIORITY` on the link
- [x] Encoder rate control: CBR / capped VBR (`b≈25M`, `maxrate≈30M`, `bufsize≈1 frame`), configurable
- [x] Back-channel: client requests IDR on loss; server forces IDR
- [x] Infinite GOP (IDR only on demand) once recovery works
- [x] Evaluate multi-slice encoding (`slices=4`); intra-refresh is low priority (weak support on VCE 3.4)
- [x] Validate: 1080p stable, `netstat -su` receive buffer errors not increasing; record 1080p baseline

## Phase 3 — Fine latency tuning
- [x] Full module split (server: capture/convert/encode/net; client: net/decode/display)
- [x] Server: capture synced to vblank, skip unchanged FB
- [x] Server: 100 Hz source vs 60 Hz output cadence strategy
- [x] Server: EGL sync fence instead of `glFinish`
- [x] Server: DRM format modifiers on EGL import
- [x] Client: atomic non-blocking KMS commit with page-flip events
- [x] Client: `drmModeAddFB2WithModifiers` using V4L2 frame modifier
- [x] Client: cache GEM handles/FBs per decoder buffer (no per-frame churn)
- [x] Client: event-driven dequeue instead of 100 µs polling
- [x] Pi 1080p60 decode headroom: test `core_freq`/CMA settings, H.264 level 4.2
- [x] PC: GPU DPM profile during streaming
- [x] Compare against Moonlight/Sunshine baseline

## Phase 4 — Lean Release & Lifecycle Hardening (Screen Mirroring)
- [x] Compile-time telemetry toggle (`KMSTREAM_STATS=0` default Release, `make stats=1` / `./scripts/deploy_pi.sh --stats` for telemetry & debug flags)
- [x] Compact 16-byte `kms_video_hdr` in Release mode (32-byte header and `PING`/`PONG` clock sync compiled only when `KMSTREAM_STATS=1`)
- [x] Release compiler flags (`-O3 -DNDEBUG -flto -s`) with zero periodic `stdout` I/O in hot loops
- [x] Clean `SIGINT` / `SIGTERM` shutdown on both server and client (disable KMS plane and release resources on exit)
- [x] Automatic KMS plane disable after ~2s idle timeout on Pi, plus seamless decoder re-init on server restart / resolution switch (`720p <-> 1080p`)
- [x] Validate Release and `stats=1` builds on PC and Pi hardware

## Phase 5 — Direct V4L2 M2M (Zero FFmpeg on Pi), DRM Page-Flip Mailbox & Client-Driven Session Control
- [x] Protocol & CLI inversion: client (`kmstream-pi <SERVER_IP> [-720|-1080] [-fps N] [-bitrate M]`) initiates and configures session via `KMS_CTRL_START` / `KMS_CTRL_STOP` / `KMS_CTRL_KEEPALIVE`; `kmstreamd` runs idle at 0% CPU/GPU until requested
- [x] Server lifecycle cleanup (`vaapi_enc_cleanup`, `egl_convert_cleanup`) for leak-free client connect/disconnect/reconfigure cycles
- [x] Client: remove `libavcodec`/`libavutil` dependency (`CLIENT_PKGS := libdrm`); implement direct `/dev/video10` (`bcm2835-codec-decode`) V4L2 M2M (`NV12`, DRC `V4L2_EVENT_SOURCE_CHANGE`)
- [x] Client: `SCHED_FIFO` (priority 45) event thread (`poll` on `v4l2_fd` and `drm_fd`) + non-blocking `DRM_MODE_PAGE_FLIP_EVENT` mailbox (`staged_frame_idx`) on Primary Plane (`NV12`, restoring `fbcon` on exit)
- [ ] Phase 5.1 — Sunshine/Moonlight Parity, 100Hz->60FPS Pacing Fix, VA-API Rate Control & Rich Telemetry/Logs:
  - [x] Rich startup & session hardware/display logs on both Server and Client (DRM driver, connector, active resolution & exact refresh rate in Hz, plane IDs, formats/modifiers, VAAPI vendor/profile/RC/VBV, V4L2 geometry & buffers)
  - [x] Fix 100 Hz monitor vs 60 FPS server pacing bug: replace `net_tx_wait_vblank` with monotonic `net_tx_pace_frame` (`ppoll` on `ctrl_sock` up to `deadline_us`)
  - [x] Sunshine-matched VAAPI & EGL quality/latency: `AMD_DEBUG=lowlatencyenc`, `VAProfileH264High` (`profile=high`), `compression_level=32 (speed)`, `slices=1`, `async_depth=1`, `sei=0`, BT.709 colorimetry, 2-tap horizontal chroma filter in `convert_egl.c`
  - [x] Configurable VA-API Rate Control from client CLI (`-rc cbr|vbr|cqp`, `-qp N`, `-strict-rc`), defaulting to CBR with standard driver VBV
  - [x] Fix `1080p60` stall on Pi: 4 `OUTPUT_MPLANE` buffers, 200 ms IDR request cooldown, never block P-frames (`waiting_for_idr`) after first IDR
  - [x] Full telemetry in `stats=1` (server `FPS`, `dt avg/p99`, `Bitrate Mbps`, `Frame KB`, `Cap/Conv/Enc/TX`; client `RX FPS`, `Flip FPS`, `V4L2 Dec avg/p99`, `KMS FlipWait avg/p99`, `E2E avg/p99`, `MboxSkip`, `RxDrop`, `Loss`, `IDR reqs`)
  - [ ] Validate on hardware (`720p60` and `1080p60`, `stats=1` and Release) against Sunshine + `moonlight-embedded-rpi3-v4l2`

## Later (out of current scope)
- [ ] Input (keyboard/mouse/gamepad) back-channel
- [ ] Audio (low-latency Opus or raw PCM)
- [ ] systemd units / autostart on Pi
