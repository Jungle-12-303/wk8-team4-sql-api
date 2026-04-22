#include "api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void api_set_error(char *error_message, size_t error_size, const char *message) {
    if (error_message == NULL || error_size == 0) {
        return;
    }

    snprintf(error_message, error_size, "%s", message);
}

static int api_ascii_case_equal_char(char left, char right) {
    return tolower((unsigned char)left) == tolower((unsigned char)right);
}

static int api_header_name_equals(const char *left, const char *right, size_t length) {
    size_t index;
    size_t right_length = strlen(right);

    if (length != right_length) {
        return 0;
    }

    for (index = 0; index < length; index++) {
        if (!api_ascii_case_equal_char(left[index], right[index])) {
            return 0;
        }
    }

    return 1;
}

static const char *api_skip_spaces(const char *cursor, const char *end) {
    while (cursor < end && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor;
}

static int api_parse_json_string(const char **cursor_ptr, const char *end, char *buffer, size_t buffer_size) {
    const char *cursor = *cursor_ptr;
    size_t length = 0;

    if (cursor >= end || *cursor != '"') {
        return 0;
    }

    cursor++;
    while (cursor < end) {
        char current = *cursor;

        if (current == '"') {
            buffer[length] = '\0';
            *cursor_ptr = cursor + 1;
            return 1;
        }

        if (current == '\\') {
            char escaped;

            cursor++;
            if (cursor >= end) {
                return 0;
            }

            escaped = *cursor;
            if (escaped == '"' || escaped == '\\' || escaped == '/') {
                current = escaped;
            } else if (escaped == 'n') {
                current = '\n';
            } else if (escaped == 'r') {
                current = '\r';
            } else if (escaped == 't') {
                current = '\t';
            } else {
                return 0;
            }
        }

        if (length + 1 >= buffer_size) {
            return 0;
        }

        buffer[length++] = current;
        cursor++;
    }

    return 0;
}

static int api_extract_query_string(const char *body, size_t body_length, char *query, size_t query_size) {
    const char *cursor = body;
    const char *end = body + body_length;
    char key[64];

    cursor = api_skip_spaces(cursor, end);
    if (cursor >= end || *cursor != '{') {
        return 0;
    }

    cursor++;
    while (cursor < end) {
        cursor = api_skip_spaces(cursor, end);
        if (cursor < end && *cursor == '}') {
            break;
        }

        if (!api_parse_json_string(&cursor, end, key, sizeof(key))) {
            return 0;
        }

        cursor = api_skip_spaces(cursor, end);
        if (cursor >= end || *cursor != ':') {
            return 0;
        }

        cursor++;
        cursor = api_skip_spaces(cursor, end);

        if (strcmp(key, "query") == 0) {
            if (!api_parse_json_string(&cursor, end, query, query_size)) {
                return 0;
            }
            return 1;
        }

        if (*cursor == '"') {
            char ignored[16];

            if (!api_parse_json_string(&cursor, end, ignored, sizeof(ignored))) {
                return 0;
            }
        } else {
            while (cursor < end && *cursor != ',' && *cursor != '}') {
                cursor++;
            }
        }

        cursor = api_skip_spaces(cursor, end);
        if (cursor < end && *cursor == ',') {
            cursor++;
        }
    }

    return 0;
}

static char *api_escape_json_string_alloc(const char *input) {
    size_t input_length = strlen(input);
    size_t capacity = input_length * 6 + 1;
    char *escaped = (char *)malloc(capacity);
    size_t out = 0;
    size_t index;

    if (escaped == NULL) {
        return NULL;
    }

    for (index = 0; index < input_length; index++) {
        unsigned char current = (unsigned char)input[index];

        if (current == '"' || current == '\\') {
            escaped[out++] = '\\';
            escaped[out++] = (char)current;
        } else if (current == '\n') {
            escaped[out++] = '\\';
            escaped[out++] = 'n';
        } else if (current == '\r') {
            escaped[out++] = '\\';
            escaped[out++] = 'r';
        } else if (current == '\t') {
            escaped[out++] = '\\';
            escaped[out++] = 't';
        } else if (current < 0x20U) {
            out += (size_t)snprintf(escaped + out, capacity - out, "\\u%04x", current);
        } else {
            escaped[out++] = (char)current;
        }
    }

    escaped[out] = '\0';
    return escaped;
}

static int api_set_response_body(APIResponse *response, int status_code, const char *body) {
    size_t body_length = strlen(body) + 1;

    response->body = (char *)malloc(body_length);
    if (response->body == NULL) {
        return 0;
    }

    memcpy(response->body, body, body_length);
    response->status_code = status_code;
    response->content_type = "application/json; charset=utf-8";
    return 1;
}

static const char *api_reason_phrase(int status_code) {
    if (status_code == 200) {
        return "OK";
    }
    if (status_code == 400) {
        return "Bad Request";
    }
    if (status_code == 404) {
        return "Not Found";
    }
    if (status_code == 405) {
        return "Method Not Allowed";
    }
    if (status_code == 413) {
        return "Payload Too Large";
    }
    if (status_code == 500) {
        return "Internal Server Error";
    }
    if (status_code == 503) {
        return "Service Unavailable";
    }
    return "Error";
}

int api_parse_http_request(const char *raw_request, APIRequest *request, char *error_message, size_t error_size) {
    const char *line_end;
    const char *header_cursor;
    const char *header_end;
    const char *body;
    char method[8];
    char version[16];
    int scanned;
    size_t path_length;
    size_t content_length = 0;

    if (raw_request == NULL || request == NULL) {
        api_set_error(error_message, error_size, "Request buffer is missing");
        return 0;
    }

    memset(request, 0, sizeof(*request));

    line_end = strstr(raw_request, "\r\n");
    if (line_end == NULL) {
        api_set_error(error_message, error_size, "Malformed HTTP request line");
        return 0;
    }

    scanned = sscanf(raw_request, "%7s %63s %15s", method, request->path, version);
    if (scanned != 3) {
        api_set_error(error_message, error_size, "Malformed HTTP request line");
        return 0;
    }

    path_length = strlen(request->path);
    if (path_length == 0) {
        api_set_error(error_message, error_size, "Request path is empty");
        return 0;
    }

    if (strcmp(method, "GET") == 0) {
        request->method = API_METHOD_GET;
    } else if (strcmp(method, "POST") == 0) {
        request->method = API_METHOD_POST;
    } else {
        request->method = API_METHOD_UNKNOWN;
    }

    header_end = strstr(raw_request, "\r\n\r\n");
    if (header_end == NULL) {
        api_set_error(error_message, error_size, "Malformed HTTP headers");
        return 0;
    }

    header_cursor = line_end + 2;
    while (header_cursor < header_end) {
        const char *next_line = strstr(header_cursor, "\r\n");
        const char *colon;
        size_t name_length;

        if (next_line == NULL || next_line > header_end) {
            api_set_error(error_message, error_size, "Malformed HTTP header line");
            return 0;
        }

        colon = memchr(header_cursor, ':', (size_t)(next_line - header_cursor));
        if (colon != NULL) {
            name_length = (size_t)(colon - header_cursor);
            if (api_header_name_equals(header_cursor, "Content-Length", name_length)) {
                content_length = (size_t)strtoul(colon + 1, NULL, 10);
            }
        }

        header_cursor = next_line + 2;
    }

    request->content_length = content_length;
    body = header_end + 4;

    if (request->method == API_METHOD_POST && strcmp(request->path, "/query") == 0) {
        if (content_length == 0) {
            api_set_error(error_message, error_size, "POST /query requires a JSON body");
            return 0;
        }

        if (!api_extract_query_string(body, content_length, request->query, sizeof(request->query))) {
            api_set_error(error_message, error_size, "JSON body must contain a string field named query");
            return 0;
        }
    }

    api_set_error(error_message, error_size, "");
    return 1;
}

int api_build_health_response(APIResponse *response) {
    return api_set_response_body(response, 200, "{\"ok\":true,\"status\":\"healthy\"}");
}

int api_build_metrics_response(const DBServerMetrics *metrics, APIResponse *response) {
    char body[1024];

    if (metrics == NULL || response == NULL) {
        return 0;
    }

    snprintf(
        body,
        sizeof(body),
        "{\"ok\":true,\"status\":\"ok\",\"metrics\":{\"totalRequests\":%lu,\"totalHealthRequests\":%lu,\"totalMetricsRequests\":%lu,\"totalQueryRequests\":%lu,\"totalSelectRequests\":%lu,\"totalInsertRequests\":%lu,\"totalErrors\":%lu,\"totalSyntaxErrors\":%lu,\"totalQueryErrors\":%lu,\"totalInternalErrors\":%lu,\"totalNotFoundResults\":%lu,\"totalQueueFull\":%lu,\"totalLockTimeouts\":%lu,\"activeQueryRequests\":%lu}}",
        (unsigned long)metrics->total_requests,
        (unsigned long)metrics->total_health_requests,
        (unsigned long)metrics->total_metrics_requests,
        (unsigned long)metrics->total_query_requests,
        (unsigned long)metrics->total_select_requests,
        (unsigned long)metrics->total_insert_requests,
        (unsigned long)metrics->total_errors,
        (unsigned long)metrics->total_syntax_errors,
        (unsigned long)metrics->total_query_errors,
        (unsigned long)metrics->total_internal_errors,
        (unsigned long)metrics->total_not_found_results,
        (unsigned long)metrics->total_queue_full,
        (unsigned long)metrics->total_lock_timeouts,
        (unsigned long)metrics->active_query_requests
    );

    return api_set_response_body(response, 200, body);
}

int api_build_error_response(APIResponse *response, int status_code, const char *error_type, const char *message) {
    char *escaped_message;
    char body[2048];

    if (response == NULL || error_type == NULL || message == NULL) {
        return 0;
    }

    escaped_message = api_escape_json_string_alloc(message);
    if (escaped_message == NULL) {
        return 0;
    }

    snprintf(
        body,
        sizeof(body),
        "{\"ok\":false,\"status\":\"%s\",\"error\":\"%s\",\"message\":\"%s\"}",
        error_type,
        error_type,
        escaped_message
    );

    free(escaped_message);
    return api_set_response_body(response, status_code, body);
}

int api_build_execution_response(const DBServerExecution *execution, APIResponse *response) {
    size_t index;

    if (execution == NULL || response == NULL) {
        return 0;
    }

    if (execution->server_status == DB_SERVER_EXEC_STATUS_LOCK_TIMEOUT) {
        return api_build_error_response(response, 503, "lock_timeout", execution->message);
    }

    if (execution->result.status == SQL_STATUS_SYNTAX_ERROR) {
        return api_build_error_response(response, 400, "syntax_error", execution->result.error_message);
    }

    if (execution->result.status == SQL_STATUS_QUERY_ERROR) {
        return api_build_error_response(response, 400, "query_error", execution->result.error_message);
    }

    if (execution->result.status == SQL_STATUS_ERROR) {
        const char *message = execution->result.error_message[0] != '\0'
            ? execution->result.error_message
            : "Internal execution error";
        return api_build_error_response(response, 500, "internal_error", message);
    }

    if (execution->result.status == SQL_STATUS_EXIT) {
        return api_build_error_response(response, 400, "query_error", "EXIT and QUIT are only supported in CLI mode");
    }

    if (execution->result.action == SQL_ACTION_INSERT) {
        char body[256];

        snprintf(
            body,
            sizeof(body),
            "{\"ok\":true,\"status\":\"ok\",\"action\":\"insert\",\"insertedId\":%d,\"usedIndex\":false}",
            execution->result.inserted_id
        );
        return api_set_response_body(response, 200, body);
    }

    if (execution->result.action == SQL_ACTION_SELECT_ROWS) {
        size_t capacity = 160;
        char *body;
        size_t offset;

        for (index = 0; index < execution->result.row_count; index++) {
            capacity += strlen(execution->result.records[index]->name) * 6 + 96;
        }

        body = (char *)malloc(capacity);
        if (body == NULL) {
            return 0;
        }

        offset = (size_t)snprintf(
            body,
            capacity,
            "{\"ok\":true,\"status\":\"ok\",\"action\":\"select\",\"rowCount\":%lu,\"usedIndex\":%s,\"rows\":[",
            (unsigned long)execution->result.row_count,
            execution->used_index ? "true" : "false"
        );

        for (index = 0; index < execution->result.row_count; index++) {
            char *escaped_name = api_escape_json_string_alloc(execution->result.records[index]->name);

            if (escaped_name == NULL) {
                free(body);
                return 0;
            }

            offset += (size_t)snprintf(
                body + offset,
                capacity - offset,
                "%s{\"id\":%d,\"name\":\"%s\",\"age\":%d}",
                (index == 0) ? "" : ",",
                execution->result.records[index]->id,
                escaped_name,
                execution->result.records[index]->age
            );

            free(escaped_name);
        }

        snprintf(body + offset, capacity - offset, "]}");
        response->status_code = 200;
        response->content_type = "application/json; charset=utf-8";
        response->body = body;
        return 1;
    }

    return api_build_error_response(response, 500, "internal_error", "Unsupported execution result");
}

int api_render_http_response(const APIResponse *response, char **raw_response) {
    const char *reason_phrase;
    size_t body_length;
    size_t capacity;
    int written;

    if (response == NULL || response->body == NULL || raw_response == NULL) {
        return 0;
    }

    reason_phrase = api_reason_phrase(response->status_code);
    body_length = strlen(response->body);
    capacity = body_length + 256;
    *raw_response = (char *)malloc(capacity);
    if (*raw_response == NULL) {
        return 0;
    }

    written = snprintf(
        *raw_response,
        capacity,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n%s",
        response->status_code,
        reason_phrase,
        response->content_type != NULL ? response->content_type : "application/json; charset=utf-8",
        (unsigned long)body_length,
        response->body
    );

    if (written < 0 || (size_t)written >= capacity) {
        free(*raw_response);
        *raw_response = NULL;
        return 0;
    }

    return 1;
}

void api_response_destroy(APIResponse *response) {
    if (response == NULL) {
        return;
    }

    free(response->body);
    response->body = NULL;
    response->content_type = NULL;
    response->status_code = 0;
}
