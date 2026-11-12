#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <pthread.h>

#define DEFAULT_PACKET_SIZE 1920 /* 48000 Hz, 2 channels, 16-bit, 20ms = 1920 bytes */
#define NUM_PACKETS 50000

struct audio_msg {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

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

struct thread_data {
    int read_fd;
    int num_packets;
    size_t packet_size;
    uint64_t *latencies;
    volatile bool ready;
};

static void *consumer_thread(void *arg) {
    struct thread_data *td = (struct thread_data *)arg;
    size_t buf_size = sizeof(struct audio_msg) + td->packet_size + 64;
    uint8_t *buf = malloc(buf_size);
    if (!buf) {
        perror("malloc");
        return NULL;
    }

    td->ready = true;

    for (int i = 0; i < td->num_packets; i++) {
        ssize_t n = recv(td->read_fd, buf, buf_size, 0);
        uint64_t recv_time = get_time_ns();
        if (n <= 0) {
            fprintf(stderr, "consumer recv error: %zd (errno %d)\n", n, errno);
            break;
        }
        if ((size_t)n >= sizeof(struct audio_msg) + sizeof(uint64_t)) {
            uint64_t send_time;
            memcpy(&send_time, buf + sizeof(struct audio_msg), sizeof(send_time));
            if (recv_time >= send_time) {
                td->latencies[i] = recv_time - send_time;
            } else {
                td->latencies[i] = 0;
            }
        }
    }

    free(buf);
    return NULL;
}

int main(int argc, char **argv) {
    int num_packets = NUM_PACKETS;
    size_t pcm_size = DEFAULT_PACKET_SIZE;

    if (argc > 1) num_packets = atoi(argv[1]);
    if (argc > 2) pcm_size = (size_t)atoi(argv[2]);

    printf("=== Baseline Audio IPC Benchmark (Unix Domain Socketpair SEQPACKET) ===\n");
    printf("Iterations: %d packets | Packet PCM Payload: %zu bytes\n", num_packets, pcm_size);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) {
        perror("socketpair");
        return 1;
    }

    /* Set buffer sizes */
    int buf_size = 256 * 1024;
    setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

    uint64_t *latencies = calloc(num_packets, sizeof(uint64_t));
    if (!latencies) {
        perror("calloc");
        return 1;
    }

    struct thread_data td;
    td.read_fd = sv[0];
    td.num_packets = num_packets;
    td.packet_size = pcm_size;
    td.latencies = latencies;
    td.ready = false;

    pthread_t thread;
    if (pthread_create(&thread, NULL, consumer_thread, &td) != 0) {
        perror("pthread_create");
        return 1;
    }

    while (!td.ready) {
        usleep(100);
    }

    size_t tx_buf_size = sizeof(struct audio_msg) + pcm_size;
    uint8_t *tx_buf = malloc(tx_buf_size);
    struct audio_msg *hdr = (struct audio_msg *)tx_buf;
    hdr->type = 2; /* AUDIO_MSG_PCM */
    hdr->size = pcm_size;

    uint64_t start_total = get_time_ns();

    for (int i = 0; i < num_packets; i++) {
        uint64_t send_time = get_time_ns();
        memcpy(tx_buf + sizeof(struct audio_msg), &send_time, sizeof(send_time));

        struct iovec iov[2] = {
            { .iov_base = hdr, .iov_len = sizeof(*hdr) },
            { .iov_base = tx_buf + sizeof(*hdr), .iov_len = pcm_size }
        };
        struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };

        while (sendmsg(sv[1], &m, 0) < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10);
                continue;
            }
            perror("sendmsg");
            break;
        }
    }

    pthread_join(thread, NULL);
    uint64_t end_total = get_time_ns();

    uint64_t total_duration_ns = end_total - start_total;
    double total_duration_sec = (double)total_duration_ns / 1e9;
    double total_mb = (double)(num_packets * (sizeof(struct audio_msg) + pcm_size)) / (1024.0 * 1024.0);
    double throughput_mb_s = total_mb / total_duration_sec;
    double packets_per_sec = (double)num_packets / total_duration_sec;

    /* Sort latencies for percentiles */
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

    close(sv[0]);
    close(sv[1]);
    free(tx_buf);
    free(latencies);
    return 0;
}
