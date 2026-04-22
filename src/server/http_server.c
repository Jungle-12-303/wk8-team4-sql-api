#include "http_server.h"

#include "api.h"
#include "db_server.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int HTTPSocket;
#define HTTP_INVALID_SOCKET (-1)
static volatile sig_atomic_t http_server_stop_requested = 0;

#define HTTP_SERVER_REQUEST_BUFFER_SIZE 8192

typedef struct HTTPRequestQueue {
    HTTPSocket *items;
    size_t head;
    size_t tail;
    size_t count;
    size_t capacity;
    int shutting_down;
    PlatformMutex mutex;
    PlatformCond cond;
} HTTPRequestQueue;

typedef struct HTTPServerContext {
    DBServer db_server;
    HTTPServerOptions options;
    HTTPRequestQueue queue;
    PlatformThread *workers;
    PlatformMutex state_mutex;
    unsigned long long completed_responses;
} HTTPServerContext;

static void http_server_signal_handler(int signal_number) {
    (void)signal_number;
    http_server_stop_requested = 1;
}

static void http_server_request_stop(void) {
    http_server_stop_requested = 1;
}

static int http_server_should_stop(void) {
    return http_server_stop_requested != 0;
}

static int http_server_install_signal_handlers(void) {
    return signal(SIGINT, http_server_signal_handler) != SIG_ERR &&
           signal(SIGTERM, http_server_signal_handler) != SIG_ERR;
}

static int http_server_socket_runtime_init(void) {
    return 1;
}

static void http_server_socket_runtime_cleanup(void) {
}

static void http_server_socket_close(HTTPSocket socket_handle) {
    close(socket_handle);
}

static int http_server_socket_send_all(HTTPSocket socket_handle, const char *buffer, size_t length) {
    size_t offset = 0;

    while (offset < length) {
        int sent = send(socket_handle, buffer + offset, (int)(length - offset), 0);

        if (sent <= 0) {
            return 0;
        }

        offset += (size_t)sent;
    }

    return 1;
}

static int http_server_socket_wait_for_read(HTTPSocket socket_handle, unsigned int timeout_ms) {
    fd_set read_fds;
    struct timeval timeout;
    int select_result;

    FD_ZERO(&read_fds);
    FD_SET(socket_handle, &read_fds);

    timeout.tv_sec = (long)(timeout_ms / 1000U);
    timeout.tv_usec = (long)(timeout_ms % 1000U) * 1000L;

    select_result = select(socket_handle + 1, &read_fds, NULL, NULL, &timeout);

    return select_result;
}

static int http_server_header_name_equals(const char *left, const char *right, size_t length) {
    size_t index;
    size_t right_length = strlen(right);

    if (length != right_length) {
        return 0;
    }

    for (index = 0; index < length; index++) {
        char left_char = left[index];
        char right_char = right[index];

        if (left_char >= 'A' && left_char <= 'Z') {
            left_char = (char)(left_char - 'A' + 'a');
        }
        if (right_char >= 'A' && right_char <= 'Z') {
            right_char = (char)(right_char - 'A' + 'a');
        }
        if (left_char != right_char) {
            return 0;
        }
    }

    return 1;
}

static size_t http_server_parse_content_length(const char *request_buffer) {
    const char *line = strstr(request_buffer, "\r\n");
    const char *header_end = strstr(request_buffer, "\r\n\r\n");

    if (line == NULL || header_end == NULL) {
        return 0;
    }

    line += 2;
    while (line < header_end) {
        const char *next_line = strstr(line, "\r\n");
        const char *colon;
        size_t name_length;

        if (next_line == NULL || next_line > header_end) {
            break;
        }

        colon = memchr(line, ':', (size_t)(next_line - line));
        if (colon != NULL) {
            name_length = (size_t)(colon - line);
            if (http_server_header_name_equals(line, "Content-Length", name_length)) {
                return (size_t)strtoul(colon + 1, NULL, 10);
            }
        }

        line = next_line + 2;
    }

    return 0;
}

static int http_server_read_request(HTTPSocket client_socket, char *buffer, size_t buffer_size, char *error_message, size_t error_size) {
    size_t total = 0;
    size_t expected_total = 0;
    int headers_ready = 0;

    if (buffer_size == 0) {
        return 0;
    }

    buffer[0] = '\0';

    while (total + 1 < buffer_size) {
        int received = recv(client_socket, buffer + total, (int)(buffer_size - total - 1), 0);

        if (received < 0) {
            snprintf(error_message, error_size, "Failed to read from socket");
            return 0;
        }

        if (received == 0) {
            break;
        }

        total += (size_t)received;
        buffer[total] = '\0';

        if (!headers_ready) {
            char *header_end = strstr(buffer, "\r\n\r\n");

            if (header_end != NULL) {
                headers_ready = 1;
                expected_total = (size_t)(header_end - buffer) + 4U + http_server_parse_content_length(buffer);
                if (expected_total + 1 > buffer_size) {
                    snprintf(error_message, error_size, "HTTP request exceeds buffer limit");
                    return 0;
                }
            }
        }

        if (headers_ready && total >= expected_total) {
            return 1;
        }
    }

    if (!headers_ready) {
        snprintf(error_message, error_size, "Incomplete HTTP request headers");
    } else {
        snprintf(error_message, error_size, "Incomplete HTTP request body");
    }

    return 0;
}

