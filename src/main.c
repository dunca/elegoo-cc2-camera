#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <linux/videodev2.h>
#include "frame_ring.h"
#include "v4l2_camera.h"
#include "http_server.h"
#include "uds_ipc.h"

#ifndef APP_VERSION
#define APP_VERSION "0.1.0"
#endif

#ifndef GIT_VERSION
#define GIT_VERSION "v0.1.0"
#endif

static volatile int keep_running = 1;
static volatile sig_atomic_t reload_requested = 0;

static void sig_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        printf("\n[main] Signal %d received, shutting down...\n", signo);
        keep_running = 0;
    } else if (signo == SIGHUP) {
        reload_requested = 1; // handled by the main loop: re-read /opt/usr/camera-quality
    }
}

#include "timelapse_encoder.h"

// Log file kept in RAM (/tmp); rotated to <path>.1 once it passes this size.
#define LOG_MAX_BYTES (256 * 1024)
#define DEFAULT_LOG_PATH "/tmp/ai_camera.log"
// Persisted resolution / fps preference; re-read on SIGHUP (see camq).
#define CAMERA_QUALITY_PATH "/opt/usr/camera-quality"

static char log_path[128] = DEFAULT_LOG_PATH;

// Points stdout and stderr at the log file (append). Returns 0 on success.
static int log_open(void) {
    int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return -1;
    fflush(stdout);
    fflush(stderr);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) close(fd);
    return 0;
}

static void log_rotate_if_needed(void) {
    struct stat st;
    if (fstat(STDOUT_FILENO, &st) != 0 || st.st_size < LOG_MAX_BYTES) return;
    char old_path[sizeof(log_path) + 2];
    snprintf(old_path, sizeof(old_path), "%s.1", log_path);
    fflush(stdout);
    fflush(stderr);
    rename(log_path, old_path);
    log_open();
}

// Persisted preference written by `camq` on the printer (survives reboots), one line:
//   <resolution>[@<fps>]   resolution = 360 | 720 | 1080 | WxH, e.g. "720", "720@15", "640x360@10"
//   @<fps>                 frame-rate cap only, default resolution
static void read_camera_quality(const char *path, int *width, int *height, int *fps) {
    FILE *qf = fopen(path, "r");
    if (!qf) return;
    char line[64] = {0};
    if (fgets(line, sizeof(line), qf)) {
        char *at = strchr(line, '@');
        if (at) {
            int f = atoi(at + 1);
            if (f >= 1 && f <= 60) *fps = f;
            *at = '\0';
        }
        int cw = 0, ch = 0;
        if (sscanf(line, "%dx%d", &cw, &ch) == 2 && cw > 0 && ch > 0) {
            *width = cw; *height = ch;
        } else if (sscanf(line, "%d", &cw) == 1) {
            if (cw == 360) { *width = 640; *height = 360; }
            else if (cw == 720) { *width = 1280; *height = 720; }
            else if (cw == 1080) { *width = 1920; *height = 1080; }
        }
    }
    fclose(qf);
}

typedef struct {
    v4l2_camera_t *camera;
    frame_ring_t *ring;
    http_server_t *http;
    uint64_t start_us;
} health_ctx_t;

static int health_json(char *buf, size_t len, void *user) {
    health_ctx_t *h = user;
    uint64_t total, last_us;
    double fps;
    frame_ring_stats(h->ring, &total, &last_us, &fps);
    uint64_t now = monotonic_us();
    long long age_ms = total ? (long long)((now - last_us) / 1000ULL) : -1;
    int streaming = h->camera->streaming;
    int ok = streaming && total && age_ms < 5000;
    char fourcc[5] = {0};
    memcpy(fourcc, &h->camera->pixelformat, 4);
    return snprintf(buf, len,
        "{\"ok\":%s,\"version\":\"%s\",\"device\":\"%s\",\"streaming\":%s,"
        "\"width\":%d,\"height\":%d,\"format\":\"%s\",\"fps_cap\":%d,\"fps\":%.1f,"
        "\"frames\":%llu,\"last_frame_age_ms\":%lld,\"camera_reopens\":%u,\"open_failures\":%d,"
        "\"stream_clients\":%d,\"uptime_s\":%llu}\n",
        ok ? "true" : "false", GIT_VERSION, h->camera->device_path, streaming ? "true" : "false",
        h->camera->width, h->camera->height, fourcc, h->camera->fps, fps,
        (unsigned long long)total, age_ms, h->camera->reopens, h->camera->open_failures,
        h->http->stream_clients, (unsigned long long)((now - h->start_us) / 1000000ULL));
}

