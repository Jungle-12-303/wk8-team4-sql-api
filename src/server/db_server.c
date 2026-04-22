#include "db_server.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef enum DBServerQueryKind {
    DB_SERVER_QUERY_KIND_NONE,
    DB_SERVER_QUERY_KIND_READ,
    DB_SERVER_QUERY_KIND_WRITE
} DBServerQueryKind;

static void db_server_zero_execution(DBServerExecution *execution) {
    memset(execution, 0, sizeof(*execution));
    execution->result.status = SQL_STATUS_ERROR;
    execution->result.action = SQL_ACTION_NONE;
    execution->server_status = DB_SERVER_EXEC_STATUS_ERROR;
    snprintf(execution->message, sizeof(execution->message), "Execution not started");
}

static void db_server_skip_spaces(const char **cursor) {
    while (**cursor != '\0' && isspace((unsigned char)**cursor)) {
        (*cursor)++;
    }
}

static int db_server_match_keyword(const char **cursor, const char *keyword) {
    const char *start = *cursor;
    const char *word = keyword;

    while (*word != '\0') {
        if (tolower((unsigned char)*start) != tolower((unsigned char)*word)) {
            return 0;
        }
        start++;
        word++;
    }

    if (isalnum((unsigned char)*start) || *start == '_') {
        return 0;
    }

    *cursor = start;
    return 1;
}

static int db_server_parse_identifier(const char **cursor, char *buffer, size_t buffer_size) {
    size_t length = 0;

    db_server_skip_spaces(cursor);

    if (!(isalpha((unsigned char)**cursor) || **cursor == '_')) {
        return 0;
    }

    while ((isalnum((unsigned char)**cursor) || **cursor == '_') && length + 1 < buffer_size) {
        buffer[length++] = **cursor;
        (*cursor)++;
    }

    while (isalnum((unsigned char)**cursor) || **cursor == '_') {
        (*cursor)++;
        length++;
    }

    if (length >= buffer_size) {
        return 0;
    }

    buffer[length] = '\0';
    return 1;
}

static int db_server_identifier_equals(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return 0;
        }
        left++;
        right++;
    }

    return *left == '\0' && *right == '\0';
}

static DBServerQueryKind db_server_classify_query(const char *query) {
    const char *cursor = query;

    if (query == NULL) {
        return DB_SERVER_QUERY_KIND_NONE;
    }

    db_server_skip_spaces(&cursor);
    if (db_server_match_keyword(&cursor, "SELECT")) {
        return DB_SERVER_QUERY_KIND_READ;
    }

    cursor = query;
    db_server_skip_spaces(&cursor);
    if (db_server_match_keyword(&cursor, "INSERT")) {
        return DB_SERVER_QUERY_KIND_WRITE;
    }

    return DB_SERVER_QUERY_KIND_NONE;
}

static int db_server_guess_uses_index(const char *query) {
    const char *cursor = query;
    char column[32];

    if (query == NULL) {
        return 0;
    }

    db_server_skip_spaces(&cursor);
    if (!db_server_match_keyword(&cursor, "SELECT")) {
        return 0;
    }

    while (*cursor != '\0') {
        if (db_server_match_keyword(&cursor, "WHERE")) {
            if (db_server_parse_identifier(&cursor, column, sizeof(column)) &&
                db_server_identifier_equals(column, "id")) {
                return 1;
            }
            return 0;
        }
        cursor++;
    }

    return 0;
}

static int db_server_try_acquire_lock(DBServer *server, DBServerQueryKind query_kind) {
    unsigned long long start_time = platform_now_millis();

    if (query_kind == DB_SERVER_QUERY_KIND_NONE) {
        return 1;
    }

    while (1) {
        if (query_kind == DB_SERVER_QUERY_KIND_WRITE) {
            if (platform_rwlock_try_write_lock(&server->db_lock)) {
                return 1;
            }
        } else {
            if (platform_rwlock_try_read_lock(&server->db_lock)) {
                return 1;
            }
        }

        if (server->config.lock_timeout_ms > 0 &&
            platform_now_millis() - start_time >= server->config.lock_timeout_ms) {
            return 0;
        }

        platform_sleep_ms(1);
    }
}

static void db_server_release_lock(DBServer *server, DBServerQueryKind query_kind) {
    if (query_kind == DB_SERVER_QUERY_KIND_WRITE) {
        platform_rwlock_write_unlock(&server->db_lock);
    } else if (query_kind == DB_SERVER_QUERY_KIND_READ) {
        platform_rwlock_read_unlock(&server->db_lock);
    }
}

static void db_server_metrics_query_started(DBServer *server, DBServerQueryKind query_kind) {
    platform_mutex_lock(&server->metrics_mutex);
    server->metrics.total_requests++;
    server->metrics.total_query_requests++;
    server->metrics.active_query_requests++;

    if (query_kind == DB_SERVER_QUERY_KIND_WRITE) {
        server->metrics.total_insert_requests++;
    } else if (query_kind == DB_SERVER_QUERY_KIND_READ) {
        server->metrics.total_select_requests++;
    }

    platform_mutex_unlock(&server->metrics_mutex);
}

