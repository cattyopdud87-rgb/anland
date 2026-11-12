# ANLAND REGRESSION & VALIDATION REPORT

**Repository:** `/data/data/com.termux/files/home/development/repo/anland/`  
**Date:** 2026-11-09  

---

## 1. Test Matrix & Results

| Test ID | Scope | Target Component | Description / Assertion | Result |
| :--- | :--- | :--- | :--- | :--- |
| **REG-01** | Wire ABI | `common/protocol.h` | Verified `sizeof(struct InputEvent) == 20`, `sizeof(struct audio_format) == 20`, and `sizeof(struct audio_msg) == 8`. | **PASS** |
| **REG-02** | Alignment | `protocol.h` | Verified `struct anland_shm_audio_header` 64-byte alignment (`write_head`, `read_tail`, `data[]`). | **PASS** |
| **REG-03** | Unit Test | `tests/tablet_test.c` | Verified tablet input protocol framing, tool identity, interleaving, and disconnect handling. | **PASS** |
| **REG-04** | Concurrency | `tests/test_shm_audio.c` | Concurrent multi-threaded SPSC streaming (100,000 frames) with boundary wrap-around: 0 corruptions, 0 errors. | **PASS** |
| **REG-05** | Fallback | `anland_audio.c` | Verified automatic fallback to `sendmsg()` datagrams if consumer does not request SHM transport. | **PASS** |
| **REG-06** | Teardown | `native_audio.c` | Verified clean unmapping and descriptor closure on connection drop, stream stop, and app exit. | **PASS** |
| **REG-07** | Build | `CMakeLists.txt` | Clean build of `display_daemon`, `display_consumer`, `display_daemon_lib`, `socket_utils`, and benchmarks. | **PASS** |
| **REG-08** | Producer | `libdisplay_producer` | Clean build and link of `libdisplay_producer.so` against `libpipewire-0.3`. | **PASS** |
| **REG-09** | Container | `mikasa` | Verified `anland-speaker` and `anland-mic` nodes are actively registered in PipeWire inside container. | **PASS** |

---

## 2. Regressions Checked and Ruled Out

1. **Deadlock / Race Conditions:**
   - The SPSC shared-memory ring buffer utilizes single-producer single-consumer lock-free semantics with `memory_order_release` and `memory_order_acquire` memory barriers. No mutex locks are held in the hot streaming path, completely eliminating deadlocks.
2. **Buffer Overrun / Memory Corruption:**
   - Ring buffer bounds checking ensures writes never exceed `buffer_size - 1 - used`. Saturated conditions drop oldest frames cleanly without overflowing memory.
3. **Cross-Architecture Portability:**
   - Replaced direct `memfd_create` calls with `anland_memfd_create()` syscall wrapper, guaranteeing that compilation succeeds whether building on Android NDK, Termux, or desktop Linux.
