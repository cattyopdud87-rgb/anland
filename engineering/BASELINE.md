# ANLAND BASELINE PERFORMANCE & TELEMETRY REPORT

**Date:** 2026-11-09  
**Target Repository:** `/data/data/com.termux/files/home/development/repo/anland/`  
**Git Commit:** `a23ccd8` (branch: `legacy`)  
**Hardware:** Xiaomi Redmi Note 10 Pro (Snapdragon 732G, 8 cores: 2x Kryo 470 Gold @ 2.3 GHz + 6x Kryo 470 Silver @ 1.8 GHz)  
**Host Environment:** Android 15 (LineageOS Reforge), Kernel 4.14.357-reforge  
**Container Environment:** Arch Linux ARM64 container (`mikasa`), KDE Plasma 6.8, PipeWire 1.6.10  

---

## 1. Baseline System & Hardware State

### 1.1 Memory & Swap Status
- **Physical RAM (MemTotal):** 5,654,864 kB (≈ 5.4 GiB)
- **MemAvailable:** 962,400 kB (17.0% headroom)
- **SwapTotal (ZRAM):** 3,145,724 kB (3.0 GiB)
- **SwapFree:** 14,872 kB (<0.5% headroom — **ZRAM is 99.5% saturated**)
- **AllocStalls / Kswapd churn:** Under concurrent loads (Android apps + container desktop + voice assistant), kernel memory pressure generates allocator stalls and triggers Android LMK reaps.

### 1.2 CPU & Core Topology
- Cores 0–5: Kryo 470 Silver (Cortex-A55 @ 1.80 GHz) — container and background task placement.
- Cores 6–7: Kryo 470 Gold (Cortex-A76 @ 2.30 GHz) — Android UI and on-device speech model inference.
- Hardware ISA: `fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp`.

---

## 2. Baseline Audio IPC Performance Measurements

### 2.1 Benchmark Methodology
- **Workload:** 50,000 discrete audio PCM packet transmissions between producer and consumer threads.
- **Packet Dimensions:** 1,920 bytes payload per packet (standard 48,000 Hz, stereo 16-bit, 20 ms buffer frame).
- **Transport:** Unix Domain Socketpair (`AF_UNIX`, `SOCK_SEQPACKET`).
- **Telemetry Collection:** High-resolution monotonic timers (`clock_gettime(CLOCK_MONOTONIC)`), sorting, and statistical percentile analysis (Min, Mean, P50, P95, P99, Max).

### 2.2 Baseline Measurement Results
```
=== Baseline Audio IPC Benchmark (Unix Domain Socketpair SEQPACKET) ===
Iterations: 50,000 packets | Packet PCM Payload: 1,920 bytes

Total Elapsed Time:   0.2185 seconds
Throughput:           420.81 MB/s (228,867.4 packets/sec)
Latency (Min):        3.75 us
Latency (Mean):       299.13 us
Latency (P50/Median): 297.50 us
Latency (P95):        666.56 us
Latency (P99):        782.08 us
Latency (Max):        886.62 us
Kernel Syscalls:      100,000 (sendmsg + recv pairs)
```

### 2.3 Analysis of Baseline Deficiencies
1. **Severe Tail Latency:** P95 latency reaches **666.56 us** and P99 latency reaches **782.08 us**, representing a 2.6x elongation over median latency. This is caused by kernel scheduler preemption, socket buffer copying, and kernel locking in the network subsystem.
2. **Buffering Bloat:** Because socket IPC is vulnerable to buffer jitter, `native_audio.c` hardcodes a **160 ms ring buffer** (`PLAY_RING_MS = 160`) and **100 ms poll cadence** (`PLAY_POLL_MS = 100`). This adds unacceptable delay to interactive audio and speech-assistant conversations.
3. **Static Allocation Waste:** Microphone capture ring buffer (`MIC_RING_BYTES`) is statically sized at 192 KB even when capturing mono audio, wasting 96 KB of unswappable memory per session.
4. **Permissive SELinux Dependency:** Android 15 introduces `untrusted_app_35`. Without corresponding policy rules in `magisk_module/sepolicy.rule`, all audio and display IPC generates AVC denials and forces permissive operation.