static void print_usage(const char *prog_name) {
    printf("ai_camera %s (%s) - lightweight camera & timelapse service\n\n", APP_VERSION, GIT_VERSION);
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -d, --device PATH          V4L2 camera device path (default: /dev/video0)\n");
    printf("  -w, --width INT            Capture width (default: 1280)\n");
    printf("  -h, --height INT           Capture height (default: 720)\n");
    printf("  -r, --fps INT              Cap the stream frame rate (default: camera rate; also settable via camq)\n");
    printf("  -q, --quality INT          JPEG compression quality 1-100\n");
    printf("  -p, --port INT             HTTP stream port (default: 8080)\n");
    printf("  -s, --socket PATH          UDS IPC socket path (default: /tmp/aicamera_uds)\n");
    printf("  -l, --log PATH             Service log file, rotated at 256 KB (default: %s; '-' = stdout)\n", DEFAULT_LOG_PATH);
    printf("  -f, --format STR           Format: H264 or MJPEG (default: MJPEG)\n");
    printf("      --no-auto-composite    Disable automatic background MP4 encoding on print stop\n");
    printf("      --cleanup              Encode uncompressed timelapses, sync database records, and exit\n");
    printf("  -S, --snapshot PATH        (for testing) Capture a single frame to PATH and exit\n");
    printf("  -E, --encode-timelapse DIR (for testing) Encode directory of JPEGs to MP4 and exit\n");
    printf("  -o, --output PATH          Output MP4 file path for timelapse encoding\n");
    printf("  -v, --version              Display version information and exit\n");
    printf("      --help                 Display this message and exit\n");
}

