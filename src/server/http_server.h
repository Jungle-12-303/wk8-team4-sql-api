#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stddef.h>

typedef struct HTTPServerOptions {
    unsigned short port;
    size_t worker_count;
    size_t queue_capacity;
    unsigned int lock_timeout_ms;
    unsigned int simulate_read_delay_ms;
    unsigned int simulate_write_delay_ms;
    unsigned int max_requests;
} HTTPServerOptions;

void http_server_options_default(HTTPServerOptions *options);
int http_server_run(const HTTPServerOptions *options);

#endif
