#include "frame_ring.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

uint64_t monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

int frame_ring_init(frame_ring_t *ring) {
    memset(ring, 0, sizeof(*ring));
    if (pthread_mutex_init(&ring->lock, NULL) != 0) return -1;
    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    int cr = pthread_cond_init(&ring->cond, &cattr);
    pthread_condattr_destroy(&cattr);
    if (cr != 0) {
        pthread_mutex_destroy(&ring->lock);
        return -1;
    }

    for (int i = 0; i < MAX_RING_BUFFERS; i++) {
        ring->frames[i].data = malloc(MAX_FRAME_SIZE);
        if (!ring->frames[i].data) {
            frame_ring_cleanup(ring);
            return -1;
        }
        ring->frames[i].size = 0;
        ring->frames[i].sequence = 0;
    }
    return 0;
}

void frame_ring_cleanup(frame_ring_t *ring) {
    pthread_mutex_lock(&ring->lock);
    for (int i = 0; i < MAX_RING_BUFFERS; i++) {
        if (ring->frames[i].data) {
            free(ring->frames[i].data);
            ring->frames[i].data = NULL;
        }
    }
    pthread_mutex_unlock(&ring->lock);
    pthread_mutex_destroy(&ring->lock);
    pthread_cond_destroy(&ring->cond);
}

void frame_ring_push(frame_ring_t *ring, const uint8_t *data, size_t size) {
    if (size > MAX_FRAME_SIZE) {
        fprintf(stderr, "Warning: frame size %zu exceeds buffer size %d\n", size, MAX_FRAME_SIZE);
        size = MAX_FRAME_SIZE;
    }

    pthread_mutex_lock(&ring->lock);
    int idx = ring->write_index;
    memcpy(ring->frames[idx].data, data, size);
    ring->frames[idx].size = size;
    ring->total_frames++;
    ring->frames[idx].sequence = ring->total_frames;

    uint64_t now = monotonic_us();
    ring->frames[idx].timestamp_us = now;

    // Restart the window after a stall so the gap does not drag the next reading down.
    if (ring->rate_window_start_us == 0 || now - ring->rate_window_start_us > 6000000ULL) {
        ring->rate_window_start_us = now;
        ring->rate_window_frames = 0;
    } else {
        ring->rate_window_frames++;
        uint64_t span = now - ring->rate_window_start_us;
        if (span >= 2000000ULL) {
            ring->fps_measured = (double)ring->rate_window_frames * 1e6 / (double)span;
            ring->rate_window_start_us = now;
            ring->rate_window_frames = 0;
        }
    }

    ring->write_index = (ring->write_index + 1) % MAX_RING_BUFFERS;
    pthread_cond_broadcast(&ring->cond);
    pthread_mutex_unlock(&ring->lock);
}

int frame_ring_get_latest(frame_ring_t *ring, uint64_t last_seq, uint8_t *out_buf, size_t out_max, size_t *out_size, uint64_t *out_seq, int timeout_ms) {
    pthread_mutex_lock(&ring->lock);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }

    while (ring->total_frames <= last_seq) {
        int r = pthread_cond_timedwait(&ring->cond, &ring->lock, &ts);
        if (r == ETIMEDOUT) {
            pthread_mutex_unlock(&ring->lock);
            return -1;
        }
    }

    // Latest frame is index (write_index - 1 + MAX) % MAX
    int latest_idx = (ring->write_index - 1 + MAX_RING_BUFFERS) % MAX_RING_BUFFERS;
    frame_t *f = &ring->frames[latest_idx];

    if (f->size > out_max) {
        pthread_mutex_unlock(&ring->lock);
        return -2;
    }

    memcpy(out_buf, f->data, f->size);
    *out_size = f->size;
    *out_seq = f->sequence;

    pthread_mutex_unlock(&ring->lock);
    return 0;
}

void frame_ring_stats(frame_ring_t *ring, uint64_t *total_frames, uint64_t *last_frame_us, double *fps_measured) {
    pthread_mutex_lock(&ring->lock);
    int latest_idx = (ring->write_index - 1 + MAX_RING_BUFFERS) % MAX_RING_BUFFERS;
    *total_frames = ring->total_frames;
    *last_frame_us = ring->total_frames ? ring->frames[latest_idx].timestamp_us : 0;
    // A stalled camera stops calling push, so age the measured rate out here.
    *fps_measured = (ring->total_frames && monotonic_us() - ring->frames[latest_idx].timestamp_us < 3000000ULL)
                    ? ring->fps_measured : 0.0;
    pthread_mutex_unlock(&ring->lock);
}
