#ifndef DB_SERVER_H
#define DB_SERVER_H

#include "sql.h"

typedef struct DBServer {
    Table *table;
} DBServer;

typedef struct DBServerExecution {
    SQLResult result;
    int used_index;
} DBServerExecution;

int db_server_init(DBServer *server);
void db_server_destroy(DBServer *server);

int db_server_execute(DBServer *server, const char *query, DBServerExecution *execution);
void db_server_execution_destroy(DBServerExecution *execution);

#endif
