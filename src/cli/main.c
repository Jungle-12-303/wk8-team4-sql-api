#include "sql.h"
#include "table.h"

#include <stdio.h>

#define INPUT_BUFFER_SIZE 256

/* Runs a small REPL that accepts the MVP SQL statements. */
int main(void) {
    Table *table = table_create();
    char input[INPUT_BUFFER_SIZE];

    if (table == NULL) {
        fprintf(stderr, "Failed to create table.\n");
        return 1;
    }

    printf("Simple SQL Processor with B+ Tree Index\n");
    printf("Supported statements:\n");
    printf("  INSERT INTO users VALUES ('Alice', 20);\n");
    printf("  SELECT * FROM users;\n");
    printf("  SELECT * FROM users WHERE id = 1;\n");
    printf("  SELECT * FROM users WHERE id >= 10;\n");
    printf("  SELECT * FROM users WHERE name = 'Alice';\n");
    printf("  SELECT * FROM users WHERE age = 20;\n");
    printf("  SELECT * FROM users WHERE age > 20;\n");
    printf("Type EXIT or QUIT to leave.\n");

    while (1) {
        SQLResult result;

        printf("sql> ");
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }

        result = sql_execute(table, input);

        if (result.status == SQL_STATUS_EXIT) {
            sql_result_destroy(&result);
            break;
        }

        if (result.status == SQL_STATUS_OK) {
            if (result.action == SQL_ACTION_INSERT) {
                printf("Inserted row with id = %d\n", result.inserted_id);
            } else if (result.action == SQL_ACTION_SELECT_ROWS) {
                if (table_print_records(result.records, result.row_count) == 0) {
                    printf("No rows\n");
                }
            } else {
                printf("Done\n");
            }
        } else if (result.status == SQL_STATUS_NOT_FOUND) {
            printf("Not found\n");
        } else if (result.error_message[0] != '\0') {
            printf("%s\n", result.error_message);
        } else if (result.status == SQL_STATUS_SYNTAX_ERROR) {
            printf("Syntax error\n");
        } else {
            printf("Execution error\n");
        }

        sql_result_destroy(&result);
    }

    table_destroy(table);
    return 0;
}
