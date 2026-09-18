#ifndef GW_HTTP_H
#define GW_HTTP_H

#include <stddef.h>

typedef struct {
    const char *method;
    const char *path;
    const char *body;
    size_t      body_len;
} gw_request;

/* Handler fills *out_body with a malloc'd JSON string the server will free,
   and returns the HTTP status code. */
typedef int (*gw_handler)(const gw_request *req, char **out_body);

int gw_serve(int port, gw_handler handler);

#endif
