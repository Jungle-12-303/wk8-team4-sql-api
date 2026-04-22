#include "db_server.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void server_print_usage(const char *program_name) {
    printf("Usage:\n");
    printf("  %s --query \"SQL\" [--query \"SQL\" ...]\n", program_name);
    printf("  %s\n", program_name);
    printf("\n");
    printf("Without --query, the server reads one SQL statement per line from stdin.\n");
}

static int server_print_execution(const DBServerExecution *execution) {
    size_t index;

    if (execution->result.status == SQL_STATUS_OK) {
        if (execution->result.action == SQL_ACTION_INSERT) {
            printf("OK INSERT id=%d used_index=false\n", execution->result.inserted_id);
            return 1;
        }

        if (execution->result.action == SQL_ACTION_SELECT_ROWS) {
            printf("OK SELECT rows=%lu used_index=%s\n",
                   (unsigned long)execution->result.row_count,
                   execution->used_index ? "true" : "false");

            for (index = 0; index < execution->result.row_count; index++) {
                printf("ROW id=%d name=%s age=%d\n",
                       execution->result.records[index]->id,
                       execution->result.records[index]->name,
                       execution->result.records[index]->age);
            }
            return 1;
        }

        printf("OK DONE\n");
        return 1;
    }

    if (execution->result.status == SQL_STATUS_NOT_FOUND) {
        printf("OK SELECT rows=0 used_index=%s\n", execution->used_index ? "true" : "false");
        return 1;
    }

    if (execution->result.status == SQL_STATUS_EXIT) {
        printf("BYE\n");
        return 0;
    }

    if (execution->result.status == SQL_STATUS_SYNTAX_ERROR) {
        printf("ERROR type=syntax message=%s\n", execution->result.error_message);
    } else if (execution->result.status == SQL_STATUS_QUERY_ERROR) {
        printf("ERROR type=query message=%s\n", execution->result.error_message);
    } else {
        printf("ERROR type=internal message=%s\n",
               execution->result.error_message[0] != '\0' ? execution->result.error_message : "Execution error");
    }

    return 1;
}

static int server_run_query(DBServer *server, const char *query) {
    DBServerExecution execution;
    int should_continue;

    if (!db_server_execute(server, query, &execution)) {
        fprintf(stderr, "Failed to execute query.\n");
        return 0;
    }

    should_continue = server_print_execution(&execution);
    db_server_execution_destroy(&execution);
    return should_continue;
}

static int server_run_stdin(DBServer *server) {
    char input[512];

    printf("SQL server harness ready.\n");
    printf("Type EXIT or QUIT to stop.\n");

    while (fgets(input, sizeof(input), stdin) != NULL) {
        if (!server_run_query(server, input)) {
            return 1;
        }
    }

    return 1;
}

int main(int argc, char **argv) {
    DBServer server;
    int index;

    if (!db_server_init(&server)) {
        fprintf(stderr, "Failed to create shared table for server bootstrap.\n");
        return 1;
    }

    if (argc == 1) {
        index = server_run_stdin(&server) ? 0 : 1;
        db_server_destroy(&server);
        return index;
    }

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--help") == 0) {
            server_print_usage(argv[0]);
            db_server_destroy(&server);
            return 0;
        }

        if (strcmp(argv[index], "--query") == 0) {
            index++;
            if (index >= argc) {
                fprintf(stderr, "--query requires a SQL string.\n");
                db_server_destroy(&server);
                return 1;
            }

            if (!server_run_query(&server, argv[index])) {
                db_server_destroy(&server);
                return 0;
            }
            continue;
        }

        fprintf(stderr, "Unknown argument: %s\n", argv[index]);
        server_print_usage(argv[0]);
        db_server_destroy(&server);
        return 1;
    }

    db_server_destroy(&server);
    return 0;
}