static void db_server_metrics_query_finished(DBServer *server, const DBServerExecution *execution) {
    platform_mutex_lock(&server->metrics_mutex);

    if (server->metrics.active_query_requests > 0) {
        server->metrics.active_query_requests--;
    }

    if (execution->server_status == DB_SERVER_EXEC_STATUS_LOCK_TIMEOUT) {
        server->metrics.total_errors++;
        server->metrics.total_lock_timeouts++;
    } else if (execution->result.status == SQL_STATUS_NOT_FOUND) {
        server->metrics.total_not_found_results++;
    } else if (execution->result.status == SQL_STATUS_SYNTAX_ERROR) {
        server->metrics.total_errors++;
        server->metrics.total_syntax_errors++;
    } else if (execution->result.status == SQL_STATUS_QUERY_ERROR ||
               execution->result.status == SQL_STATUS_EXIT) {
        server->metrics.total_errors++;
        server->metrics.total_query_errors++;
    } else if (execution->result.status == SQL_STATUS_ERROR) {
        server->metrics.total_errors++;
        server->metrics.total_internal_errors++;
    }

    platform_mutex_unlock(&server->metrics_mutex);
}

static void db_server_apply_simulated_delay(const DBServer *server, DBServerQueryKind query_kind) {
    if (query_kind == DB_SERVER_QUERY_KIND_WRITE && server->config.simulate_write_delay_ms > 0) {
        platform_sleep_ms(server->config.simulate_write_delay_ms);
    } else if (query_kind == DB_SERVER_QUERY_KIND_READ && server->config.simulate_read_delay_ms > 0) {
        platform_sleep_ms(server->config.simulate_read_delay_ms);
    }
}

void db_server_config_default(DBServerConfig *config) {
    if (config == NULL) {
        return;
    }

    config->lock_timeout_ms = 1000;
    config->simulate_read_delay_ms = 0;
    config->simulate_write_delay_ms = 0;
}

int db_server_init(DBServer *server) {
    DBServerConfig default_config;

    db_server_config_default(&default_config);
    return db_server_init_with_config(server, &default_config);
}

int db_server_init_with_config(DBServer *server, const DBServerConfig *config) {
    DBServerConfig default_config;

    if (server == NULL) {
        return 0;
    }

    db_server_config_default(&default_config);
    memset(server, 0, sizeof(*server));
    server->config = (config != NULL) ? *config : default_config;

    if (!platform_rwlock_init(&server->db_lock)) {
        return 0;
    }

    if (!platform_mutex_init(&server->metrics_mutex)) {
        platform_rwlock_destroy(&server->db_lock);
        return 0;
    }

    server->table = table_create();
    if (server->table == NULL) {
        platform_mutex_destroy(&server->metrics_mutex);
        platform_rwlock_destroy(&server->db_lock);
        return 0;
    }

    return 1;
}

void db_server_destroy(DBServer *server) {
    if (server == NULL) {
        return;
    }

    if (server->table != NULL) {
        table_destroy(server->table);
        server->table = NULL;
    }

    platform_mutex_destroy(&server->metrics_mutex);
    platform_rwlock_destroy(&server->db_lock);
}

int db_server_execute(DBServer *server, const char *query, DBServerExecution *execution) {
    DBServerQueryKind query_kind;

    if (server == NULL || server->table == NULL || query == NULL || execution == NULL) {
        return 0;
    }

    db_server_zero_execution(execution);
    query_kind = db_server_classify_query(query);
    execution->used_index = db_server_guess_uses_index(query);
    execution->is_write = (query_kind == DB_SERVER_QUERY_KIND_WRITE);

    db_server_metrics_query_started(server, query_kind);

    if (!db_server_try_acquire_lock(server, query_kind)) {
        execution->server_status = DB_SERVER_EXEC_STATUS_LOCK_TIMEOUT;
        execution->result.status = SQL_STATUS_ERROR;
        execution->result.action = SQL_ACTION_NONE;
        snprintf(execution->message, sizeof(execution->message), "Lock wait timeout exceeded");
        db_server_metrics_query_finished(server, execution);
        return 1;
    }

    db_server_apply_simulated_delay(server, query_kind);
    execution->result = sql_execute(server->table, query);
    execution->server_status = DB_SERVER_EXEC_STATUS_OK;
    snprintf(execution->message, sizeof(execution->message), "OK");

    db_server_release_lock(server, query_kind);
    db_server_metrics_query_finished(server, execution);
    return 1;
}

void db_server_execution_destroy(DBServerExecution *execution) {
    if (execution == NULL) {
        return;
    }

    sql_result_destroy(&execution->result);
    db_server_zero_execution(execution);
}

void db_server_get_metrics(DBServer *server, DBServerMetrics *metrics) {
    if (server == NULL || metrics == NULL) {
        return;
    }

    platform_mutex_lock(&server->metrics_mutex);
    *metrics = server->metrics;
    platform_mutex_unlock(&server->metrics_mutex);
}

void db_server_record_health_request(DBServer *server) {
    if (server == NULL) {
        return;
    }

    platform_mutex_lock(&server->metrics_mutex);
    server->metrics.total_requests++;
    server->metrics.total_health_requests++;
    platform_mutex_unlock(&server->metrics_mutex);
}

void db_server_record_metrics_request(DBServer *server) {
    if (server == NULL) {
        return;
    }

    platform_mutex_lock(&server->metrics_mutex);
    server->metrics.total_requests++;
    server->metrics.total_metrics_requests++;
    platform_mutex_unlock(&server->metrics_mutex);
}

void db_server_record_queue_full(DBServer *server) {
    if (server == NULL) {
        return;
    }

    platform_mutex_lock(&server->metrics_mutex);
    server->metrics.total_requests++;
    server->metrics.total_errors++;
    server->metrics.total_queue_full++;
    platform_mutex_unlock(&server->metrics_mutex);
}
