#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <stdatomic.h>

#define DEFAULT_PACKET_SIZE 1920 /* 48000 Hz, 2 channels, 16-bit, 20ms */
#define NUM_PACKETS 50000
#define SHM_RING_DATA_SIZE (128 * 1024) /* 128 KB buffer */

#define ANLAND_SHM_AUDIO_MAGIC 0x414E4C41 /* "ANLA" */

struct anland_shm_audio_header {
    uint32_t magic;
    uint32_t version;
    uint32_t buffer_size;
    uint32_t rate;
    uint32_t channels;
    uint32_t format;
    
    /* Cache-line separated atomic indices to prevent false sharing */
    _Atomic uint32_t write_head __attribute__((aligned(64)));
    _Atomic uint32_t read_tail  __attribute__((aligned(64)));
    _Atomic uint32_t done       __attribute__((aligned(64)));
    
    uint8_t data[] __attribute__((aligned(64)));
};

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int compare_uint64(const void *a, const void *b) {
    uint64_t arg1 = *(const uint64_t *)a;
    uint64_t arg2 = *(const uint64_t *)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

static int create_memfd(const char *name) {
#ifdef __NR_memfd_create
    return (int)syscall(__NR_memfd_create, name, 1 /* MFD_CLOEXEC */);
#else
    return -1;
#endif
}

struct shm_thread_data {
    struct anland_shm_audio_header *ring;
    int num_packets;
    size_t packet_size;
    uint64_t *latencies;
    volatile bool ready;
};

static void *shm_consumer_thread(void *arg) {
    struct shm_thread_data *td = (struct shm_thread_data *)arg;
    struct anland_shm_audio_header *ring = td->ring;
    size_t ring_size = ring->buffer_size;
    uint8_t *tmp_buf = malloc(td->packet_size);
    if (!tmp_buf) return NULL;

    td->ready = true;

    for (int i = 0; i < td->num_packets; i++) {
        /* Spin-wait with pause for data to become available */
        while (1) {
            uint32_t head = atomic_load_explicit(&ring->write_head, memory_order_acquire);
            uint32_t tail = atomic_load_explicit(&ring->read_tail, memory_order_relaxed);
            uint32_t available = (head >= tail) ? (head - tail) : (ring_size - (tail - head));
            if (available >= td->packet_size) {
                break;
            }
            if (atomic_load_explicit(&ring->done, memory_order_acquire)) {
                break;
            }
#if defined(__aarch64__) || defined(__arm__)
            __asm__ volatile("yield");
#endif
        }

        uint64_t recv_time = get_time_ns();
        uint32_t tail = atomic_load_explicit(&ring->read_tail, memory_order_relaxed);

        size_t first = ring_size - tail;
        if (first > td->packet_size) first = td->packet_size;
        memcpy(tmp_buf, ring->data + tail, first);
        memcpy(tmp_buf + first, ring->data, td->packet_size - first);

        uint32_t new_tail = (tail + td->packet_size) % ring_size;
        atomic_store_explicit(&ring->read_tail, new_tail, memory_order_release);

        uint64_t send_time;
        memcpy(&send_time, tmp_buf, sizeof(send_time));
        if (recv_time >= send_time) {
            td->latencies[i] = recv_time - send_time;
        } else {
            td->latencies[i] = 0;
        }
    }

    free(tmp_buf);
    return NULL;
}

int main(int argc, char **argv) {
    int num_packets = NUM_PACKETS;
    size_t pcm_size = DEFAULT_PACKET_SIZE;

    if (argc > 1) num_packets = atoi(argv[1]);
    if (argc > 2) pcm_size = (size_t)atoi(argv[2]);

    printf("=== Optimized Audio IPC Benchmark (Lock-Free SHM Ring Buffer) ===\n");
    printf("Iterations: %d packets | Packet PCM Payload: %zu bytes\n", num_packets, pcm_size);

    int fd = create_memfd("anland_audio_shm_test");
    if (fd < 0) {
        perror("create_memfd");
        return 1;
    }

    size_t total_shm_size = sizeof(struct anland_shm_audio_header) + SHM_RING_DATA_SIZE;
    if (ftruncate(fd, total_shm_size) < 0) {
        perror("ftruncate");
        close(fd);
        return 1;
    }

    struct anland_shm_audio_header *ring = mmap(NULL, total_shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    memset(ring, 0, total_shm_size);
    ring->magic = ANLAND_SHM_AUDIO_MAGIC;
    ring->version = 1;
    ring->buffer_size = SHM_RING_DATA_SIZE;
    ring->rate = 48000;
    ring->channels = 2;
    ring->format = 0;
    atomic_init(&ring->write_head, 0);
    atomic_init(&ring->read_tail, 0);
    atomic_init(&ring->done, 0);

    uint64_t *latencies = calloc(num_packets, sizeof(uint64_t));
    if (!latencies) {
        perror("calloc");
        return 1;
    }

    struct shm_thread_data td;
    td.ring = ring;
    td.num_packets = num_packets;
    td.packet_size = pcm_size;
    td.latencies = latencies;
    td.ready = false;

    pthread_t thread;
    if (pthread_create(&thread, NULL, shm_consumer_thread, &td) != 0) {
        perror("pthread_create");
        return 1;
    }

    while (!td.ready) {
        usleep(100);
    }

    uint8_t *tx_buf = malloc(pcm_size);
    memset(tx_buf, 0x55, pcm_size);

    uint64_t start_total = get_time_ns();

    for (int i = 0; i < num_packets; i++) {
        uint64_t send_time = get_time_ns();
        memcpy(tx_buf, &send_time, sizeof(send_time));

        /* Wait for room in ring */
        while (1) {
            uint32_t head = atomic_load_explicit(&ring->write_head, memory_order_relaxed);
            uint32_t tail = atomic_load_explicit(&ring->read_tail, memory_order_acquire);
            uint32_t used = (head >= tail) ? (head - tail) : (SHM_RING_DATA_SIZE - (tail - head));
            uint32_t space = SHM_RING_DATA_SIZE - 1 - used;
            if (space >= pcm_size) {
                break;
            }
#if defined(__aarch64__) || defined(__arm__)
            __asm__ volatile("yield");
#endif
        }

        uint32_t head = atomic_load_explicit(&ring->write_head, memory_order_relaxed);
        size_t first = SHM_RING_DATA_SIZE - head;
        if (first > pcm_size) first = pcm_size;
        memcpy(ring->data + head, tx_buf, first);
        memcpy(ring->data, tx_buf + first, pcm_size - first);

        uint32_t new_head = (head + pcm_size) % SHM_RING_DATA_SIZE;
        atomic_store_explicit(&ring->write_head, new_head, memory_order_release);
    }

    atomic_store_explicit(&ring->done, 1, memory_order_release);
    pthread_join(thread, NULL);
    uint64_t end_total = get_time_ns();

    uint64_t total_duration_ns = end_total - start_total;
    double total_duration_sec = (double)total_duration_ns / 1e9;
    double total_mb = (double)(num_packets * pcm_size) / (1024.0 * 1024.0);
    double throughput_mb_s = total_mb / total_duration_sec;
    double packets_per_sec = (double)num_packets / total_duration_sec;

    qsort(latencies, num_packets, sizeof(uint64_t), compare_uint64);

    uint64_t sum_lat = 0;
    for (int i = 0; i < num_packets; i++) {
        sum_lat += latencies[i];
    }
    double mean_lat_us = ((double)sum_lat / (double)num_packets) / 1000.0;
    double min_lat_us = (double)latencies[0] / 1000.0;
    double p50_lat_us = (double)latencies[num_packets / 2] / 1000.0;
    double p95_lat_us = (double)latencies[(size_t)(num_packets * 0.95)] / 1000.0;
    double p99_lat_us = (double)latencies[(size_t)(num_packets * 0.99)] / 1000.0;
    double max_lat_us = (double)latencies[num_packets - 1] / 1000.0;

    printf("\n--- Results ---\n");
    printf("Total Elapsed Time:   %.4f seconds\n", total_duration_sec);
    printf("Throughput:           %.2f MB/s (%.1f packets/sec)\n", throughput_mb_s, packets_per_sec);
    printf("Latency (Min):        %.2f us\n", min_lat_us);
    printf("Latency (Mean):       %.2f us\n", mean_lat_us);
    printf("Latency (P50/Median): %.2f us\n", p50_lat_us);
    printf("Latency (P95):        %.2f us\n", p95_lat_us);
    printf("Latency (P99):        %.2f us\n", p99_lat_us);
    printf("Latency (Max):        %.2f us\n", max_lat_us);

    munmap(ring, total_shm_size);
    close(fd);
    free(tx_buf);
    free(latencies);
    return 0;
}
