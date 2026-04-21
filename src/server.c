#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "sqlproc.h"

/*
 * HTTP API 서버입니다.
 *
 * CS:APP의 세 가지 개념을 한 파일에 모았습니다.
 * - Chapter 10: rio_* 함수로 소켓 short count를 안전하게 처리
 * - Chapter 11: open_listenfd -> accept -> connected descriptor 처리
 * - Chapter 12: bounded queue + prethreaded worker pool
 */

#define RIO_BUFSIZE 8192
#define LISTEN_BACKLOG 1024
#define HTTP_MAX_LINE 1024
#define HTTP_MAX_METHOD 16
#define HTTP_MAX_PATH 256
#define HTTP_MAX_VERSION 32
#define HTTP_HEADER_MAX 512
#define HTTP_MAX_BODY_SIZE (SQLPROC_MAX_SQL_SIZE - 1)

typedef struct {
    int rio_fd;
    int rio_cnt;
    char *rio_bufptr;
    char rio_buf[RIO_BUFSIZE];
} Rio;

typedef struct {
    int *buf;
    int n;
    int front;
    int rear;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t slots;
    pthread_cond_t items;
} ConnectionQueue;

typedef struct {
    const AppConfig *config;
    ConnectionQueue *queue;
} WorkerContext;

static pthread_mutex_t sql_engine_mutex = PTHREAD_MUTEX_INITIALIZER;

static void set_server_error(ErrorInfo *error, const char *message)
{
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = 0;
    error->column = 0;
}

ssize_t rio_readn(int fd, void *usrbuf, size_t n)
{
    size_t nleft;
    ssize_t nread;
    char *bufp;

    nleft = n;
    bufp = (char *)usrbuf;

    while (nleft > 0) {
        nread = read(fd, bufp, nleft);
        if (nread < 0) {
            if (errno == EINTR) {
                nread = 0;
            } else {
                return -1;
            }
        } else if (nread == 0) {
            break;
        }

        nleft -= (size_t)nread;
        bufp += nread;
    }

    return (ssize_t)(n - nleft);
}

static ssize_t rio_writen(int fd, const void *usrbuf, size_t n)
{
    size_t nleft;
    ssize_t nwritten;
    const char *bufp;

    nleft = n;
    bufp = (const char *)usrbuf;

    while (nleft > 0) {
        nwritten = write(fd, bufp, nleft);
        if (nwritten <= 0) {
            if (nwritten < 0 && errno == EINTR) {
                nwritten = 0;
            } else {
                return -1;
            }
        }

        nleft -= (size_t)nwritten;
        bufp += nwritten;
    }

    return (ssize_t)n;
}

static void rio_readinitb(Rio *rp, int fd)
{
    rp->rio_fd = fd;
    rp->rio_cnt = 0;
    rp->rio_bufptr = rp->rio_buf;
}

static ssize_t rio_read(Rio *rp, char *usrbuf, size_t n)
{
    int cnt;

    while (rp->rio_cnt <= 0) {
        rp->rio_cnt = (int)read(rp->rio_fd, rp->rio_buf, sizeof(rp->rio_buf));
        if (rp->rio_cnt < 0) {
            if (errno != EINTR) {
                return -1;
            }
        } else if (rp->rio_cnt == 0) {
            return 0;
        } else {
            rp->rio_bufptr = rp->rio_buf;
        }
    }

    cnt = (int)n;
    if (rp->rio_cnt < cnt) {
        cnt = rp->rio_cnt;
    }

    memcpy(usrbuf, rp->rio_bufptr, (size_t)cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}

static ssize_t rio_readlineb(Rio *rp, void *usrbuf, size_t maxlen)
{
    size_t n;
    ssize_t rc;
    char c;
    char *bufp;

    bufp = (char *)usrbuf;

    for (n = 1; n < maxlen; n++) {
        rc = rio_read(rp, &c, 1);
        if (rc == 1) {
            *bufp = c;
            bufp += 1;
            if (c == '\n') {
                n += 1;
                break;
            }
        } else if (rc == 0) {
            if (n == 1) {
                return 0;
            }
            break;
        } else {
            return -1;
        }
    }

    *bufp = '\0';
    return (ssize_t)(n - 1);
}

static ssize_t rio_readnb(Rio *rp, void *usrbuf, size_t n)
{
    size_t nleft;
    ssize_t nread;
    char *bufp;

    nleft = n;
    bufp = (char *)usrbuf;

    while (nleft > 0) {
        nread = rio_read(rp, bufp, nleft);
        if (nread < 0) {
            return -1;
        }

        if (nread == 0) {
            break;
        }

        nleft -= (size_t)nread;
        bufp += nread;
    }

    return (ssize_t)(n - nleft);
}

static int open_listenfd(const char *port)
{
    struct addrinfo hints;
    struct addrinfo *listp;
    struct addrinfo *p;
    int listenfd;
    int optval;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    rc = getaddrinfo(NULL, port, &hints, &listp);
    if (rc != 0) {
        return -1;
    }

    optval = 1;
    listenfd = -1;
    for (p = listp; p != NULL; p = p->ai_next) {
        listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listenfd < 0) {
            continue;
        }

        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }

        close(listenfd);
        listenfd = -1;
    }

    freeaddrinfo(listp);
    if (listenfd < 0) {
        return -1;
    }

    if (listen(listenfd, LISTEN_BACKLOG) < 0) {
        close(listenfd);
        return -1;
    }

    return listenfd;
}

