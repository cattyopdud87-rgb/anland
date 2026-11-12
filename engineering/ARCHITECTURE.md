# ANLAND ARCHITECTURE & BOTTLENECK AUDIT

**Date:** 2026-11-09  
**Repository:** `/data/data/com.termux/files/home/development/repo/anland/`  

---

## 1. System Role & Topology

`anland` is the ultra-low-latency display and audio bridge connecting Linux container Wayland compositors (KWin, Mutter, Hyprland, Niri, Mango, Gamescope) running inside Droidspaces containers (`mikasa`) to Android host apps (`com.anland.consumer`, `com.anlandnext`).

```
+---------------------------------------------------------------------------------------+
| LINUX CONTAINER (Arch ARM64 - mikasa)                                                 |
|                                                                                       |
|   [Wayland Compositor: KWin / Mutter]       [PipeWire 1.6 / WirePlumber]              |
|                   |                                       |                           |
|       +-----------v------------+             +------------v------------+              |
|       |  libdisplay_producer   |             |      anland_audio       |              |
|       | (anland_present/scene) |             | (Virtual Sink / Source) |              |
|       +-----------+------------+             +------------+------------+              |
|                   |                                       |                           |
+-------------------|---------------------------------------|---------------------------+
                    | (UNIX Domain Socket)                  | (audio_fd socketpair)
                    v                                       v
+---------------------------------------------------------------------------------------+
| ANDROID HOST (LineageOS Reforge - Android 15 / SM6150)                                |
|                                                                                       |
|   +------------------------------------+                                              |
|   |   display_daemon (Daemon Broker)   |                                              |
|   |   Relays 5 core fds:               |                                              |
|   |   [buf_ready_efd, fence_fd,        |                                              |
|   |    data_fd, shm_fd, audio_fd]      |                                              |
|   +-----------------+------------------+                                              |
|                     |                                                                 |
|   +-----------------v------------------+     +------------------------------------+   |
|   | libdisplay_consumer (JNI Engine)   |     |    native_audio (AAudio Player)    |   |
|   | Owns AHardwareBuffer dmabufs &     |     |    Reads/writes audio_fd socket    |   |
|   | Event pump                         |     |    Feeds Android Audio HAL / ALSA  |   |
|   +-----------------+------------------+     +-----------------+------------------+   |
|                     |                                          |                      |
|           [SurfaceFlinger / GL]                        [audioserver / QDSP6]          |
+---------------------------------------------------------------------------------------+
```

---

## 2. Component Analysis

### 2.1 `common/protocol.h` & `common/socket_utils.[ch]`
- Defines binary message frames (`ctrl_msg`, `data_msg`, `audio_msg`, `InputEvent`, `OutputEvent`).
- Control phase exchanges:
  - `CTRL_MSG_CONSUMER_HELLO` (1): Consumer passes 5 descriptors to daemon.
  - `CTRL_MSG_PRODUCER_HELLO` (2): Producer registers with daemon.
  - `CTRL_MSG_SCREEN_INFO` (7): Screen dimensions, format, refresh rate.
  - `CTRL_MSG_PICKUP_FDS` (9) / `CTRL_MSG_FDS_READY` (10): Producer retrieves the 5 descriptors.
- Data phase exchanges:
  - `DATA_MSG_BUFS_READY` (200): Consumer hands over AHardwareBuffer DMABUFs.
  - `DATA_MSG_BUF_READY` (100) / `buf_ready_efd`: Signals buffer release.
  - `fence_fd`: Frame render completion fence.
- Audio phase:
  - Dedicated socketpair `audio_fd` (slot 4 of hello fds).
  - Lines 235-238 declare:
    - `AUDIO_MSG_FORMAT` (1)
    - `AUDIO_MSG_PCM` (2)
    - `AUDIO_MSG_SHM` (3)
    - `AUDIO_MSG_SHM_FD` (4)

### 2.2 `libdisplay_producer` (Container Side)
- Linked into container compositors.
- `display_producer.c`: Orchestrates connection, fd reception, DMABUF mapping, presentation loop.
- `anland_audio.c`:
  - Spins a dedicated PipeWire thread loop (`pw_thread_loop`).
  - Registers `anland-speaker` (`Audio/Sink`) and `anland-mic` (`Audio/Source`).
  - Playback path: `on_capture_process` dequeues PipeWire PCM buffer, packages it into `audio_msg` with `AUDIO_MSG_PCM`, and calls `sendmsg(a->audio_fd, &m, MSG_DONTWAIT | MSG_NOSIGNAL)`.
  - Capture path: `on_audio_readable` reads `audio_msg` packets from `audio_fd` via `recv()` and writes to `a->ring` buffer; `on_source_process` dequeues from `a->ring` to feed the virtual mic.
