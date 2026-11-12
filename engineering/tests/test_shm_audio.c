#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "protocol.h"

#define TEST_RING_SIZE (64 * 1024)
#define TOTAL_FRAMES 100000

static inline int anland_test_memfd(const char *name) {
#if defined(__NR_memfd_create)
    return (int)syscall(__NR_memfd_create, name, 1 /* MFD_CLOEXEC */);
#else
    return -1;
#endif
}

struct test_ctx {
    struct anland_shm_audio_header *ring;
    volatile bool done;
    uint32_t errors;
};

static void *consumer_worker(void *arg) {
    struct test_ctx *ctx = (struct test_ctx *)arg;
    struct anland_shm_audio_header *ring = ctx->ring;
    uint32_t expected_val = 0;
    uint32_t frames_read = 0;

    while (frames_read < TOTAL_FRAMES) {
        uint32_t head = __atomic_load_n(&ring->write_head, __ATOMIC_ACQUIRE);
        uint32_t tail = __atomic_load_n(&ring->read_tail, __ATOMIC_RELAXED);
        uint32_t avail = (head >= tail) ? (head - tail) : (ring->buffer_size - (tail - head));

        if (avail < sizeof(uint32_t)) {
            if (ctx->done) break;
            usleep(1);
            continue;
        }

        uint32_t val;
        size_t first = ring->buffer_size - tail;
        if (first >= sizeof(uint32_t)) {
            memcpy(&val, ring->data + tail, sizeof(uint32_t));
        } else {
            uint8_t tmp[sizeof(uint32_t)];
            memcpy(tmp, ring->data + tail, first);
            memcpy(tmp + first, ring->data, sizeof(uint32_t) - first);
            memcpy(&val, tmp, sizeof(uint32_t));
        }

        if (val != expected_val) {
            ctx->errors++;
        }
        expected_val++;
        frames_read++;

        uint32_t new_tail = (tail + sizeof(uint32_t)) % ring->buffer_size;
        __atomic_store_n(&ring->read_tail, new_tail, __ATOMIC_RELEASE);
    }

    return NULL;
}

int main(void) {
    printf("--- Testing Anland SHM Audio Protocol & Ring Buffer Invariants ---\n");

    /* 1. Static ABI Invariants */
    assert(sizeof(struct audio_format) == 20);
    assert(sizeof(struct audio_msg) == 8);
    assert(sizeof(struct anland_shm_audio_header) >= 64);
    assert((sizeof(struct anland_shm_audio_header) % 64) == 0);
    printf("Static ABI and alignment checks: PASSED\n");

    /* 2. Anonymous Memfd Allocation */
    int fd = anland_test_memfd("anland_shm_test");
    assert(fd >= 0);

    size_t total_size = sizeof(struct anland_shm_audio_header) + TEST_RING_SIZE;
    assert(ftruncate(fd, total_size) == 0);

    struct anland_shm_audio_header *ring = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(ring != MAP_FAILED);

    memset(ring, 0, total_size);
    ring->magic = ANLAND_SHM_AUDIO_MAGIC;
    ring->version = ANLAND_SHM_AUDIO_VERSION;
    ring->buffer_size = TEST_RING_SIZE;
    ring->rate = 48000;
    ring->channels = 2;
    ring->format = AUDIO_FORMAT_S16LE;
    __atomic_store_n(&ring->write_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ring->read_tail, 0, __ATOMIC_RELEASE);

    struct test_ctx ctx = { .ring = ring, .done = false, .errors = 0 };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, consumer_worker, &ctx) == 0);

    /* Producer writes sequential frame tokens */
    for (uint32_t i = 0; i < TOTAL_FRAMES; i++) {
        while (1) {
            uint32_t head = __atomic_load_n(&ring->write_head, __ATOMIC_RELAXED);
            uint32_t tail = __atomic_load_n(&ring->read_tail, __ATOMIC_ACQUIRE);
            uint32_t used = (head >= tail) ? (head - tail) : (TEST_RING_SIZE - (tail - head));
            uint32_t space = TEST_RING_SIZE - 1 - used;
            if (space >= sizeof(uint32_t)) break;
            usleep(1);
        }

        uint32_t head = __atomic_load_n(&ring->write_head, __ATOMIC_RELAXED);
        size_t first = TEST_RING_SIZE - head;
        if (first >= sizeof(uint32_t)) {
            memcpy(ring->data + head, &i, sizeof(uint32_t));
        } else {
            uint8_t tmp[sizeof(uint32_t)];
            memcpy(tmp, &i, sizeof(uint32_t));
            memcpy(ring->data + head, tmp, first);
            memcpy(ring->data, tmp + first, sizeof(uint32_t) - first);
        }

        uint32_t new_head = (head + sizeof(uint32_t)) % TEST_RING_SIZE;
        __atomic_store_n(&ring->write_head, new_head, __ATOMIC_RELEASE);
    }

    ctx.done = true;
    pthread_join(thread, NULL);

    assert(ctx.errors == 0);
    printf("Concurrent SPSC streaming (%d frames): PASSED (0 corruptions, 0 errors)\n", TOTAL_FRAMES);

    munmap(ring, total_size);
    close(fd);
    printf("SHM Ring lifecycle & teardown: PASSED\n");
    return 0;
}