static int connection_queue_init(ConnectionQueue *queue, int n)
{
    queue->buf = (int *)calloc((size_t)n, sizeof(int));
    if (queue->buf == NULL) {
        return 0;
    }

    queue->n = n;
    queue->front = 0;
    queue->rear = 0;
    queue->count = 0;

    if (pthread_mutex_init(&queue->mutex, NULL) != 0 ||
        pthread_cond_init(&queue->slots, NULL) != 0 ||
        pthread_cond_init(&queue->items, NULL) != 0) {
        free(queue->buf);
        queue->buf = NULL;
        return 0;
    }

    return 1;
}

static void connection_queue_insert(ConnectionQueue *queue, int item)
{
    pthread_mutex_lock(&queue->mutex);
    while (queue->count == queue->n) {
        pthread_cond_wait(&queue->slots, &queue->mutex);
    }

    queue->buf[queue->rear] = item;
    queue->rear = (queue->rear + 1) % queue->n;
    queue->count += 1;

    pthread_cond_signal(&queue->items);
    pthread_mutex_unlock(&queue->mutex);
}

static int connection_queue_remove(ConnectionQueue *queue)
{
    int item;

    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0) {
        pthread_cond_wait(&queue->items, &queue->mutex);
    }

    item = queue->buf[queue->front];
    queue->front = (queue->front + 1) % queue->n;
    queue->count -= 1;

    pthread_cond_signal(&queue->slots);
    pthread_mutex_unlock(&queue->mutex);
    return item;
}

static int line_is_blank(const char *line)
{
    return strcmp(line, "\n") == 0 || strcmp(line, "\r\n") == 0;
}

static int starts_with_header_name(const char *line, const char *name)
{
    size_t i;

    for (i = 0; name[i] != '\0'; i++) {
        if (tolower((unsigned char)line[i]) != tolower((unsigned char)name[i])) {
            return 0;
        }
    }

    return line[i] == ':';
}

static int parse_content_length(const char *line, long *content_length)
{
    const char *cursor;
    char *end_ptr;
    long parsed_value;

    cursor = strchr(line, ':');
    if (cursor == NULL) {
        return 0;
    }

    cursor += 1;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor += 1;
    }

    errno = 0;
    parsed_value = strtol(cursor, &end_ptr, 10);
    if (errno == ERANGE || parsed_value < 0) {
        return 0;
    }

    while (*end_ptr == ' ' || *end_ptr == '\t' ||
           *end_ptr == '\r' || *end_ptr == '\n') {
        end_ptr += 1;
    }

    if (*end_ptr != '\0') {
        return 0;
    }

    *content_length = parsed_value;
    return 1;
}

static int format_response_header(char *dest,
                                  size_t dest_size,
                                  int status_code,
                                  const char *reason,
                                  long content_length)
{
    int header_length;

    header_length = snprintf(dest,
                             dest_size,
                             "HTTP/1.0 %d %s\r\n"
                             "Server: sqlproc\r\n"
                             "Content-Type: text/plain; charset=utf-8\r\n"
                             "Content-Length: %ld\r\n"
                             "Connection: close\r\n"
                             "\r\n",
                             status_code,
                             reason,
                             content_length);
    if (header_length < 0 || (size_t)header_length >= dest_size) {
        return 0;
    }

    return header_length;
}

static int write_response_header(int connfd,
                                 int status_code,
                                 const char *reason,
                                 long content_length)
{
    char header[HTTP_HEADER_MAX];
    int header_length;

    header_length = format_response_header(header,
                                           sizeof(header),
                                           status_code,
                                           reason,
                                           content_length);
    if (header_length <= 0) {
        return 0;
    }

    return rio_writen(connfd, header, (size_t)header_length) == header_length;
}

static int send_text_response(int connfd, int status_code, const char *reason, const char *body)
{
    long body_length;

    body_length = (long)strlen(body);
    if (!write_response_header(connfd, status_code, reason, body_length)) {
        return 0;
    }

    if (body_length == 0) {
        return 1;
    }

    return rio_writen(connfd, body, (size_t)body_length) == body_length;
}

