# ANLAND BATTERY & POWER CONSUMPTION ANALYSIS

**Repository:** `/data/data/com.termux/files/home/development/repo/anland/`  
**Device:** Xiaomi Redmi Note 10 Pro (Snapdragon 732G)  
**Date:** 2026-11-09  

---

## 1. Electrical Telemetry Baseline

Direct telemetry from `/sys/class/power_supply/battery`:
- Battery Capacity: 59%
- Battery Voltage: 3.77 V (`3,772,454 uV`)
- Current Consumption: ~905 mA (`905,457 uA`) under mixed container + IDE workload

---

## 2. Power Optimization Mechanisms

### 2.1 Sleep & Deep Idle States
- **Idle Timeout Protection:** In `consumers/anland_v5/android_consumer/app/src/main/jni/native_audio.c`, the `PLAY_IDLE_STOP_MS` logic automatically calls `AAudioStream_requestStop()` after 1,500 ms of silence. Stopping the AAudio stream releases the Qualcomm hardware audio DSP (`adsprpcd_audiopd`) and permits the audio power domain to sleep.
- **Wakeup Cadence:** Shifting the streaming path from socket polling to shared-memory circular queue reduces kernel timer interrupts and IPC wakeups, reducing battery drain during media playback.

### 2.2 Memory Compaction & Energy Efficiency
- In resource-constrained devices with heavy ZRAM utilization (only 14 MB free), memory churn forces continuous `kswapd` page compression and decompression.
- Reducing `MIC_RING_BYTES` from 192 KB to 96 KB for mono speech capture saves unswappable memory, directly curbing swap thrashing and CPU power consumption per audio hour.
