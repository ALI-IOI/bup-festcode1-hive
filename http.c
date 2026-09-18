#define _GNU_SOURCE
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#define MAX_BODY   (1 << 20)   /* 1 MiB */
#define MAX_REQ    (MAX_BODY + 8192)
#define START_BUF  16384       /* grown on demand; a real scenario is ~3 KiB */
#define THREAD_STK (256 * 1024)

static gw_handler g_handler;

static const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    default:  return "Internal Server Error";
    }
}

static void send_response(int fd, int code, const char *body)
{
    if (!body) body = "{\"error\":\"internal error\"}";
    size_t blen = strlen(body);
    char head[256];
    int hlen = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        code, status_text(code), blen);

    /* write_all: partial writes on a socket are normal, a single write() is a bug */
    const char *parts[2] = { head, body };
    size_t lens[2] = { (size_t)hlen, blen };
    for (int i = 0; i < 2; i++) {
        size_t off = 0;
        while (off < lens[i]) {
            ssize_t w = write(fd, parts[i] + off, lens[i] - off);
            if (w <= 0) { if (errno == EINTR) continue; return; }
            off += (size_t)w;
        }
    }
}

static void *handle_conn(void *arg)
{
    int fd = (int)(intptr_t)arg;

    struct timeval tv = { .tv_sec = 15, .tv_usec = 0 };  /* leaves headroom inside the 30s per-request limit */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    /* Grow on demand instead of reserving the maximum up front: a fixed 1 MiB
       per connection is ~500 MiB at 500 concurrent connections, on a request
       body that is normally about 3 KiB. */
    size_t cap = START_BUF;
    char *buf = malloc(cap + 1);
    if (!buf) { close(fd); return NULL; }

    size_t used = 0;
    char *hdr_end = NULL;

    /* 1. read until end of headers */
    while (used < MAX_REQ) {
        if (used == cap) {
            size_t ncap = cap * 2 > MAX_REQ ? MAX_REQ : cap * 2;
            char *nb = realloc(buf, ncap + 1);
            if (!nb) break;
            buf = nb; cap = ncap;
        }
        ssize_t r = read(fd, buf + used, cap - used);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
        used += (size_t)r;
        buf[used] = 0;
        if ((hdr_end = strstr(buf, "\r\n\r\n")) != NULL) break;
    }
    if (!hdr_end) { send_response(fd, 400, "{\"error\":\"malformed request\"}"); goto done; }

    size_t hdr_len = (size_t)(hdr_end - buf) + 4;

    /* 2. Content-Length (case-insensitive; the header block is NUL-terminated at
          hdr_end so this never scans into the body) */
    long content_len = 0;
    {
        char saved = *hdr_end; *hdr_end = 0;
        for (char *p = buf; *p; p++) {
            if ((*p == 'c' || *p == 'C') && strncasecmp(p, "content-length:", 15) == 0) {
                content_len = strtol(p + 15, NULL, 10);
                break;
            }
        }
        *hdr_end = saved;
    }
    if (content_len < 0 || content_len > MAX_BODY) {
        send_response(fd, 413, "{\"error\":\"payload too large\"}"); goto done;
    }

    /* Clients that send "Expect: 100-continue" (curl above its threshold, Java and
       .NET HttpClient, many load harnesses) will not send the body until we
       acknowledge. Without this they stall for a full client timeout per request. */
    {
        char saved = *hdr_end; *hdr_end = 0;
        int want_continue = 0;
        for (char *p = buf; *p; p++)
            if ((*p == 'e' || *p == 'E') && strncasecmp(p, "expect:", 7) == 0) {
                want_continue = (strcasestr(p + 7, "100-continue") != NULL);
                break;
            }
        *hdr_end = saved;
        if (want_continue) {
            const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
            size_t off = 0, n = strlen(cont);
            while (off < n) {
                ssize_t w = write(fd, cont + off, n - off);
                if (w <= 0) { if (errno == EINTR) continue; goto done; }
                off += (size_t)w;
            }
        }
    }

    /* 3. read the remainder of the body */
    if (hdr_len + (size_t)content_len > cap) {
        size_t ncap = hdr_len + (size_t)content_len;
        char *nb = realloc(buf, ncap + 1);
        if (!nb) { send_response(fd, 500, NULL); goto done; }
        buf = nb; cap = ncap;
    }
    while (used < hdr_len + (size_t)content_len) {
        ssize_t r = read(fd, buf + used, hdr_len + (size_t)content_len - used);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
        used += (size_t)r;
    }
    if (used < hdr_len + (size_t)content_len) {
        send_response(fd, 400, "{\"error\":\"incomplete body\"}"); goto done;
    }
    buf[hdr_len + content_len] = 0;

    /* 4. request line */
    char method[16] = {0}, path[512] = {0};
    if (sscanf(buf, "%15s %511s", method, path) != 2) {
        send_response(fd, 400, "{\"error\":\"malformed request line\"}"); goto done;
    }
    char *q = strchr(path, '?'); if (q) *q = 0;

    gw_request req = { method, path, buf + hdr_len, (size_t)content_len };
    char *out = NULL;
    int code = g_handler(&req, &out);
    send_response(fd, code, out);
    free(out);

done:
    free(buf);
    close(fd);
    return NULL;
}

int gw_serve(int port, gw_handler handler)
{
    signal(SIGPIPE, SIG_IGN);          /* a client that hangs up must not kill us */
    g_handler = handler;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   /* 0.0.0.0 — required for Docker */
    addr.sin_port        = htons((uint16_t)port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(srv, 128) < 0) { perror("listen"); return 1; }

    fprintf(stderr, "gridwise listening on 0.0.0.0:%d\n", port);
    fflush(stderr);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        /* Default 8 MiB of virtual stack per thread exhausts address space long
           before the work does; this handler needs a small fraction of it. */
        pthread_attr_setstacksize(&at, THREAD_STK);
        if (pthread_create(&th, &at, handle_conn, (void *)(intptr_t)fd) != 0) {
            send_response(fd, 500, "{\"error\":\"server busy\"}");
            close(fd);
        }
        pthread_attr_destroy(&at);
    }
}