static int send_file_response(int connfd, FILE *body_file, long body_length)
{
    char buffer[4096];
    long remaining;

    if (!write_response_header(connfd, 200, "OK", body_length)) {
        return 0;
    }

    rewind(body_file);
    remaining = body_length;
    while (remaining > 0) {
        size_t chunk_size;
        size_t read_size;

        chunk_size = sizeof(buffer);
        if (remaining < (long)chunk_size) {
            chunk_size = (size_t)remaining;
        }

        read_size = fread(buffer, 1, chunk_size, body_file);
        if (read_size == 0) {
            return 0;
        }

        if (rio_writen(connfd, buffer, read_size) != (ssize_t)read_size) {
            return 0;
        }

        remaining -= (long)read_size;
    }

    return 1;
}

static int send_sql_success_response(int connfd, FILE *output_file)
{
    long body_length;

    fflush(output_file);
    if (fseek(output_file, 0, SEEK_END) != 0) {
        return send_text_response(connfd, 500, "Internal Server Error", "결과 길이를 계산할 수 없습니다.\n");
    }

    body_length = ftell(output_file);
    if (body_length < 0) {
        return send_text_response(connfd, 500, "Internal Server Error", "결과 길이를 계산할 수 없습니다.\n");
    }

    if (body_length == 0) {
        return send_text_response(connfd, 200, "OK", "OK\n");
    }

    return send_file_response(connfd, output_file, body_length);
}

static int execute_sql_request(int connfd, const AppConfig *base_config, const char *sql_text)
{
    AppConfig request_config;
    ErrorInfo error;
    FILE *output_file;
    int ok;

    output_file = tmpfile();
    if (output_file == NULL) {
        return send_text_response(connfd, 500, "Internal Server Error", "결과 파일을 만들 수 없습니다.\n");
    }

    request_config = *base_config;
    request_config.output = output_file;

    pthread_mutex_lock(&sql_engine_mutex);
    ok = run_sql_text(&request_config, sql_text, &error);
    pthread_mutex_unlock(&sql_engine_mutex);

    if (!ok) {
        char body[SQLPROC_MAX_ERROR_LEN + 32];

        if (error.line > 0) {
            snprintf(body,
                     sizeof(body),
                     "오류: %s (line %d, column %d)\n",
                     error.message,
                     error.line,
                     error.column);
        } else {
            snprintf(body, sizeof(body), "오류: %s\n", error.message);
        }

        fclose(output_file);
        return send_text_response(connfd, 400, "Bad Request", body);
    }

    ok = send_sql_success_response(connfd, output_file);
    fclose(output_file);
    return ok;
}

static void discard_request_body(Rio *rio, long content_length)
{
    char discard[1024];
    long remaining;

    remaining = content_length;
    while (remaining > 0) {
        size_t chunk_size;
        ssize_t read_size;

        chunk_size = sizeof(discard);
        if (remaining < (long)chunk_size) {
            chunk_size = (size_t)remaining;
        }

        read_size = rio_readnb(rio, discard, chunk_size);
        if (read_size <= 0) {
            return;
        }

        remaining -= read_size;
    }
}

static int read_http_request_body(Rio *rio,
                                  char body[SQLPROC_MAX_SQL_SIZE],
                                  long content_length,
                                  int *status_code)
{
    ssize_t read_size;

    if (content_length == 0) {
        *status_code = 400;
        return 0;
    }

    if (content_length > HTTP_MAX_BODY_SIZE) {
        discard_request_body(rio, content_length);
        *status_code = 413;
        return 0;
    }

    read_size = rio_readnb(rio, body, (size_t)content_length);
    if (read_size != content_length) {
        *status_code = 400;
        return 0;
    }

    body[content_length] = '\0';
    return 1;
}

