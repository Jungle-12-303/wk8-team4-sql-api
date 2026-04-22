#include "sql.h"

#include <stdio.h>
#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Builds a zero-initialized SQL result with the requested defaults. */
static SQLResult sql_make_result(SQLStatus status, SQLAction action) {
    SQLResult result;

    result.status = status;
    result.action = action;
    result.record = NULL;
    result.records = NULL;
    result.inserted_id = 0;
    result.row_count = 0;
    result.error_code = 0;
    result.sql_state[0] = '\0';
    result.error_message[0] = '\0';
    return result;
}

static int sql_is_known_column(const char *column) {
    return strcasecmp(column, "id") == 0 ||
           strcasecmp(column, "name") == 0 ||
           strcasecmp(column, "age") == 0;
}

static void sql_build_near_excerpt(const char *cursor, char *buffer, size_t buffer_size) {
    size_t length = 0;

    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }

    if (*cursor == '\0') {
        snprintf(buffer, buffer_size, "end of input");
        return;
    }

    while (cursor[length] != '\0' &&
           cursor[length] != '\n' &&
           cursor[length] != '\r' &&
           cursor[length] != ';' &&
           length + 1 < buffer_size) {
        buffer[length] = cursor[length];
        length++;
    }

    while (length > 0 && isspace((unsigned char)buffer[length - 1])) {
        length--;
    }

    buffer[length] = '\0';

    if (length == 0) {
        snprintf(buffer, buffer_size, "end of input");
    }
}

static void sql_set_syntax_error(SQLResult *result, const char *cursor) {
    char near_excerpt[64];

    sql_build_near_excerpt(cursor, near_excerpt, sizeof(near_excerpt));
    result->status = SQL_STATUS_SYNTAX_ERROR;
    result->error_code = 1064;
    snprintf(result->sql_state, sizeof(result->sql_state), "42000");
    snprintf(
        result->error_message,
        sizeof(result->error_message),
        "ERROR 1064 (42000): You have an error in your SQL syntax; check the manual that corresponds to your sql processor2 version for the right syntax to use near '%s' at line 1",
        near_excerpt
    );
}

static void sql_set_unknown_column_error(SQLResult *result, const char *column, const char *clause_name) {
    result->status = SQL_STATUS_QUERY_ERROR;
    result->error_code = 1054;
    snprintf(result->sql_state, sizeof(result->sql_state), "42S22");
    snprintf(
        result->error_message,
        sizeof(result->error_message),
        "ERROR 1054 (42S22): Unknown column '%s' in '%s'",
        column,
        clause_name
    );
}

/* Skips whitespace characters in the parser cursor. */
static void sql_skip_spaces(const char **cursor) {
    while (**cursor != '\0' && isspace((unsigned char)**cursor)) {
        (*cursor)++;
    }
}