static int http_request_queue_init(HTTPRequestQueue *queue, size_t capacity) {
    if (queue == NULL || capacity == 0) {
        return 0;
    }

    memset(queue, 0, sizeof(*queue));
    queue->items = (HTTPSocket *)malloc(sizeof(*queue->items) * capacity);
    if (queue->items == NULL) {
        return 0;
    }

    queue->capacity = capacity;
    if (!platform_mutex_init(&queue->mutex)) {
        free(queue->items);
        queue->items = NULL;
        return 0;
    }

    if (!platform_cond_init(&queue->cond)) {
        platform_mutex_destroy(&queue->mutex);
        free(queue->items);
        queue->items = NULL;
        return 0;
    }

    return 1;
}

static void http_request_queue_destroy(HTTPRequestQueue *queue) {
    if (queue == NULL) {
        return;
    }

    platform_cond_destroy(&queue->cond);
    platform_mutex_destroy(&queue->mutex);
    free(queue->items);
    queue->items = NULL;
}

static void http_request_queue_shutdown(HTTPRequestQueue *queue) {
    platform_mutex_lock(&queue->mutex);
    queue->shutting_down = 1;
    platform_cond_broadcast(&queue->cond);
    platform_mutex_unlock(&queue->mutex);
}

static int http_request_queue_push(HTTPRequestQueue *queue, HTTPSocket client_socket) {
    int pushed = 0;

    platform_mutex_lock(&queue->mutex);
    if (!queue->shutting_down && queue->count < queue->capacity) {
        queue->items[queue->tail] = client_socket;
        queue->tail = (queue->tail + 1U) % queue->capacity;
        queue->count++;
        pushed = 1;
        platform_cond_signal(&queue->cond);
    }
    platform_mutex_unlock(&queue->mutex);

    return pushed;
}

static HTTPSocket http_request_queue_pop(HTTPRequestQueue *queue) {
    HTTPSocket client_socket = HTTP_INVALID_SOCKET;

    platform_mutex_lock(&queue->mutex);
    while (queue->count == 0 && !queue->shutting_down) {
        platform_cond_wait(&queue->cond, &queue->mutex);
    }

    if (queue->count > 0) {
        client_socket = queue->items[queue->head];
        queue->head = (queue->head + 1U) % queue->capacity;
        queue->count--;
    }
    platform_mutex_unlock(&queue->mutex);

    return client_socket;
}

static void http_server_note_response_complete(HTTPServerContext *context) {
    platform_mutex_lock(&context->state_mutex);
    context->completed_responses++;
    if (context->options.max_requests > 0 &&
        context->completed_responses >= context->options.max_requests) {
        http_server_request_stop();
    }
    platform_mutex_unlock(&context->state_mutex);
}

static int http_server_send_response(HTTPSocket client_socket, APIResponse *response) {
    char *raw_response = NULL;
    int sent;

    if (!api_render_http_response(response, &raw_response)) {
        return 0;
    }

    sent = http_server_socket_send_all(client_socket, raw_response, strlen(raw_response));
    free(raw_response);
    return sent;
}

static void http_server_send_error_and_count(HTTPServerContext *context, HTTPSocket client_socket, int status_code, const char *error_type, const char *message) {
    APIResponse response;

    memset(&response, 0, sizeof(response));
    if (api_build_error_response(&response, status_code, error_type, message)) {
        http_server_send_response(client_socket, &response);
    }
    api_response_destroy(&response);
    http_server_note_response_complete(context);
}

