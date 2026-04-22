#ifndef SQL_H
#define SQL_H

#include "table.h"
#include <stddef.h>

#define SQL_SQLSTATE_SIZE 6
#define SQL_ERROR_MESSAGE_SIZE 256

typedef enum SQLStatus {
    SQL_STATUS_OK,
    SQL_STATUS_NOT_FOUND,
    SQL_STATUS_SYNTAX_ERROR,
    SQL_STATUS_QUERY_ERROR,
    SQL_STATUS_EXIT,
    SQL_STATUS_ERROR
} SQLStatus;

typedef enum SQLAction {
    SQL_ACTION_NONE,
    SQL_ACTION_INSERT,
    SQL_ACTION_SELECT_ROWS
} SQLAction;

typedef struct SQLResult {
    SQLStatus status;
    SQLAction action;
    Record *record;
    Record **records;
    int inserted_id;
    size_t row_count;
    int error_code;
    char sql_state[SQL_SQLSTATE_SIZE];
    char error_message[SQL_ERROR_MESSAGE_SIZE];
} SQLResult;

/* Parses one SQL statement and executes it against the table. */
SQLResult sql_execute(Table *table, const char *input);

/* Releases any heap memory owned by a SQL result. */
void sql_result_destroy(SQLResult *result);

#endif
