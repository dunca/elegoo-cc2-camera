#include "v4l2_camera.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <time.h>

// No frame for this long with the device open counts as a stall and triggers a reopen.
#define STALL_TIMEOUT_MS 5000
// Retry delay after a failed open grows from 1 s up to this cap.
#define REOPEN_BACKOFF_MAX_MS 10000
// A frame may arrive this early relative to the frame-rate cap and still be kept (camera jitter).
#define FPS_CAP_SLACK_US 8000

static int xioctl(int fd, int request, void *arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (-1 == r && EINTR == errno);
    return r;
}

static void cam_close(v4l2_camera_t *cam) {
    if (cam->fd >= 0 && cam->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(cam->fd, VIDIOC_STREAMOFF, &type);
    }
    cam->streaming = 0;

    if (cam->buffers) {
        for (unsigned int i = 0; i < cam->n_buffers; i++) {
            if (cam->buffers[i].start && cam->buffers[i].start != MAP_FAILED) {
                munmap(cam->buffers[i].start, cam->buffers[i].length);
            }
        }
        free(cam->buffers);
        cam->buffers = NULL;
    }
    cam->n_buffers = 0;

    if (cam->fd >= 0) {
        close(cam->fd);
        cam->fd = -1;
    }
}

// Opens, configures and starts streaming. Errors are printed only when `verbose`
// so a missing camera does not flood the log while the capture thread retries.
static int cam_open(v4l2_camera_t *cam, int verbose) {
    cam->fd = open(cam->device_path, O_RDWR | O_NONBLOCK, 0);
    if (cam->fd < 0) {
        if (verbose) fprintf(stderr, "[v4l2_camera] Cannot open video device '%s': %s\n", cam->device_path, strerror(errno));
        return -1;
    }

    struct v4l2_capability cap;
    if (xioctl(cam->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        if (verbose) fprintf(stderr, "[v4l2_camera] %s is not a V4L2 device\n", cam->device_path);
        cam_close(cam);
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(cap.capabilities & V4L2_CAP_STREAMING)) {
        if (verbose) fprintf(stderr, "[v4l2_camera] %s does not support video capture streaming\n", cam->device_path);
        cam_close(cam);
        return -1;
    }

    // Set Format
    struct v4l2_format vfmt;
    memset(&vfmt, 0, sizeof(vfmt));
    vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vfmt.fmt.pix.width = cam->req_width;
    vfmt.fmt.pix.height = cam->req_height;
    vfmt.fmt.pix.pixelformat = cam->req_pixelformat;
    vfmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(cam->fd, VIDIOC_S_FMT, &vfmt) < 0) {
        // If requested format failed, try MJPEG fallback if H.264 was requested
        if (cam->req_pixelformat == V4L2_PIX_FMT_H264) {
            if (verbose) fprintf(stderr, "[v4l2_camera] H.264 not accepted by %s, falling back to MJPEG\n", cam->device_path);
            vfmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
            if (xioctl(cam->fd, VIDIOC_S_FMT, &vfmt) < 0) {
                if (verbose) fprintf(stderr, "[v4l2_camera] Failed to set MJPEG format on %s: %s\n", cam->device_path, strerror(errno));
                cam_close(cam);
                return -1;
            }
        } else {
            if (verbose) fprintf(stderr, "[v4l2_camera] Failed to set format on %s: %s\n", cam->device_path, strerror(errno));
            cam_close(cam);
            return -1;
        }
    }

    cam->width = vfmt.fmt.pix.width;
    cam->height = vfmt.fmt.pix.height;
    cam->pixelformat = vfmt.fmt.pix.pixelformat;

    // Set Quality if specified
    if (cam->quality > 0) {
        struct v4l2_control ctrl;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
        ctrl.value = cam->quality;
        if (xioctl(cam->fd, VIDIOC_S_CTRL, &ctrl) == 0) {
            printf("[v4l2_camera] JPEG compression quality set to %d%%\n", cam->quality);
        }
    }

    char fourcc[5] = {0};
    memcpy(fourcc, &cam->pixelformat, 4);
    printf("[v4l2_camera] Negotiated format: %s (%dx%d)\n", fourcc, cam->width, cam->height);

    // Request MMAP buffers
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        fprintf(stderr, "[v4l2_camera] VIDIOC_REQBUFS error on %s\n", cam->device_path);
        cam_close(cam);
        return -1;
    }

    cam->buffers = calloc(req.count, sizeof(*cam->buffers));
    if (!cam->buffers) {
        cam_close(cam);
        return -1;
    }

    for (cam->n_buffers = 0; cam->n_buffers < req.count; cam->n_buffers++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = cam->n_buffers;

        if (xioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "[v4l2_camera] VIDIOC_QUERYBUF error\n");
            cam_close(cam);
            return -1;
        }

        cam->buffers[cam->n_buffers].length = buf.length;
        cam->buffers[cam->n_buffers].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, cam->fd, buf.m.offset);
        if (cam->buffers[cam->n_buffers].start == MAP_FAILED) {
            fprintf(stderr, "[v4l2_camera] mmap error\n");
            cam_close(cam);
            return -1;
        }
    }

    for (unsigned int i = 0; i < cam->n_buffers; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "[v4l2_camera] VIDIOC_QBUF error\n");
            cam_close(cam);
            return -1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "[v4l2_camera] VIDIOC_STREAMON error: %s\n", strerror(errno));
        cam_close(cam);
        return -1;
    }
    cam->streaming = 1;
    printf("[v4l2_camera] Camera streaming started on %s\n", cam->device_path);
    return 0;
}

