#ifndef API_H
#define API_H

#include "db_server.h"

#include <stddef.h>

typedef enum APIRequestMethod {
    API_METHOD_UNKNOWN,
    API_METHOD_GET,
    API_METHOD_POST
} APIRequestMethod;

typedef struct APIRequest {
    APIRequestMethod method;
    char path[64];
    char query[1024];
    size_t content_length;
} APIRequest;

typedef struct APIResponse {
    int status_code;
    const char *content_type;
    char *body;
} APIResponse;

int api_parse_http_request(const char *raw_request, APIRequest *request, char *error_message, size_t error_size);

int api_build_health_response(APIResponse *response);
int api_build_metrics_response(const DBServerMetrics *metrics, APIResponse *response);
int api_build_execution_response(const DBServerExecution *execution, APIResponse *response);
int api_build_error_response(APIResponse *response, int status_code, const char *error_type, const char *message);
int api_render_http_response(const APIResponse *response, char **raw_response);

void api_response_destroy(APIResponse *response);

#endif
