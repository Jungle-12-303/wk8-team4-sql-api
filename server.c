#include "db_server.h"
#include "http_server.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void server_print_usage(const char *program_name) {
    printf("Usage:\n");
    printf("  %s --query \"SQL\" [--query \"SQL\" ...]\n", program_name);
    printf("  %s --serve [--port 8080] [--workers 4] [--queue 16]\n", program_name);
    printf("  %s\n", program_name);
    printf("\n");
    printf("CLI mode reads one SQL statement per line from stdin when --query is omitted.\n");
    printf("HTTP mode enables GET /health, GET /metrics, and POST /query.\n");
}

static int server_parse_unsigned(const char *value, unsigned long *parsed) {
    char *end_ptr;

    *parsed = strtoul(value, &end_ptr, 10);
    return end_ptr != value && *end_ptr == '\0';
}

static int server_print_execution(const DBServerExecution *execution) {
    size_t index;

    if (execution->server_status == DB_SERVER_EXEC_STATUS_LOCK_TIMEOUT) {
        printf("ERROR type=lock_timeout message=%s\n", execution->message);
        return 1;
    }

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
    DBServerConfig cli_config;
    HTTPServerOptions http_options;
    int serve_mode = 0;
    int has_query_argument = 0;
    int index;

    db_server_config_default(&cli_config);
    http_server_options_default(&http_options);

    for (index = 1; index < argc; index++) {
        unsigned long parsed = 0;

        if (strcmp(argv[index], "--help") == 0) {
            server_print_usage(argv[0]);
            return 0;
        }

        if (strcmp(argv[index], "--serve") == 0) {
            serve_mode = 1;
            continue;
        }

        if (strcmp(argv[index], "--query") == 0) {
            index++;
            if (index >= argc) {
                fprintf(stderr, "--query requires a SQL string.\n");
                return 1;
            }
            has_query_argument = 1;
            continue;
        }

        if (strcmp(argv[index], "--port") == 0) {
            index++;
            if (index >= argc || !server_parse_unsigned(argv[index], &parsed) || parsed > 65535UL) {
                fprintf(stderr, "--port requires a valid number between 0 and 65535.\n");
                return 1;
            }
            http_options.port = (unsigned short)parsed;
            continue;
        }

        if (strcmp(argv[index], "--workers") == 0) {
            index++;
            if (index >= argc || !server_parse_unsigned(argv[index], &parsed) || parsed == 0UL) {
                fprintf(stderr, "--workers requires a positive integer.\n");
                return 1;
            }
            http_options.worker_count = (size_t)parsed;
            continue;
        }

        if (strcmp(argv[index], "--queue") == 0) {
            index++;
            if (index >= argc || !server_parse_unsigned(argv[index], &parsed) || parsed == 0UL) {
                fprintf(stderr, "--queue requires a positive integer.\n");
                return 1;
            }
            http_options.queue_capacity = (size_t)parsed;
            continue;
        }

        if (strcmp(argv[index], "--lock-timeout-ms") == 0) {
            index++;
            if (index >= argc || !server_parse_unsigned(argv[index], &parsed)) {
                fprintf(stderr, "--lock-timeout-ms requires a non-negative integer.\n");
                return 1;
            }
            cli_config.lock_timeout_ms = (unsigned int)parsed;
            http_options.lock_timeout_ms = (unsigned int)parsed;
            continue;
        }

        if (strcmp(argv[index], "--simulate-read-delay-ms") == 0) {
            index++;
            if (index >= argc || !server_parse_unsigned(argv[index], &parsed)) {
                fprintf(stderr, "--simulate-read-delay-ms requires a non-negative integer.\n");
                return 1;
            }
            cli_config.simulate_read_delay_ms = (unsigned int)parsed;
            http_options.simulate_read_delay_ms = (unsigned int)parsed;
            continue;
        }

        if (strcmp(argv[index], "--simulate-write-delay-ms") == 0) {
            index++;
            if (index >= argc || !server_parse_unsigned(argv[index], &parsed)) {
                fprintf(stderr, "--simulate-write-delay-ms requires a non-negative integer.\n");
                return 1;
            }
            cli_config.simulate_write_delay_ms = (unsigned int)parsed;
            http_options.simulate_write_delay_ms = (unsigned int)parsed;
            continue;
        }

        if (strcmp(argv[index], "--max-requests") == 0) {
            index++;
            if (index >= argc || !server_parse_unsigned(argv[index], &parsed)) {
                fprintf(stderr, "--max-requests requires a non-negative integer.\n");
                return 1;
            }
            http_options.max_requests = (unsigned int)parsed;
            continue;
        }

        fprintf(stderr, "Unknown argument: %s\n", argv[index]);
        server_print_usage(argv[0]);
        return 1;
    }

    if (serve_mode && has_query_argument) {
        fprintf(stderr, "--serve cannot be combined with --query.\n");
        return 1;
    }

    if (serve_mode) {
        return http_server_run(&http_options);
    }

    if (!db_server_init_with_config(&server, &cli_config)) {
        fprintf(stderr, "Failed to create shared table for server bootstrap.\n");
        return 1;
    }

    if (argc == 1 || !has_query_argument) {
        index = server_run_stdin(&server) ? 0 : 1;
        db_server_destroy(&server);
        return index;
    }

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--query") == 0) {
            index++;
            if (!server_run_query(&server, argv[index])) {
                db_server_destroy(&server);
                return 0;
            }
        }
    }

    db_server_destroy(&server);
    return 0;
}