// Sleeps up to `ms`, waking early when the camera is being stopped.
static void sleep_while_running(v4l2_camera_t *cam, int ms) {
    while (cam->running && ms > 0) {
        int step = ms > 100 ? 100 : ms;
        usleep(step * 1000);
        ms -= step;
    }
}

static void *camera_thread_proc(void *arg) {
    v4l2_camera_t *cam = (v4l2_camera_t *)arg;
    int backoff_ms = 1000;
    uint64_t interval_us = cam->fps > 0 ? 1000000ULL / (uint64_t)cam->fps : 0;
    uint64_t next_due_us = 0;

    while (cam->running) {
        if (cam->fd < 0) {
            // Log the first failure of a run and then every ~5 minutes.
            int verbose = cam->open_failures == 0 || cam->open_failures % 30 == 0;
            if (cam_open(cam, verbose) != 0) {
                cam->open_failures++;
                if (verbose) fprintf(stderr, "[v4l2_camera] Open failed (attempt %d), retrying in %d ms\n", cam->open_failures, backoff_ms);
                sleep_while_running(cam, backoff_ms);
                backoff_ms = backoff_ms * 2 > REOPEN_BACKOFF_MAX_MS ? REOPEN_BACKOFF_MAX_MS : backoff_ms * 2;
                continue;
            }
            if (cam->open_failures) printf("[v4l2_camera] Camera back after %d failed open attempts\n", cam->open_failures);
            cam->open_failures = 0;
            backoff_ms = 1000;
            next_due_us = 0;
        }

        struct pollfd fds[1];
        fds[0].fd = cam->fd;
        fds[0].events = POLLIN;

        const char *fault = NULL;
        uint64_t last_frame_us = monotonic_us();

        while (cam->running && !fault) {
            int r = poll(fds, 1, 1000); // 1 sec timeout
            if (r < 0) {
                if (errno == EINTR) continue;
                fault = "poll error";
                break;
            }
            if (r == 0) {
                if (monotonic_us() - last_frame_us >= STALL_TIMEOUT_MS * 1000ULL) fault = "no frames (stall)";
                continue;
            }
            if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                fault = "device error/hangup";
                break;
            }
            if (!(fds[0].revents & POLLIN)) continue;

            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;

            if (xioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EAGAIN) continue;
                fprintf(stderr, "[v4l2_camera] VIDIOC_DQBUF: %s\n", strerror(errno));
                fault = "dequeue failed";
                break;
            }

            uint64_t now = monotonic_us();
            last_frame_us = now;
            if (buf.index < cam->n_buffers && buf.bytesused > 0) {
                int keep = 1;
                if (interval_us) {
                    if (now + FPS_CAP_SLACK_US < next_due_us) {
                        keep = 0;
                    } else {
                        // Advance on a fixed grid so the average rate is exact; resync after a gap.
                        next_due_us = (next_due_us + interval_us < now) ? now + interval_us : next_due_us + interval_us;
                    }
                }
                if (keep) frame_ring_push(cam->ring, cam->buffers[buf.index].start, buf.bytesused);
            }

            if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
                fprintf(stderr, "[v4l2_camera] VIDIOC_QBUF: %s\n", strerror(errno));
                fault = "requeue failed";
                break;
            }
        }

        if (fault && cam->running) {
            cam->reopens++;
            time_t t = time(NULL);
            char when[32];
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&t));
            fprintf(stderr, "[v4l2_camera] %s capture fault: %s; reopening %s (reopen #%u)\n", when, fault, cam->device_path, cam->reopens);
            cam_close(cam);
            sleep_while_running(cam, 500);
        }
    }

    return NULL;
}

int v4l2_camera_init(v4l2_camera_t *cam, const char *dev_path, int width, int height, int fps, int quality, uint32_t fmt, frame_ring_t *ring) {
    memset(cam, 0, sizeof(*cam));
    snprintf(cam->device_path, sizeof(cam->device_path), "%s", dev_path ? dev_path : "/dev/video0");
    cam->req_width = width > 0 ? width : 1280;
    cam->req_height = height > 0 ? height : 720;
    cam->width = cam->req_width;
    cam->height = cam->req_height;
    cam->fps = fps > 0 ? fps : 0;
    cam->quality = quality > 0 ? quality : 0;
    cam->req_pixelformat = fmt ? fmt : V4L2_PIX_FMT_MJPEG;
    cam->pixelformat = cam->req_pixelformat;
    cam->fd = -1;
    cam->ring = ring;

    if (cam->fps > 0) printf("[v4l2_camera] Output frame rate capped at %d fps\n", cam->fps);

    if (cam_open(cam, 1) != 0) {
        cam->open_failures = 1;
        return -1;
    }
    return 0;
}

int v4l2_camera_start(v4l2_camera_t *cam) {
    if (cam->running) return -1;

    cam->running = 1;
    if (pthread_create(&cam->thread, NULL, camera_thread_proc, cam) != 0) {
        cam->running = 0;
        return -1;
    }
    return 0;
}

void v4l2_camera_stop(v4l2_camera_t *cam) {
    if (!cam->running) return;

    cam->running = 0;
    pthread_join(cam->thread, NULL);

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (cam->fd >= 0 && cam->streaming) xioctl(cam->fd, VIDIOC_STREAMOFF, &type);
    cam->streaming = 0;
    printf("[v4l2_camera] Camera streaming stopped\n");
}

void v4l2_camera_cleanup(v4l2_camera_t *cam) {
    v4l2_camera_stop(cam);
    cam_close(cam);
}
