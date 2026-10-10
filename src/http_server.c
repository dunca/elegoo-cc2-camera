#include "http_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/time.h>

#define HTTP_HEADER \
    "HTTP/1.1 200 OK\r\n" \
    "Cache-Control: no-cache\r\n" \
    "Pragma: no-cache\r\n" \
    "Connection: close\r\n" \
    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n"

#define CORS_RESPONSE \
    "HTTP/1.1 200 OK\r\n" \
    "Access-Control-Allow-Origin: *\r\n" \
    "Access-Control-Allow-Methods: GET, OPTIONS\r\n" \
    "Access-Control-Allow-Headers: Content-Type\r\n" \
    "Content-Length: 0\r\n\r\n"

// A frame older than this is not served as a snapshot.
#define SNAPSHOT_MAX_AGE_US 5000000ULL

typedef struct {
    int client_fd;
    http_server_t *server;
} client_ctx_t;

static int send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void send_simple(int fd, const char *status, const char *ctype, const char *body) {
    char hdr[256];
    size_t blen = strlen(body);
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                     status, ctype, blen);
    if (send_all(fd, hdr, (size_t)n) == 0) send_all(fd, body, blen);
}

static void send_health(int fd, http_server_t *server) {
    char body[1024];
    if (server->health_fn) {
        server->health_fn(body, sizeof(body), server->health_user);
    } else {
        snprintf(body, sizeof(body), "{\"ok\":true}\n");
    }
    send_simple(fd, "200 OK", "application/json", body);
}

// Single JPEG of the latest frame: lighter than the stream for Home Assistant, Obico etc.
static void send_snapshot(int fd, http_server_t *server) {
    uint64_t total, last_us;
    double fps;
    frame_ring_stats(server->ring, &total, &last_us, &fps);
    if (total == 0 || monotonic_us() - last_us > SNAPSHOT_MAX_AGE_US) {
        send_simple(fd, "503 Service Unavailable", "text/plain", "no recent camera frame\n");
        return;
    }

    uint8_t *frame_buf = malloc(MAX_FRAME_SIZE);
    if (!frame_buf) {
        send_simple(fd, "500 Internal Server Error", "text/plain", "out of memory\n");
        return;
    }
    size_t frame_size = 0;
    uint64_t seq = 0;
    if (frame_ring_get_latest(server->ring, 0, frame_buf, MAX_FRAME_SIZE, &frame_size, &seq, 2000) == 0 && frame_size > 0) {
        char hdr[256];
        int n = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n"
                         "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                         frame_size);
        if (send_all(fd, hdr, (size_t)n) == 0) send_all(fd, frame_buf, frame_size);
    } else {
        send_simple(fd, "503 Service Unavailable", "text/plain", "no camera frame\n");
    }
    free(frame_buf);
}