static void http_server_handle_client(HTTPServerContext *context, HTTPSocket client_socket) {
    APIRequest request;
    APIResponse response;
    DBServerExecution execution;
    DBServerMetrics metrics;
    char request_buffer[HTTP_SERVER_REQUEST_BUFFER_SIZE];
    char error_message[256];

    memset(&response, 0, sizeof(response));
    memset(&request, 0, sizeof(request));

    if (!http_server_read_request(client_socket, request_buffer, sizeof(request_buffer), error_message, sizeof(error_message))) {
        http_server_send_error_and_count(context, client_socket, 400, "malformed_http", error_message);
        return;
    }

    if (!api_parse_http_request(request_buffer, &request, error_message, sizeof(error_message))) {
        http_server_send_error_and_count(context, client_socket, 400, "malformed_http", error_message);
        return;
    }

    if (request.method == API_METHOD_GET && strcmp(request.path, "/health") == 0) {
        db_server_record_health_request(&context->db_server);
        if (api_build_health_response(&response)) {
            http_server_send_response(client_socket, &response);
        }
        api_response_destroy(&response);
        http_server_note_response_complete(context);
        return;
    }

    if (request.method == API_METHOD_GET && strcmp(request.path, "/metrics") == 0) {
        db_server_record_metrics_request(&context->db_server);
        db_server_get_metrics(&context->db_server, &metrics);
        if (api_build_metrics_response(&metrics, &response)) {
            http_server_send_response(client_socket, &response);
        }
        api_response_destroy(&response);
        http_server_note_response_complete(context);
        return;
    }

    if (request.method == API_METHOD_POST && strcmp(request.path, "/query") == 0) {
        if (!db_server_execute(&context->db_server, request.query, &execution)) {
            http_server_send_error_and_count(context, client_socket, 500, "internal_error", "Failed to execute query");
            return;
        }

        if (!api_build_execution_response(&execution, &response)) {
            db_server_execution_destroy(&execution);
            http_server_send_error_and_count(context, client_socket, 500, "internal_error", "Failed to serialize query result");
            return;
        }

        http_server_send_response(client_socket, &response);
        api_response_destroy(&response);
        db_server_execution_destroy(&execution);
        http_server_note_response_complete(context);
        return;
    }

    if ((strcmp(request.path, "/health") == 0 || strcmp(request.path, "/metrics") == 0) &&
        request.method != API_METHOD_GET) {
        http_server_send_error_and_count(context, client_socket, 405, "method_not_allowed", "This endpoint only supports GET");
        return;
    }

    if (strcmp(request.path, "/query") == 0 && request.method != API_METHOD_POST) {
        http_server_send_error_and_count(context, client_socket, 405, "method_not_allowed", "This endpoint only supports POST");
        return;
    }

    if (request.method == API_METHOD_UNKNOWN) {
        http_server_send_error_and_count(context, client_socket, 405, "method_not_allowed", "Unsupported HTTP method");
        return;
    }

    http_server_send_error_and_count(context, client_socket, 404, "not_found", "Unknown endpoint");
}

static void *http_server_worker_main(void *raw_context) {
    HTTPServerContext *context = (HTTPServerContext *)raw_context;

    while (1) {
        HTTPSocket client_socket = http_request_queue_pop(&context->queue);

        if (client_socket == HTTP_INVALID_SOCKET) {
            if (http_server_should_stop() || context->queue.shutting_down) {
                break;
            }
            continue;
        }

        http_server_handle_client(context, client_socket);
        http_server_socket_close(client_socket);
    }

    return NULL;
}

static HTTPSocket http_server_create_listen_socket(unsigned short port) {
    HTTPSocket listen_socket;
    struct sockaddr_in address;
    int reuse_value = 1;

    listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket == HTTP_INVALID_SOCKET) {
        return HTTP_INVALID_SOCKET;
    }

    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse_value, (int)sizeof(reuse_value));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (bind(listen_socket, (struct sockaddr *)&address, sizeof(address)) != 0) {
        http_server_socket_close(listen_socket);
        return HTTP_INVALID_SOCKET;
    }

    if (listen(listen_socket, 16) != 0) {
        http_server_socket_close(listen_socket);
        return HTTP_INVALID_SOCKET;
    }

    return listen_socket;
}

void http_server_options_default(HTTPServerOptions *options) {
    if (options == NULL) {
        return;
    }

    options->port = 8080;
    options->worker_count = 4;
    options->queue_capacity = 16;
    options->lock_timeout_ms = 1000;
    options->simulate_read_delay_ms = 0;
    options->simulate_write_delay_ms = 0;
    options->max_requests = 0;
}

