#ifndef V4L2_CAMERA_H
#define V4L2_CAMERA_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <linux/videodev2.h>
#include "frame_ring.h"

typedef struct {
    char device_path[64];
    int req_width;
    int req_height;
    int width;          // negotiated with the driver on the last successful open
    int height;
    volatile int fps;   // output frame-rate cap applied in software; 0 = whatever the camera delivers
    int quality;
    uint32_t req_pixelformat;
    uint32_t pixelformat; // V4L2_PIX_FMT_MJPEG or V4L2_PIX_FMT_H264
    int fd;
    
    // MMAP buffers
    struct {
        void *start;
        size_t length;
    } *buffers;
    unsigned int n_buffers;

    frame_ring_t *ring;
    pthread_t thread;
    volatile int running;
    volatile int streaming;      // 1 while the device is open and streaming
    volatile unsigned reopens;   // times capture was torn down and reopened after a fault
    volatile int open_failures;  // consecutive failed open attempts
    volatile int reconfigure;    // set by v4l2_camera_reconfigure: reopen with the new requested size
} v4l2_camera_t;

// Opens the device and starts streaming. On failure the struct is still usable:
// v4l2_camera_start() launches the capture thread, which keeps retrying the open.
int v4l2_camera_init(v4l2_camera_t *cam, const char *dev_path, int width, int height, int fps, int quality, uint32_t fmt, frame_ring_t *ring);
int v4l2_camera_start(v4l2_camera_t *cam);
void v4l2_camera_stop(v4l2_camera_t *cam);
// Applies new settings to the running capture: the fps cap from the next frame on; a new size by
// reopening the device inside the capture thread (stream clients stay connected).
void v4l2_camera_reconfigure(v4l2_camera_t *cam, int width, int height, int fps);
void v4l2_camera_cleanup(v4l2_camera_t *cam);

#endif // V4L2_CAMERA_H