/* Matches a keyword case-insensitively and requires a token boundary. */
static int sql_match_keyword(const char **cursor, const char *keyword) {
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

/* Matches a single expected punctuation character. */
static int sql_match_char(const char **cursor, char expected) {
    sql_skip_spaces(cursor);

    if (**cursor != expected) {
        return 0;
    }

    (*cursor)++;
    return 1;
}

/* Parses an identifier such as users, id, name, or age. */
static int sql_parse_identifier(const char **cursor, char *buffer, size_t buffer_size) {
    size_t length = 0;

    sql_skip_spaces(cursor);

    if (!(isalpha((unsigned char)**cursor) || **cursor == '_')) {
        return 0;
    }

    while ((isalnum((unsigned char)**cursor) || **cursor == '_') && length + 1 < buffer_size) {
        buffer[length++] = **cursor;
        (*cursor)++;
    }

    if (length == 0) {
        return 0;
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

/* Parses a single-quoted string literal. */
static int sql_parse_string(const char **cursor, char *buffer, size_t buffer_size) {
    size_t length = 0;

    sql_skip_spaces(cursor);

    if (**cursor != '\'') {
        return 0;
    }

    (*cursor)++;

    while (**cursor != '\0' && **cursor != '\'') {
        if (length + 1 >= buffer_size) {
            return 0;
        }
        buffer[length++] = **cursor;
        (*cursor)++;
    }

    if (**cursor != '\'') {
        return 0;
    }

    (*cursor)++;
    buffer[length] = '\0';
    return 1;
}

/* Parses a signed integer literal. */
static int sql_parse_int(const char **cursor, int *value) {
    char *end_ptr;
    long parsed;

    sql_skip_spaces(cursor);

    if (!(isdigit((unsigned char)**cursor) || **cursor == '-')) {
        return 0;
    }

    parsed = strtol(*cursor, &end_ptr, 10);

    if (end_ptr == *cursor) {
        return 0;
    }

    *value = (int)parsed;
    *cursor = end_ptr;
    return 1;
}

/* Parses one of =, <, <=, >, >= into a comparison enum. */
static int sql_parse_comparison(const char **cursor, TableComparison *comparison) {
    sql_skip_spaces(cursor);

    if ((*cursor)[0] == '>' && (*cursor)[1] == '=') {
        *comparison = TABLE_COMPARISON_GE;
        *cursor += 2;
        return 1;
    }

    if ((*cursor)[0] == '<' && (*cursor)[1] == '=') {
        *comparison = TABLE_COMPARISON_LE;
        *cursor += 2;
        return 1;
    }

    if (**cursor == '>') {
        *comparison = TABLE_COMPARISON_GT;
        (*cursor)++;
        return 1;
    }

    if (**cursor == '<') {
        *comparison = TABLE_COMPARISON_LT;
        (*cursor)++;
        return 1;
    }

    if (**cursor == '=') {
        *comparison = TABLE_COMPARISON_EQ;
        (*cursor)++;
        return 1;
    }

    return 0;
}

/* Accepts an optional semicolon and trailing whitespace at the end. */
static int sql_match_statement_end(const char **cursor) {
    sql_skip_spaces(cursor);

    if (**cursor == ';') {
        (*cursor)++;
    }

    sql_skip_spaces(cursor);
    return **cursor == '\0';
}

/* Parses and executes the fixed INSERT syntax. */
static SQLResult sql_execute_insert(Table *table, const char *input) {
    SQLResult result = sql_make_result(SQL_STATUS_SYNTAX_ERROR, SQL_ACTION_NONE);
    const char *cursor = input;
    char table_name[32];
    char name[RECORD_NAME_SIZE];
    int age;
    Record *record;

    if (!sql_match_keyword(&cursor, "INSERT")) {
        return result;
    }

    sql_skip_spaces(&cursor);
    if (!sql_match_keyword(&cursor, "INTO")) {
        sql_set_syntax_error(&result, cursor);
        return result;
    }

    if (!sql_parse_identifier(&cursor, table_name, sizeof(table_name))) {
        sql_set_syntax_error(&result, cursor);
        return result;
    }

    if (strcasecmp(table_name, "users") != 0) {
        sql_set_syntax_error(&result, table_name);
        return result;
    }

    sql_skip_spaces(&cursor);
    if (!sql_match_keyword(&cursor, "VALUES")) {
        sql_set_syntax_error(&result, cursor);
        return result;
    }

    if (!sql_match_char(&cursor, '(')) {
        sql_set_syntax_error(&result, cursor);
        return result;
    }

    if (!sql_parse_string(&cursor, name, sizeof(name))) {
        sql_set_syntax_error(&result, cursor);
        return result;
    }

    if (!sql_match_char(&cursor, ',')) {
        sql_set_syntax_error(&result, cursor);
        return result;
    }

    if (!sql_parse_int(&cursor, &age)) {
        sql_set_syntax_error(&result, cursor);
        return result;
    }

    if (!sql_match_char(&cursor, ')')) {
        sql_set_syntax_error(&result, cursor);
        return result;
    }

    if (!sql_match_statement_end(&cursor)) {
        sql_set_syntax_error(&result, cursor);
        return result;
    }

    record = table_insert(table, name, age);

    if (record == NULL) {
        result.status = SQL_STATUS_ERROR;
        return result;
    }

    result.status = SQL_STATUS_OK;
    result.action = SQL_ACTION_INSERT;
    result.record = record;
    result.inserted_id = record->id;
    result.row_count = 1;
    return result;
}

/* Parses and executes the fixed SELECT syntax. */
static SQLResult sql_execute_select(Table *table, const char *input) {
    SQLResult result = sql_make_result(SQL_STATUS_SYNTAX_ERROR, SQL_ACTION_NONE);
    const char *cursor = input;
    char table_name[32];
    char column[32];
    char selected_column[32];
    char text_value[RECORD_NAME_SIZE];
    int int_value;
    TableComparison comparison;
    const char *comparison_cursor;
    const char *select_list_cursor;

    if (!sql_match_keyword(&cursor, "SELECT")) {
        return result;
    }

    sql_skip_spaces(&cursor);
    select_list_cursor = cursor;
    if (!sql_match_char(&cursor, '*')) {
        if (!sql_parse_identifier(&cursor, selected_column, sizeof(selected_column))) {
            sql_set_syntax_error(&result, cursor);
            return result;
        }

        if (!sql_is_known_column(selected_column)) {
            sql_set_unknown_column_error(&result, selected_column, "field list");
            return result;
        }

        sql_set_syntax_error(&result, select_list_cursor);
        return result;
    }

    sql_skip_spaces(&cursor);
    if (!sql_match_keyword(&cursor, "FROM")) {
        sql_set_syntax_error(&result, cursor);
        return result;
    }

    if (!sql_parse_identifier(&cursor, table_name, sizeof(table_name))) {
        sql_set_syntax_error(&result, cursor);
        return result;
    }

    if (strcasecmp(table_name, "users") != 0) {
        sql_set_syntax_error(&result, table_name);
        return result;
    }

    sql_skip_spaces(&cursor);
    if (sql_match_statement_end(&cursor)) {
        if (!table_collect_all(table, &result.records, &result.row_count)) {
            result.status = SQL_STATUS_ERROR;
            return result;
        }

        result.status = SQL_STATUS_OK;
        result.action = SQL_ACTION_SELECT_ROWS;
        result.record = (result.row_count > 0) ? result.records[0] : NULL;
        return result;
    }

    if (!sql_match_keyword(&cursor, "WHERE")) {
        sql_set_syntax_error(&result, cursor);
        return result;
    }

    if (!sql_parse_identifier(&cursor, column, sizeof(column))) {
        sql_set_syntax_error(&result, cursor);
        return result;
    }

    comparison_cursor = cursor;
    if (!sql_parse_comparison(&cursor, &comparison)) {
        sql_set_syntax_error(&result, cursor);
        return result;
    }

    if (strcasecmp(column, "id") == 0) {
        if (!sql_parse_int(&cursor, &int_value) || !sql_match_statement_end(&cursor)) {
            sql_set_syntax_error(&result, cursor);
            return result;
        }

        if (!table_find_by_id_condition(table, comparison, int_value, &result.records, &result.row_count)) {
            result.status = SQL_STATUS_ERROR;
            return result;
        }
    } else if (strcasecmp(column, "name") == 0) {
        if (comparison != TABLE_COMPARISON_EQ) {
            sql_set_syntax_error(&result, comparison_cursor);
            return result;
        }

        if (!sql_parse_string(&cursor, text_value, sizeof(text_value)) || !sql_match_statement_end(&cursor)) {
            sql_set_syntax_error(&result, cursor);
            return result;
        }

        if (!table_find_by_name_matches(table, text_value, &result.records, &result.row_count)) {
            result.status = SQL_STATUS_ERROR;
            return result;
        }
    } else if (strcasecmp(column, "age") == 0) {
        if (!sql_parse_int(&cursor, &int_value) || !sql_match_statement_end(&cursor)) {
            sql_set_syntax_error(&result, cursor);
            return result;
        }

        if (!table_find_by_age_condition(table, comparison, int_value, &result.records, &result.row_count)) {
            result.status = SQL_STATUS_ERROR;
            return result;
        }
    } else {
        sql_set_unknown_column_error(&result, column, "where clause");
        return result;
    }

    result.record = (result.row_count > 0) ? result.records[0] : NULL;
    result.status = (result.row_count == 0) ? SQL_STATUS_NOT_FOUND : SQL_STATUS_OK;
    result.action = SQL_ACTION_SELECT_ROWS;
    return result;
}

/* Parses one SQL statement and executes it against the table. */
SQLResult sql_execute(Table *table, const char *input) {
    SQLResult result = sql_make_result(SQL_STATUS_SYNTAX_ERROR, SQL_ACTION_NONE);
    const char *cursor;

    if (table == NULL || input == NULL) {
        result.status = SQL_STATUS_ERROR;
        return result;
    }

    cursor = input;
    sql_skip_spaces(&cursor);

    if (sql_match_keyword(&cursor, "EXIT") || sql_match_keyword(&cursor, "QUIT")) {
        if (sql_match_statement_end(&cursor)) {
            result.status = SQL_STATUS_EXIT;
        } else {
            sql_set_syntax_error(&result, cursor);
        }
        return result;
    }

    result = sql_execute_insert(table, input);
    if (result.status != SQL_STATUS_SYNTAX_ERROR || result.error_message[0] != '\0') {
        return result;
    }

    result = sql_execute_select(table, input);
    if (result.status == SQL_STATUS_SYNTAX_ERROR && result.error_message[0] == '\0') {
        sql_set_syntax_error(&result, cursor);
    }
    return result;
}

/* Releases any heap memory owned by a SQL result. */
void sql_result_destroy(SQLResult *result) {
    if (result == NULL) {
        return;
    }

    free(result->records);
    result->records = NULL;
    result->record = NULL;
    result->inserted_id = 0;
    result->row_count = 0;
    result->action = SQL_ACTION_NONE;
    result->status = SQL_STATUS_OK;
    result->error_code = 0;
    result->sql_state[0] = '\0';
    result->error_message[0] = '\0';
}