int http_server_run(const HTTPServerOptions *options) {
    HTTPServerContext context;
    HTTPServerOptions effective_options;
    DBServerConfig db_config;
    HTTPSocket listen_socket = HTTP_INVALID_SOCKET;
    size_t worker_index;
    int exit_code = 1;

    memset(&context, 0, sizeof(context));
    http_server_options_default(&effective_options);
    if (options != NULL) {
        effective_options = *options;
    }

    if (effective_options.worker_count == 0 || effective_options.queue_capacity == 0) {
        fprintf(stderr, "workers and queue capacity must be greater than zero.\n");
        return 1;
    }

    if (!http_server_socket_runtime_init()) {
        fprintf(stderr, "Failed to initialize socket runtime.\n");
        return 1;
    }

    if (!http_server_install_signal_handlers()) {
        fprintf(stderr, "Failed to install shutdown signal handlers.\n");
        http_server_socket_runtime_cleanup();
        return 1;
    }

    http_server_stop_requested = 0;

    db_server_config_default(&db_config);
    db_config.lock_timeout_ms = effective_options.lock_timeout_ms;
    db_config.simulate_read_delay_ms = effective_options.simulate_read_delay_ms;
    db_config.simulate_write_delay_ms = effective_options.simulate_write_delay_ms;

    if (!db_server_init_with_config(&context.db_server, &db_config)) {
        fprintf(stderr, "Failed to initialize shared DB server.\n");
        http_server_socket_runtime_cleanup();
        return 1;
    }

    context.options = effective_options;

    if (!platform_mutex_init(&context.state_mutex)) {
        fprintf(stderr, "Failed to initialize server state mutex.\n");
        db_server_destroy(&context.db_server);
        http_server_socket_runtime_cleanup();
        return 1;
    }

    if (!http_request_queue_init(&context.queue, effective_options.queue_capacity)) {
        fprintf(stderr, "Failed to initialize request queue.\n");
        platform_mutex_destroy(&context.state_mutex);
        db_server_destroy(&context.db_server);
        http_server_socket_runtime_cleanup();
        return 1;
    }

    context.workers = (PlatformThread *)malloc(sizeof(*context.workers) * effective_options.worker_count);
    if (context.workers == NULL) {
        fprintf(stderr, "Failed to allocate worker threads.\n");
        http_request_queue_destroy(&context.queue);
        platform_mutex_destroy(&context.state_mutex);
        db_server_destroy(&context.db_server);
        http_server_socket_runtime_cleanup();
        return 1;
    }

    for (worker_index = 0; worker_index < effective_options.worker_count; worker_index++) {
        if (!platform_thread_create(&context.workers[worker_index], http_server_worker_main, &context)) {
            fprintf(stderr, "Failed to start worker thread %lu.\n", (unsigned long)worker_index);
            http_server_request_stop();
            context.queue.shutting_down = 1;
            worker_index++;
            while (worker_index > 1) {
                worker_index--;
                platform_thread_join(context.workers[worker_index - 1]);
            }
            free(context.workers);
            http_request_queue_destroy(&context.queue);
            platform_mutex_destroy(&context.state_mutex);
            db_server_destroy(&context.db_server);
            http_server_socket_runtime_cleanup();
            return 1;
        }
    }

    listen_socket = http_server_create_listen_socket(effective_options.port);
    if (listen_socket == HTTP_INVALID_SOCKET) {
        fprintf(stderr, "Failed to bind HTTP server to port %u.\n", (unsigned int)effective_options.port);
        http_server_request_stop();
        http_request_queue_shutdown(&context.queue);
        for (worker_index = 0; worker_index < effective_options.worker_count; worker_index++) {
            platform_thread_join(context.workers[worker_index]);
        }
        free(context.workers);
        http_request_queue_destroy(&context.queue);
        platform_mutex_destroy(&context.state_mutex);
        db_server_destroy(&context.db_server);
        http_server_socket_runtime_cleanup();
        return 1;
    }

    printf("HTTP SQL server listening on port %u with %lu workers and queue size %lu.\n",
           (unsigned int)effective_options.port,
           (unsigned long)effective_options.worker_count,
           (unsigned long)effective_options.queue_capacity);

    while (!http_server_should_stop()) {
        int ready = http_server_socket_wait_for_read(listen_socket, 200);

        if (ready < 0) {
            break;
        }

        if (ready == 0) {
            continue;
        }

        if (ready > 0) {
            HTTPSocket client_socket = accept(listen_socket, NULL, NULL);

            if (client_socket == HTTP_INVALID_SOCKET) {
                continue;
            }

            if (!http_request_queue_push(&context.queue, client_socket)) {
                db_server_record_queue_full(&context.db_server);
                http_server_send_error_and_count(&context, client_socket, 503, "queue_full", "Worker queue is full");
                http_server_socket_close(client_socket);
            }
        }
    }

    http_request_queue_shutdown(&context.queue);
    for (worker_index = 0; worker_index < effective_options.worker_count; worker_index++) {
        platform_thread_join(context.workers[worker_index]);
    }

    exit_code = 0;

    if (listen_socket != HTTP_INVALID_SOCKET) {
        http_server_socket_close(listen_socket);
    }
    free(context.workers);
    http_request_queue_destroy(&context.queue);
    platform_mutex_destroy(&context.state_mutex);
    db_server_destroy(&context.db_server);
    http_server_socket_runtime_cleanup();

    return exit_code;
}
