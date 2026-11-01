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
- [x] Move reference PDF to `docs/`
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

## Later (out of current scope)
- [ ] Input (keyboard/mouse/gamepad) back-channel
- [ ] Audio (low-latency Opus or raw PCM)
- [ ] systemd units / autostart on Pi