int main(int argc, char **argv) {
    // Line-buffer stdout so log lines appear immediately when redirected to a file.
    setvbuf(stdout, NULL, _IOLBF, 0);

    char device_path[64] = "/dev/video0";
    int width = 1280;
    int height = 720;
    int fps = 0;
    int quality = 0;
    int http_port = 8080;
    int auto_composite = 1;
    int cleanup_mode = 0;
    char socket_path[128] = "/tmp/aicamera_uds";
    char snapshot_path[256] = {0};
    char encode_dir[256] = {0};
    char output_mp4[256] = "/opt/usr/video/timelapse.mp4";
    uint32_t format = V4L2_PIX_FMT_MJPEG;

    read_camera_quality(CAMERA_QUALITY_PATH, &width, &height, &fps);

    static struct option long_options[] = {
        {"device", required_argument, 0, 'd'},
        {"width", required_argument, 0, 'w'},
        {"height", required_argument, 0, 'h'},
        {"fps", required_argument, 0, 'r'},
        {"quality", required_argument, 0, 'q'},
        {"port", required_argument, 0, 'p'},
        {"socket", required_argument, 0, 's'},
        {"log", required_argument, 0, 'l'},
        {"format", required_argument, 0, 'f'},
        {"no-auto-composite", no_argument, 0, 1001},
        {"cleanup", no_argument, 0, 1002},
        {"snapshot", required_argument, 0, 'S'},
        {"encode-timelapse", required_argument, 0, 'E'},
        {"output", required_argument, 0, 'o'},
        {"version", no_argument, 0, 'v'},
        {"help", no_argument, 0, '?'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:w:h:r:q:p:s:l:f:S:E:o:v?", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd': snprintf(device_path, sizeof(device_path), "%s", optarg); break;
            case 'w': width = atoi(optarg); break;
            case 'h': height = atoi(optarg); break;
            case 'r': fps = atoi(optarg); break;
            case 'q': quality = atoi(optarg); break;
            case 'p': http_port = atoi(optarg); break;
            case 's': snprintf(socket_path, sizeof(socket_path), "%s", optarg); break;
            case 'l': snprintf(log_path, sizeof(log_path), "%s", optarg); break;
            case 1001: auto_composite = 0; break;
            case 1002: cleanup_mode = 1; break;
            case 'S': snprintf(snapshot_path, sizeof(snapshot_path), "%s", optarg); break;
            case 'E': snprintf(encode_dir, sizeof(encode_dir), "%s", optarg); break;
            case 'o': snprintf(output_mp4, sizeof(output_mp4), "%s", optarg); break;
            case 'v':
                printf("ai_camera %s (%s)\n", APP_VERSION, GIT_VERSION);
                return 0;
            case 'f':
                if (strcasecmp(optarg, "MJPEG") == 0 || strcasecmp(optarg, "JPG") == 0) {
                    format = V4L2_PIX_FMT_MJPEG;
                } else if (strcasecmp(optarg, "H264") == 0 || strcasecmp(optarg, "264") == 0) {
                    format = V4L2_PIX_FMT_H264;
                }
                break;
            case '?':
            default:
                print_usage(argv[0]);
                return (opt == '?') ? 0 : 1;
        }
    }

    if (cleanup_mode) {
        return uds_ipc_sync_all_timelapses(socket_path);
    }

    // Direct standalone timelapse encoding mode (-E / --encode-timelapse)
    if (encode_dir[0] != '\0') {
        int encode_fps = fps > 0 ? fps : 15;
        printf("[TEST MODE] Encoding timelapse from '%s' to '%s' (%d fps)...\n",
               encode_dir, output_mp4, encode_fps);
        int res = timelapse_encode_directory(encode_dir, output_mp4, encode_fps, width, height, NULL, NULL);
        if (res == 0) {
            printf("[TEST MODE] Timelapse encoding completed successfully: '%s'\n", output_mp4);
            return 0;
        } else {
            fprintf(stderr, "[TEST MODE] Timelapse encoding failed with error code %d\n", res);
            return 1;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGHUP, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    frame_ring_t ring;
    if (frame_ring_init(&ring) != 0) {
        fprintf(stderr, "Failed to initialize frame ring buffer\n");
        return 1;
    }

    // The service logs to a file unless told otherwise (Elegoo's init sends stdout nowhere).
    int log_to_file = snapshot_path[0] == '\0' && strcmp(log_path, "-") != 0;
    if (log_to_file && log_open() != 0) log_to_file = 0;
    if (log_to_file) {
        time_t t = time(NULL);
        char when[32];
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&t));
        printf("=== ai_camera %s starting at %s ===\n", GIT_VERSION, when);
    }

    v4l2_camera_t camera;
    if (v4l2_camera_init(&camera, device_path, width, height, fps, quality, format, &ring) != 0) {
        if (snapshot_path[0] != '\0') {
            fprintf(stderr, "Error: Failed to initialize camera at %s\n", device_path);
            frame_ring_cleanup(&ring);
            return 1;
        }
        // Keep serving HTTP/IPC; the capture thread retries until the camera shows up.
        fprintf(stderr, "[main] Camera %s not available yet, will keep retrying\n", device_path);
    }

    v4l2_camera_start(&camera);

    // If Test Snapshot mode requested (-S / --snapshot)
    if (snapshot_path[0] != '\0') {
        printf("[TEST MODE] Capturing snapshot to '%s'...\n", snapshot_path);
        uint8_t *frame_buf = malloc(MAX_FRAME_SIZE);
        size_t frame_size = 0;
        uint64_t seq = 0;
        int r = frame_ring_get_latest(&ring, 0, frame_buf, MAX_FRAME_SIZE, &frame_size, &seq, 3000);
        if (r == 0 && frame_size > 0) {
            FILE *f = fopen(snapshot_path, "wb");
            if (f) {
                fwrite(frame_buf, 1, frame_size, f);
                fclose(f);
                printf("[TEST MODE] Snapshot successfully saved to '%s' (%zu bytes, seq %llu)\n", snapshot_path, frame_size, (unsigned long long)seq);
            } else {
                fprintf(stderr, "[TEST MODE] Failed to open '%s' for writing\n", snapshot_path);
            }
        } else {
            fprintf(stderr, "[TEST MODE] Failed to acquire frame from camera within timeout\n");
        }
        free(frame_buf);
        v4l2_camera_cleanup(&camera);
        frame_ring_cleanup(&ring);
        return 0;
    }

    printf("Starting ai_camera v%s (%s)\n", APP_VERSION, GIT_VERSION);
    printf("Device: %s (%dx%d, format: %s, fps cap: %d)\n", device_path, camera.width, camera.height,
           camera.pixelformat == V4L2_PIX_FMT_H264 ? "H264" : "MJPEG", camera.fps);
    printf("HTTP Port: %d\n", http_port);
    printf("UDS Socket: %s\n", socket_path);

    http_server_t http;
    health_ctx_t health = { &camera, &ring, &http, monotonic_us() };
    if (http_server_init(&http, http_port, &ring) == 0) {
        http_server_set_health(&http, health_json, &health);
        http_server_start(&http);
    }

    uds_ipc_t uds;
    if (uds_ipc_init(&uds, socket_path, &ring, auto_composite) == 0) {
        uds_ipc_start(&uds);
    }

    unsigned ticks = 0;
    while (keep_running) {
        sleep(1); // returns early when a signal arrives
        if (reload_requested) {
            reload_requested = 0;
            // Same defaults as at startup; the file decides (camq writes it before signalling).
            int new_w = 1280, new_h = 720, new_fps = 0;
            read_camera_quality(CAMERA_QUALITY_PATH, &new_w, &new_h, &new_fps);
            printf("[main] SIGHUP: camera-quality reload -> %dx%d, fps cap %d\n", new_w, new_h, new_fps);
            v4l2_camera_reconfigure(&camera, new_w, new_h, new_fps);
        }
        if (log_to_file && ++ticks % 10 == 0) log_rotate_if_needed();
    }

    printf("Shutting down camera service...\n");
    uds_ipc_stop(&uds);
    http_server_stop(&http);
    v4l2_camera_cleanup(&camera);
    frame_ring_cleanup(&ring);

    printf("Shutdown complete.\n");
    return 0;
}
