#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <pthread.h>
#include "frame_ring.h"

// Concurrent MJPEG stream viewers; further stream requests get 503.
#define HTTP_MAX_STREAM_CLIENTS 8

// Fills `buf` with the /health JSON body, returns its length.
typedef int (*http_health_fn)(char *buf, size_t len, void *user);

typedef struct {
    int port;
    int server_fd;
    frame_ring_t *ring;
    pthread_t accept_thread;
    volatile int running;
    volatile int stream_clients;
    http_health_fn health_fn;
    void *health_user;
} http_server_t;

int http_server_init(http_server_t *server, int port, frame_ring_t *ring);
void http_server_set_health(http_server_t *server, http_health_fn fn, void *user);
int http_server_start(http_server_t *server);
void http_server_stop(http_server_t *server);

#endif // HTTP_SERVER_H