- `anland_pacing.c`: Moving average frame pacing logic.

### 2.3 `libdisplay_consumer` & `consumers/anland_v5/android_consumer` (Android Host Side)
- `display_consumer.c`: Allocates eventfds, socketpairs, and memfd (`create_shm`), and deposits them to `display_daemon`.
- `native_audio.c`:
  - Starts two POSIX threads: `play_thread` and `cap_thread`.
  - Opens AAudio output and input streams.
  - Reads PCM from `audio_fd` socket via `recv()` inside `play_thread`, buffering into `b->play_ring`.
  - `PLAY_RING_MS` is set to 160 ms! `PLAY_POLL_MS` is 100 ms!
  - Microphone capture writes PCM packets through `send()` to `audio_fd`.

---

## 3. Discovered Engineering Bottlenecks & Optimization Targets

### Bottleneck A: Audio IPC Latency & Socket Copy Thrashing (P0 - Critical)
1. **The Problem:** Audio data currently traverses 3 separate userspace/kernel boundaries over a Unix domain socket:
   - PipeWire buffer -> userspace stack `audio_msg`
   - Kernel socket buffer (`sendmsg` -> socket queue -> `recv`)
   - Consumer userspace buffer -> AAudio ring buffer -> AudioFlinger
2. **Impact on Hardware:** Under heavy combined load (Android host + Arch Linux container + on-device speech LLM), socket queuing induces context-switch storms and jitter. The conservative 160 ms ring buffer adds massive perceived latency, conflicting with the on-device voice assistant objective (which requires <50 ms roundtrip).
3. **The Solution:** Implement the dormant `AUDIO_MSG_SHM` / `AUDIO_MSG_SHM_FD` protocol. Create a lock-free, cache-aligned single-producer single-consumer (SPSC) shared-memory circular ring buffer backed by `memfd_create`. Audio frames write directly into shared memory with atomic head/tail pointers, eliminating socket copy overhead, reducing kernel context switches, and slashing playback latency from 160 ms down to 24–48 ms!

### Bottleneck B: Memory Footprint & Static Allocation in `anland_audio.c`
1. **The Problem:** `MIC_RING_BYTES` is statically hardcoded to `(48000 * 2 * sizeof(int16_t))` = 192 KB, even when negotiated as a mono capture stream (96 KB).
2. **Impact on Hardware:** The device suffers from acute memory pressure (only 14 MB free in ZRAM swap!). Unnecessary allocations and static peak sizing directly contribute to memory churn.
3. **The Solution:** Dynamically size the mic ring buffer based on negotiated channel count and rate. Provide an adaptive ring size configuration.

### Bottleneck C: Android 15 SDK 35 SELinux Policy Gaps
1. **The Problem:** `magisk_module/sepolicy.rule` only grants rules for `untrusted_app` and `untrusted_app_34`. On Android 15, apps targeting SDK 35 run in the `untrusted_app_35` domain.
2. **Impact:** System log is flooded with audit denials whenever `com.anland.consumer` (running under `untrusted_app_35`) communicates with `droidspacesd`, forcing the device into permissive mode.
3. **The Solution:** Update `magisk_module/sepolicy.rule` to include complete `untrusted_app_35` rules for fd passing, socket access, and graphics allocator.

### Bottleneck D: Build System Portability & Cross-Environment Usability
1. **The Problem:** `display_consumer.c` uses `memfd_create`, which Bionic restricts based on `__ANDROID_API__ >= 30`. Default builds on Termux or older toolchains fail with compilation errors.
2. **Impact:** Prevents clean automated builds and testing across host environments and CI/CD pipelines.
3. **The Solution:** Implement transparent fallback to `syscall(__NR_memfd_create, ...)` when `memfd_create` is unavailable in the toolchain C library, ensuring 100% build portability across Android NDK, Termux, and desktop Linux.

### Bottleneck E: Ecosystem Interoperability & Automated Cleanup
1. **The Problem:** `display_daemon` and `display_consumer` leave orphaned socket files (`anland-*.sock`) in `/data/local/Droidspaces/Net` upon abnormal termination, requiring manual intervention or cluttering the container net namespace (24 stale sockets observed!).
2. **The Solution:** Add socket stale-detection and automatic unlinking hooks on daemon initialization and shutdown, and provide ecosystem helper scripts and C APIs for clean container lifecycle integration.
