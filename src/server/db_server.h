#ifndef DB_SERVER_H
#define DB_SERVER_H

#include "platform.h"
#include "sql.h"

typedef enum DBServerExecStatus {
    DB_SERVER_EXEC_STATUS_OK,
    DB_SERVER_EXEC_STATUS_LOCK_TIMEOUT,
    DB_SERVER_EXEC_STATUS_ERROR
} DBServerExecStatus;

typedef struct DBServerConfig {
    unsigned int lock_timeout_ms;
    unsigned int simulate_read_delay_ms;
    unsigned int simulate_write_delay_ms;
} DBServerConfig;

typedef struct DBServerMetrics {
    unsigned long long total_requests;
    unsigned long long total_health_requests;
    unsigned long long total_metrics_requests;
    unsigned long long total_query_requests;
    unsigned long long total_select_requests;
    unsigned long long total_insert_requests;
    unsigned long long total_errors;
    unsigned long long total_syntax_errors;
    unsigned long long total_query_errors;
    unsigned long long total_internal_errors;
    unsigned long long total_not_found_results;
    unsigned long long total_queue_full;
    unsigned long long total_lock_timeouts;
    unsigned long long active_query_requests;
} DBServerMetrics;

typedef struct DBServer {
    Table *table;
    PlatformRWLock db_lock;
    PlatformMutex metrics_mutex;
    DBServerConfig config;
    DBServerMetrics metrics;
} DBServer;

typedef struct DBServerExecution {
    SQLResult result;
    int used_index;
    int is_write;
    DBServerExecStatus server_status;
    char message[128];
} DBServerExecution;

void db_server_config_default(DBServerConfig *config);

int db_server_init(DBServer *server);
int db_server_init_with_config(DBServer *server, const DBServerConfig *config);
void db_server_destroy(DBServer *server);

int db_server_execute(DBServer *server, const char *query, DBServerExecution *execution);
void db_server_execution_destroy(DBServerExecution *execution);

void db_server_get_metrics(DBServer *server, DBServerMetrics *metrics);
void db_server_record_health_request(DBServer *server);
void db_server_record_metrics_request(DBServer *server);
void db_server_record_queue_full(DBServer *server);

#endif
