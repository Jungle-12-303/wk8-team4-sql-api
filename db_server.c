#include "db_server.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static void db_server_zero_execution(DBServerExecution *execution) {
    memset(execution, 0, sizeof(*execution));
    execution->result.status = SQL_STATUS_ERROR;
    execution->result.action = SQL_ACTION_NONE;
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

int db_server_init(DBServer *server) {
    if (server == NULL) {
        return 0;
    }

    memset(server, 0, sizeof(*server));
    server->table = table_create();
    return server->table != NULL;
}

void db_server_destroy(DBServer *server) {
    if (server == NULL || server->table == NULL) {
        return;
    }

    table_destroy(server->table);
    server->table = NULL;
}

int db_server_execute(DBServer *server, const char *query, DBServerExecution *execution) {
    if (server == NULL || server->table == NULL || query == NULL || execution == NULL) {
        return 0;
    }

    db_server_zero_execution(execution);
    execution->used_index = db_server_guess_uses_index(query);
    execution->result = sql_execute(server->table, query);
    return 1;
}

void db_server_execution_destroy(DBServerExecution *execution) {
    if (execution == NULL) {
        return;
    }

    sql_result_destroy(&execution->result);
    db_server_zero_execution(execution);
}
