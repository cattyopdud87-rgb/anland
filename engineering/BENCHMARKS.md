# ANLAND BENCHMARKS & EXPERIMENTAL VERIFICATION REPORT

**Repository:** `/data/data/com.termux/files/home/development/repo/anland/`  
**Test Hardware:** Qualcomm SM6150 (Snapdragon 732G) | Android 15 | Arch Linux ARM64 container (`mikasa`)  
**Date:** 2026-11-09  

---

## 1. Benchmark Methodology

To evaluate IPC overhead and latency without introducing measurement noise:
1. **Benchmark Tooling:** Native C benchmarks compiled with `-O3 -pthread` using Clang 21.1.8 on ARM64:
   - Baseline: `engineering/benchmarks/benchmark_audio_ipc.c`
   - Optimized: `engineering/benchmarks/benchmark_shm_audio.c`
2. **Workload:** 50,000 iterations transmitting 1,920-byte PCM audio blocks (48 kHz stereo 16-bit, 20 ms audio frame).
3. **Telemetry:** Nanosecond-resolution monotonic timestamps (`clock_gettime(CLOCK_MONOTONIC)`) measuring roundtrip transfer time for every single packet.
4. **Statistical Analysis:** Sorting full sample arrays to calculate Min, Mean, Median (P50), P95, P99, and Max latency percentiles.

---

## 2. Comparative Benchmark Results

```
+------------------------------------+--------------------------+---------------------------+---------------------+
| Performance Metric                | Baseline (AF_UNIX Sock)  | Optimized (Lock-Free SHM) | Relative Impact     |
+------------------------------------+--------------------------+---------------------------+---------------------+
| Total Elapsed Time (50k packets)   | 0.2185 s                 | 0.0313 s                  | 6.98x faster        |
| Useful Throughput                  | 420.81 MB/s              | 2,923.48 MB/s             | 6.95x higher        |
| Packet Processing Rate             | 228,867 packets/sec      | 1,596,610 packets/sec     | 6.98x higher        |
| Latency (Minimum)                  | 3.75 us                  | 0.57 us                   | 6.58x lower         |
| Latency (Mean)                     | 299.13 us                | 39.61 us                  | 7.55x lower         |
| Latency (Median / P50)             | 297.50 us                | 38.59 us                  | 7.71x lower         |
| Latency (P95 Tail)                 | 666.56 us                | 43.02 us                  | 15.49x lower        |
| Latency (P99 Tail)                 | 782.08 us                | 58.96 us                  | 13.26x lower        |
| Latency (Worst-case Max)           | 886.62 us                | 1068.02 us (cold init)    | Comparable max      |
| Kernel Syscalls in Streaming Path  | 100,000 (sendmsg + recv) | 0 syscalls                | 100% kernel bypass  |
+------------------------------------+--------------------------+---------------------------+---------------------+
```

---

## 3. Performance & Efficiency Analysis

1. **Jitter Elimination:**
   Under the baseline socket implementation, 5% of packets experienced latencies above **666 us** due to socket lock contention and scheduler context switches. Under the lock-free shared memory ring buffer, P95 latency is **43.02 us** — virtually eliminating audio packet jitter.
2. **Buffering Delay Slashed:**
   Because transmission jitter is reduced by over 15x, the consumer-side AAudio buffer (`PLAY_RING_MS`) can safely run at **48 ms** instead of **160 ms**, and poll intervals can drop from **100 ms to 20 ms**, shaving **112 ms of dead buffering delay** from the system.
3. **CPU Utilization:**
   Bypassing the kernel network stack removes 100,000 kernel entries per 50,000 frames, keeping CPU core temperature low and freeing cycles on the Silver cluster (Cores 0-5) for container desktop rendering and speech AI tasks.
