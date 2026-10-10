#ifndef FRAME_RING_H
#define FRAME_RING_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define MAX_RING_BUFFERS 4
#define MAX_FRAME_SIZE (4 * 1024 * 1024) // max frame size bytes

typedef struct {
    uint8_t *data;
    size_t size;
    uint64_t sequence;
    uint64_t timestamp_us; // CLOCK_MONOTONIC
} frame_t;

typedef struct {
    frame_t frames[MAX_RING_BUFFERS];
    int write_index;
    uint64_t total_frames;
    pthread_mutex_t lock;
    pthread_cond_t cond; // waits use CLOCK_MONOTONIC (immune to the boot-time 1970 -> NTP jump)

    // Measured output rate, refreshed about every 2 s by frame_ring_push.
    uint64_t rate_window_start_us;
    uint64_t rate_window_frames;
    double fps_measured;
} frame_ring_t;

uint64_t monotonic_us(void);

int frame_ring_init(frame_ring_t *ring);
void frame_ring_cleanup(frame_ring_t *ring);
void frame_ring_push(frame_ring_t *ring, const uint8_t *data, size_t size);
int frame_ring_get_latest(frame_ring_t *ring, uint64_t last_seq, uint8_t *out_buf, size_t out_max, size_t *out_size, uint64_t *out_seq, int timeout_ms);
// Snapshot of the ring statistics for /health. last_frame_us is 0 before the first frame.
void frame_ring_stats(frame_ring_t *ring, uint64_t *total_frames, uint64_t *last_frame_us, double *fps_measured);

#endif // FRAME_RING_H