static void *client_thread_proc(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    int fd = ctx->client_fd;
    http_server_t *server = ctx->server;
    free(ctx);

    // Set TCP_NODELAY
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // A client that never sends its request, or stops reading the stream, must not pin this thread.
    struct timeval tmo = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
    tmo.tv_sec = 10;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));

    char req_buf[1024];
    ssize_t bytes_read = recv(fd, req_buf, sizeof(req_buf) - 1, 0);
    if (bytes_read <= 0) {
        close(fd);
        return NULL;
    }
    req_buf[bytes_read] = '\0';

    if (strncmp(req_buf, "OPTIONS", 7) == 0) {
        send(fd, CORS_RESPONSE, strlen(CORS_RESPONSE), MSG_NOSIGNAL);
        close(fd);
        return NULL;
    }

    // Request target: "GET /path?query HTTP/1.1"
    char target[256] = {0};
    const char *sp = strchr(req_buf, ' ');
    if (sp) {
        sp++;
        size_t n = strcspn(sp, " \r\n");
        if (n >= sizeof(target)) n = sizeof(target) - 1;
        memcpy(target, sp, n);
    }

    if (strcmp(target, "/health") == 0) {
        send_health(fd, server);
        close(fd);
        return NULL;
    }

    if (strcmp(target, "/snapshot") == 0 || strcmp(target, "/snapshot.jpg") == 0 || strstr(target, "action=snapshot")) {
        send_snapshot(fd, server);
        close(fd);
        return NULL;
    }

    // Anything else is the MJPEG stream (Elegoo's apps and existing dashboards use various paths).
    if (__sync_add_and_fetch(&server->stream_clients, 1) > HTTP_MAX_STREAM_CLIENTS) {
        __sync_sub_and_fetch(&server->stream_clients, 1);
        send_simple(fd, "503 Service Unavailable", "text/plain", "too many stream clients\n");
        close(fd);
        return NULL;
    }

    // Send HTTP header
    if (send(fd, HTTP_HEADER, strlen(HTTP_HEADER), MSG_NOSIGNAL) <= 0) {
        __sync_sub_and_fetch(&server->stream_clients, 1);
        close(fd);
        return NULL;
    }

    uint8_t *frame_buf = malloc(MAX_FRAME_SIZE);
    if (!frame_buf) {
        __sync_sub_and_fetch(&server->stream_clients, 1);
        close(fd);
        return NULL;
    }

    uint64_t last_seq = 0;
    char boundary_hdr[128];

    while (server->running) {
        size_t frame_size = 0;
        uint64_t seq = 0;

        int r = frame_ring_get_latest(server->ring, last_seq, frame_buf, MAX_FRAME_SIZE, &frame_size, &seq, 500);
        if (r == 0) {
            last_seq = seq;
            int hdr_len = snprintf(boundary_hdr, sizeof(boundary_hdr),
                                  "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", frame_size);

            if (send_all(fd, boundary_hdr, (size_t)hdr_len) < 0) break;
            if (send_all(fd, frame_buf, frame_size) < 0) break;
            if (send_all(fd, "\r\n", 2) < 0) break;
        } else if (r == -2) {
            fprintf(stderr, "[http_server] Frame exceeded buffer limit\n");
            break;
        }
        // r == -1 is timeout, loop again if server->running
    }

    free(frame_buf);
    __sync_sub_and_fetch(&server->stream_clients, 1);
    close(fd);
    return NULL;
}

static void *accept_thread_proc(void *arg) {
    http_server_t *server = (http_server_t *)arg;

    struct pollfd fds[1];
    fds[0].fd = server->server_fd;
    fds[0].events = POLLIN;

    while (server->running) {
        int r = poll(fds, 1, 1000);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;

        if (fds[0].revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(server->server_fd, (struct sockaddr *)&client_addr, &addr_len);
            if (client_fd >= 0) {
                client_ctx_t *ctx = malloc(sizeof(*ctx));
                if (ctx) {
                    ctx->client_fd = client_fd;
                    ctx->server = server;
                    pthread_t thread;
                    pthread_attr_t attr;
                    pthread_attr_init(&attr);
                    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                    if (pthread_create(&thread, &attr, client_thread_proc, ctx) != 0) {
                        free(ctx);
                        close(client_fd);
                    }
                    pthread_attr_destroy(&attr);
                } else {
                    close(client_fd);
                }
            }
        }
    }

    return NULL;
}

int http_server_init(http_server_t *server, int port, frame_ring_t *ring) {
    memset(server, 0, sizeof(*server));
    server->port = port > 0 ? port : 8080;
    server->server_fd = -1;
    server->ring = ring;

    server->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->server_fd < 0) {
        perror("socket creation failed");
        return -1;
    }

    int opt = 1;
    setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(server->port);

    if (bind(server->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[http_server] Failed to bind HTTP server to port %d: %s\n", server->port, strerror(errno));
        close(server->server_fd);
        server->server_fd = -1;
        return -1;
    }

    if (listen(server->server_fd, 10) < 0) {
        perror("listen failed");
        close(server->server_fd);
        server->server_fd = -1;
        return -1;
    }

    printf("[http_server] HTTP MJPEG stream server initialized on port %d\n", server->port);
    return 0;
}

void http_server_set_health(http_server_t *server, http_health_fn fn, void *user) {
    server->health_fn = fn;
    server->health_user = user;
}

int http_server_start(http_server_t *server) {
    if (server->server_fd < 0 || server->running) return -1;

    server->running = 1;
    if (pthread_create(&server->accept_thread, NULL, accept_thread_proc, server) != 0) {
        server->running = 0;
        return -1;
    }

    printf("[http_server] HTTP stream server listening on port %d\n", server->port);
    return 0;
}

void http_server_stop(http_server_t *server) {
    if (!server->running) return;

    server->running = 0;
    pthread_join(server->accept_thread, NULL);

    if (server->server_fd >= 0) {
        close(server->server_fd);
        server->server_fd = -1;
    }
    printf("[http_server] HTTP stream server stopped\n");
}