static int handle_client(int connfd, const AppConfig *config)
{
    Rio rio;
    char line[HTTP_MAX_LINE];
    char method[HTTP_MAX_METHOD];
    char path[HTTP_MAX_PATH];
    char version[HTTP_MAX_VERSION];
    char body[SQLPROC_MAX_SQL_SIZE];
    long content_length;
    int has_content_length;
    ssize_t line_size;

    rio_readinitb(&rio, connfd);
    line_size = rio_readlineb(&rio, line, sizeof(line));
    if (line_size <= 0) {
        return 0;
    }

    memset(method, 0, sizeof(method));
    memset(path, 0, sizeof(path));
    memset(version, 0, sizeof(version));
    if (sscanf(line, "%15s %255s %31s", method, path, version) != 3) {
        return send_text_response(connfd, 400, "Bad Request", "잘못된 HTTP 요청입니다.\n");
    }

    content_length = 0;
    has_content_length = 0;
    while (1) {
        line_size = rio_readlineb(&rio, line, sizeof(line));
        if (line_size <= 0) {
            return send_text_response(connfd, 400, "Bad Request", "HTTP 헤더를 읽을 수 없습니다.\n");
        }

        if (line_is_blank(line)) {
            break;
        }

        if (starts_with_header_name(line, "Content-Length")) {
            if (!parse_content_length(line, &content_length)) {
                return send_text_response(connfd, 400, "Bad Request", "Content-Length가 잘못되었습니다.\n");
            }

            has_content_length = 1;
        }
    }

    if (strcmp(path, "/health") == 0) {
        if (strcmp(method, "GET") != 0) {
            return send_text_response(connfd, 405, "Method Not Allowed", "지원하지 않는 메서드입니다.\n");
        }

        return send_text_response(connfd, 200, "OK", "OK\n");
    }

    if (strcmp(path, "/query") != 0) {
        return send_text_response(connfd, 404, "Not Found", "요청 경로를 찾을 수 없습니다.\n");
    }

    if (strcmp(method, "POST") != 0) {
        return send_text_response(connfd, 405, "Method Not Allowed", "지원하지 않는 메서드입니다.\n");
    }

    if (!has_content_length) {
        return send_text_response(connfd, 400, "Bad Request", "Content-Length가 필요합니다.\n");
    }

    {
        int status_code;

        status_code = 200;
        if (!read_http_request_body(&rio, body, content_length, &status_code)) {
            if (status_code == 413) {
                return send_text_response(connfd, 413, "Payload Too Large", "SQL 요청 본문이 너무 큽니다.\n");
            }

            return send_text_response(connfd, 400, "Bad Request", "SQL 요청 본문을 읽을 수 없습니다.\n");
        }
    }

    return execute_sql_request(connfd, config, body);
}

static void *worker_main(void *arg)
{
    WorkerContext *context;

    context = (WorkerContext *)arg;
    pthread_detach(pthread_self());

    while (1) {
        int connfd;

        connfd = connection_queue_remove(context->queue);
        handle_client(connfd, context->config);
        close(connfd);
    }

    return NULL;
}

int run_server(const AppConfig *config, ErrorInfo *error)
{
    ConnectionQueue queue;
    WorkerContext context;
    pthread_t tid;
    int listenfd;
    int i;

    listenfd = open_listenfd(config->port);
    if (listenfd < 0) {
        set_server_error(error, "서버 listen socket을 열 수 없습니다.");
        return 0;
    }

    if (!connection_queue_init(&queue, config->queue_size)) {
        close(listenfd);
        set_server_error(error, "서버 작업 큐를 초기화할 수 없습니다.");
        return 0;
    }

    context.config = config;
    context.queue = &queue;

    for (i = 0; i < config->thread_count; i++) {
        if (pthread_create(&tid, NULL, worker_main, &context) != 0) {
            close(listenfd);
            set_server_error(error, "worker thread를 만들 수 없습니다.");
            return 0;
        }
    }

    printf("sqlproc HTTP server listening on port %s with %d threads\n",
           config->port,
           config->thread_count);
    fflush(stdout);

    while (1) {
        struct sockaddr_storage clientaddr;
        socklen_t clientlen;
        int connfd;

        clientlen = sizeof(clientaddr);
        connfd = accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen);
        if (connfd < 0) {
            if (errno == EINTR) {
                continue;
            }

            continue;
        }

        connection_queue_insert(&queue, connfd);
    }
}

#ifdef SQLPROC_TEST
static void connection_queue_destroy(ConnectionQueue *queue)
{
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->slots);
    pthread_cond_destroy(&queue->items);
    free(queue->buf);
    queue->buf = NULL;
}

int sqlproc_test_parse_content_length(const char *line, long *content_length)
{
    return parse_content_length(line, content_length);
}

int sqlproc_test_format_response_header(char *dest,
                                        size_t dest_size,
                                        int status_code,
                                        const char *reason,
                                        long content_length)
{
    return format_response_header(dest, dest_size, status_code, reason, content_length);
}

int sqlproc_test_connection_queue_round_trip(void)
{
    ConnectionQueue queue;
    int first;
    int second;

    if (!connection_queue_init(&queue, 2)) {
        return 0;
    }

    connection_queue_insert(&queue, 11);
    connection_queue_insert(&queue, 22);
    first = connection_queue_remove(&queue);
    second = connection_queue_remove(&queue);
    connection_queue_destroy(&queue);

    return first == 11 && second == 22;
}
#endif
