#include "api.h"
#include "bptree.h"
#include "db_server.h"
#include "sql.h"
#include "table.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Verifies a SQL select result contains the expected record IDs in order. */
static void assert_result_ids(const SQLResult *result, const int *expected_ids, size_t expected_count) {
    size_t index;

    assert(result->action == SQL_ACTION_SELECT_ROWS);
    assert(result->row_count == expected_count);

    if (expected_count == 0) {
        assert(result->records == NULL);
        assert(result->record == NULL);
        return;
    }

    assert(result->records != NULL);
    assert(result->record == result->records[0]);

    for (index = 0; index < expected_count; index++) {
        assert(result->records[index]->id == expected_ids[index]);
    }
}

/* Verifies searching an empty tree returns NULL. */
static void test_empty_tree_search(void) {
    BPTree *tree = bptree_create();

    assert(tree != NULL);
    assert(bptree_search(tree, 10) == NULL);

    bptree_destroy(tree);
}

/* Verifies a single inserted key can be found again. */
static void test_single_insert_search(void) {
    BPTree *tree = bptree_create();
    int value = 123;

    assert(tree != NULL);
    assert(bptree_insert(tree, 1, &value) == 1);
    assert(*(int *)bptree_search(tree, 1) == 123);

    bptree_destroy(tree);
}

/* Verifies duplicate keys are rejected. */
static void test_duplicate_key_rejected(void) {
    BPTree *tree = bptree_create();
    int value1 = 100;
    int value2 = 200;

    assert(tree != NULL);
    assert(bptree_insert(tree, 7, &value1) == 1);
    assert(bptree_insert(tree, 7, &value2) == 0);
    assert(*(int *)bptree_search(tree, 7) == 100);

    bptree_destroy(tree);
}

/* Verifies a leaf split still preserves every inserted key. */
static void test_leaf_split_search(void) {
    BPTree *tree = bptree_create();
    int values[4] = {10, 20, 30, 40};
    int index;

    assert(tree != NULL);

    for (index = 0; index < 4; index++) {
        assert(bptree_insert(tree, index + 1, &values[index]) == 1);
    }

    assert(tree->root != NULL);
    assert(tree->root->is_leaf == 0);
    assert(tree->root->num_keys == 1);

    for (index = 0; index < 4; index++) {
        assert(*(int *)bptree_search(tree, index + 1) == values[index]);
    }

    bptree_destroy(tree);
}

/* Verifies internal splits can grow the tree height above one level. */
static void test_internal_split_new_root(void) {
    BPTree *tree = bptree_create();
    int values[10];
    int index;

    assert(tree != NULL);

    for (index = 0; index < 10; index++) {
        values[index] = index + 1;
        assert(bptree_insert(tree, index + 1, &values[index]) == 1);
    }

    assert(tree->root != NULL);
    assert(tree->root->is_leaf == 0);
    assert(tree->root->children[0] != NULL);
    assert(tree->root->children[0]->is_leaf == 0);

    for (index = 0; index < 10; index++) {
        assert(*(int *)bptree_search(tree, index + 1) == values[index]);
    }

    bptree_destroy(tree);
}

/* Verifies leaf siblings remain linked after a split. */
static void test_leaf_next_link(void) {
    BPTree *tree = bptree_create();
    int values[4] = {1, 2, 3, 4};
    BPTreeNode *left_leaf;
    BPTreeNode *right_leaf;

    assert(tree != NULL);
    assert(bptree_insert(tree, 1, &values[0]) == 1);
    assert(bptree_insert(tree, 2, &values[1]) == 1);
    assert(bptree_insert(tree, 3, &values[2]) == 1);
    assert(bptree_insert(tree, 4, &values[3]) == 1);

    left_leaf = tree->root->children[0];
    right_leaf = tree->root->children[1];

    assert(left_leaf != NULL);
    assert(right_leaf != NULL);
    assert(left_leaf->is_leaf == 1);
    assert(left_leaf->next == right_leaf);

    bptree_destroy(tree);
}

/* Verifies the table assigns monotonically increasing IDs. */
static void test_table_auto_increment(void) {
    Table *table = table_create();
    Record *first;
    Record *second;

    assert(table != NULL);

    first = table_insert(table, "Alice", 20);
    second = table_insert(table, "Bob", 21);

    assert(first != NULL);
    assert(second != NULL);
    assert(first->id == 1);
    assert(second->id == 2);

    table_destroy(table);
}

/* Verifies ID search uses the inserted indexed records. */
static void test_table_find_by_id(void) {
    Table *table = table_create();
    Record *alice;

    assert(table != NULL);

    alice = table_insert(table, "Alice", 20);
    assert(alice != NULL);
    assert(table_find_by_id(table, alice->id) == alice);
    assert(table_scan_by_id(table, alice->id) == alice);

    table_destroy(table);
}

/* Verifies non-ID lookups use simple linear scans. */
static void test_table_linear_search_fields(void) {
    Table *table = table_create();
    Record *alice;
    Record *bob;

    assert(table != NULL);

    alice = table_insert(table, "Alice", 20);
    bob = table_insert(table, "Bob", 30);

    assert(alice != NULL);
    assert(bob != NULL);
    assert(table_find_by_name(table, "Bob") == bob);
    assert(table_find_by_age(table, 20) == alice);

    table_destroy(table);
}

/* Verifies numeric WHERE conditions can collect multiple matches. */
static void test_table_condition_search(void) {
    Table *table = table_create();
    Record **matches = NULL;
    size_t count = 0;

    assert(table != NULL);
    assert(table_insert(table, "Alice", 10) != NULL);
    assert(table_insert(table, "Bob", 20) != NULL);
    assert(table_insert(table, "Carol", 20) != NULL);
    assert(table_insert(table, "Dave", 40) != NULL);

    assert(table_find_by_id_condition(table, TABLE_COMPARISON_GE, 3, &matches, &count) == 1);
    assert(count == 2);
    assert(matches[0]->id == 3);
    assert(matches[1]->id == 4);
    free(matches);
    matches = NULL;

    assert(table_find_by_id_condition(table, TABLE_COMPARISON_LT, 3, &matches, &count) == 1);
    assert(count == 2);
    assert(matches[0]->id == 1);
    assert(matches[1]->id == 2);
    free(matches);
    matches = NULL;

    assert(table_find_by_age_condition(table, TABLE_COMPARISON_EQ, 20, &matches, &count) == 1);
    assert(count == 2);
    assert(matches[0]->id == 2);
    assert(matches[1]->id == 3);
    free(matches);
    matches = NULL;

    assert(table_find_by_age_condition(table, TABLE_COMPARISON_GT, 15, &matches, &count) == 1);
    assert(count == 3);
    assert(matches[0]->id == 2);
    assert(matches[1]->id == 3);
    assert(matches[2]->id == 4);
    free(matches);

    table_destroy(table);
}

/* Verifies the SQL executor handles insert and select statements. */
static void test_sql_execution(void) {
    Table *table = table_create();
    SQLResult result;
    int expected_ids[4];

    assert(table != NULL);

    result = sql_execute(table, "INSERT INTO users VALUES ('Alice', 20);");
    assert(result.status == SQL_STATUS_OK);
    assert(result.action == SQL_ACTION_INSERT);
    assert(result.inserted_id == 1);
    sql_result_destroy(&result);

    result = sql_execute(table, "INSERT INTO users VALUES ('Bob', 30);");
    assert(result.status == SQL_STATUS_OK);
    assert(result.action == SQL_ACTION_INSERT);
    assert(result.inserted_id == 2);
    sql_result_destroy(&result);

    result = sql_execute(table, "INSERT INTO users VALUES ('Carol', 20);");
    assert(result.status == SQL_STATUS_OK);
    assert(result.action == SQL_ACTION_INSERT);
    assert(result.inserted_id == 3);
    sql_result_destroy(&result);

    result = sql_execute(table, "INSERT INTO users VALUES ('Dave', 40);");
    assert(result.status == SQL_STATUS_OK);
    assert(result.action == SQL_ACTION_INSERT);
    assert(result.inserted_id == 4);
    sql_result_destroy(&result);

    expected_ids[0] = 1;
    expected_ids[1] = 2;
    expected_ids[2] = 3;
    expected_ids[3] = 4;
    result = sql_execute(table, "SELECT * FROM users;");
    assert(result.status == SQL_STATUS_OK);
    assert_result_ids(&result, expected_ids, 4);
    sql_result_destroy(&result);

    result = sql_execute(table, "SELECT * FROM users WHERE id = 1;");
    assert(result.status == SQL_STATUS_OK);
    expected_ids[0] = 1;
    assert_result_ids(&result, expected_ids, 1);
    assert(strcmp(result.record->name, "Alice") == 0);
    sql_result_destroy(&result);

    result = sql_execute(table, "SELECT * FROM users WHERE id >= 3;");
    assert(result.status == SQL_STATUS_OK);
    expected_ids[0] = 3;
    expected_ids[1] = 4;
    assert_result_ids(&result, expected_ids, 2);
    sql_result_destroy(&result);

    result = sql_execute(table, "SELECT * FROM users WHERE name = 'Alice';");
    assert(result.status == SQL_STATUS_OK);
    expected_ids[0] = 1;
    assert_result_ids(&result, expected_ids, 1);
    assert(result.record->age == 20);
    sql_result_destroy(&result);

    result = sql_execute(table, "SELECT * FROM users WHERE age = 20;");
    assert(result.status == SQL_STATUS_OK);
    expected_ids[0] = 1;
    expected_ids[1] = 3;
    assert_result_ids(&result, expected_ids, 2);
    sql_result_destroy(&result);

    result = sql_execute(table, "SELECT * FROM users WHERE age > 20;");
    assert(result.status == SQL_STATUS_OK);
    expected_ids[0] = 2;
    expected_ids[1] = 4;
    assert_result_ids(&result, expected_ids, 2);
    sql_result_destroy(&result);

    result = sql_execute(table, "SELECT * FROM users WHERE age <= 20;");
    assert(result.status == SQL_STATUS_OK);
    expected_ids[0] = 1;
    expected_ids[1] = 3;
    assert_result_ids(&result, expected_ids, 2);
    sql_result_destroy(&result);

    result = sql_execute(table, "SELECT * FROM users WHERE id = 999;");
    assert(result.status == SQL_STATUS_NOT_FOUND);
    assert_result_ids(&result, expected_ids, 0);
    sql_result_destroy(&result);

    result = sql_execute(table, "SELECT * FROM users WHERE name > 'Alice';");
    assert(result.status == SQL_STATUS_SYNTAX_ERROR);
    sql_result_destroy(&result);

    result = sql_execute(table, "INSERT users VALUES ('Bad', 10);");
    assert(result.status == SQL_STATUS_SYNTAX_ERROR);
    sql_result_destroy(&result);

    table_destroy(table);
}

static void test_sql_detailed_errors(void) {
    Table *table = table_create();
    SQLResult result;

    assert(table != NULL);

    result = sql_execute(table, "SELECT * FORM users;");
    assert(result.status == SQL_STATUS_SYNTAX_ERROR);
    assert(result.error_code == 1064);
    assert(strcmp(result.sql_state, "42000") == 0);
    assert(strstr(result.error_message, "your sql processor2 version") != NULL);
    assert(strstr(result.error_message, "near 'FORM users' at line 1") != NULL);
    sql_result_destroy(&result);

    result = sql_execute(table, "SELECT nickname FROM users;");
    assert(result.status == SQL_STATUS_QUERY_ERROR);
    assert(result.error_code == 1054);
    assert(strcmp(result.sql_state, "42S22") == 0);
    assert(strcmp(result.error_message, "ERROR 1054 (42S22): Unknown column 'nickname' in 'field list'") == 0);
    sql_result_destroy(&result);

    result = sql_execute(table, "SELECT * FROM users WHERE nickname = 1;");
    assert(result.status == SQL_STATUS_QUERY_ERROR);
    assert(result.error_code == 1054);
    assert(strcmp(result.error_message, "ERROR 1054 (42S22): Unknown column 'nickname' in 'where clause'") == 0);
    sql_result_destroy(&result);

    table_destroy(table);
}

static void test_db_server_shared_table_execution(void) {
    DBServer server;
    DBServerExecution execution;
    DBServerMetrics metrics;
    int expected_ids[2];

    assert(db_server_init(&server) == 1);

    assert(db_server_execute(&server, "INSERT INTO users VALUES ('Alice', 20);", &execution) == 1);
    assert(execution.result.status == SQL_STATUS_OK);
    assert(execution.result.action == SQL_ACTION_INSERT);
    assert(execution.result.inserted_id == 1);
    assert(execution.used_index == 0);
    db_server_execution_destroy(&execution);

    assert(db_server_execute(&server, "INSERT INTO users VALUES ('Bob', 30);", &execution) == 1);
    assert(execution.result.status == SQL_STATUS_OK);
    assert(execution.result.inserted_id == 2);
    db_server_execution_destroy(&execution);

    assert(db_server_execute(&server, "SELECT * FROM users;", &execution) == 1);
    assert(execution.result.status == SQL_STATUS_OK);
    expected_ids[0] = 1;
    expected_ids[1] = 2;
    assert_result_ids(&execution.result, expected_ids, 2);
    assert(execution.used_index == 0);
    db_server_execution_destroy(&execution);

    assert(db_server_execute(&server, "SELECT * FROM users WHERE id = 2;", &execution) == 1);
    assert(execution.result.status == SQL_STATUS_OK);
    expected_ids[0] = 2;
    assert_result_ids(&execution.result, expected_ids, 1);
    assert(execution.used_index == 1);
    db_server_execution_destroy(&execution);

    assert(db_server_execute(&server, "SELECT * FROM users WHERE name = 'Alice';", &execution) == 1);
    assert(execution.result.status == SQL_STATUS_OK);
    expected_ids[0] = 1;
    assert_result_ids(&execution.result, expected_ids, 1);
    assert(execution.used_index == 0);
    db_server_execution_destroy(&execution);

    assert(db_server_execute(&server, "QUIT", &execution) == 1);
    assert(execution.result.status == SQL_STATUS_EXIT);
    db_server_execution_destroy(&execution);

    db_server_get_metrics(&server, &metrics);
    assert(metrics.total_query_requests == 6);
    assert(metrics.total_insert_requests == 2);
    assert(metrics.total_select_requests == 3);
    assert(metrics.total_not_found_results == 0);
    assert(metrics.total_query_errors == 1);
    assert(metrics.total_errors == 1);

    db_server_destroy(&server);
}

typedef struct QueryThreadArgs {
    DBServer *server;
    const char *query;
    DBServerExecution execution;
    int executed;
} QueryThreadArgs;

static void *run_query_thread(void *raw_args) {
    QueryThreadArgs *args = (QueryThreadArgs *)raw_args;

    args->executed = db_server_execute(args->server, args->query, &args->execution);
    return NULL;
}

static void test_db_server_lock_timeout_and_metrics(void) {
    DBServer server;
    DBServerConfig config;
    DBServerExecution execution;
    DBServerMetrics metrics;
    PlatformThread thread;
    QueryThreadArgs args;

    db_server_config_default(&config);
    config.lock_timeout_ms = 40;
    config.simulate_write_delay_ms = 200;

    assert(db_server_init_with_config(&server, &config) == 1);

    memset(&args, 0, sizeof(args));
    args.server = &server;
    args.query = "INSERT INTO users VALUES ('Slow Writer', 99);";

    assert(platform_thread_create(&thread, run_query_thread, &args) == 1);
    platform_sleep_ms(40);

    assert(db_server_execute(&server, "SELECT * FROM users;", &execution) == 1);
    assert(execution.server_status == DB_SERVER_EXEC_STATUS_LOCK_TIMEOUT);
    assert(execution.result.status == SQL_STATUS_ERROR);
    db_server_execution_destroy(&execution);

    assert(platform_thread_join(thread) == 1);
    assert(args.executed == 1);
    assert(args.execution.server_status == DB_SERVER_EXEC_STATUS_OK);
    assert(args.execution.result.status == SQL_STATUS_OK);
    db_server_execution_destroy(&args.execution);

    db_server_record_health_request(&server);
    db_server_record_metrics_request(&server);
    db_server_record_queue_full(&server);

    db_server_get_metrics(&server, &metrics);
    assert(metrics.total_query_requests == 2);
    assert(metrics.total_insert_requests == 1);
    assert(metrics.total_select_requests == 1);
    assert(metrics.total_health_requests == 1);
    assert(metrics.total_metrics_requests == 1);
    assert(metrics.total_queue_full == 1);
    assert(metrics.total_lock_timeouts == 1);
    assert(metrics.total_errors == 2);

    db_server_destroy(&server);
}

static void test_api_http_contract(void) {
    DBServer server;
    DBServerExecution execution;
    APIRequest request;
    APIResponse response;
    char raw_request[512];
    char error_message[256];
    char *rendered_response = NULL;
    const char *body = "{\"query\":\"SELECT * FROM users WHERE id = 1;\"}";

    snprintf(
        raw_request,
        sizeof(raw_request),
        "POST /query HTTP/1.1\r\nHost: localhost\r\nContent-Length: %lu\r\n\r\n%s",
        (unsigned long)strlen(body),
        body
    );

    assert(api_parse_http_request(raw_request, &request, error_message, sizeof(error_message)) == 1);
    assert(request.method == API_METHOD_POST);
    assert(strcmp(request.path, "/query") == 0);
    assert(strcmp(request.query, "SELECT * FROM users WHERE id = 1;") == 0);

    assert(db_server_init(&server) == 1);
    assert(db_server_execute(&server, "INSERT INTO users VALUES ('Alice', 20);", &execution) == 1);
    db_server_execution_destroy(&execution);

    assert(db_server_execute(&server, "SELECT * FROM users WHERE id = 1;", &execution) == 1);
    memset(&response, 0, sizeof(response));
    assert(api_build_execution_response(&execution, &response) == 1);
    assert(response.status_code == 200);
    assert(strstr(response.body, "\"action\":\"select\"") != NULL);
    assert(strstr(response.body, "\"usedIndex\":true") != NULL);
    assert(strstr(response.body, "\"name\":\"Alice\"") != NULL);
    assert(api_render_http_response(&response, &rendered_response) == 1);
    assert(strstr(rendered_response, "HTTP/1.1 200 OK") != NULL);
    free(rendered_response);
    rendered_response = NULL;
    api_response_destroy(&response);
    db_server_execution_destroy(&execution);

    assert(db_server_execute(&server, "SELECT * FORM users;", &execution) == 1);
    memset(&response, 0, sizeof(response));
    assert(api_build_execution_response(&execution, &response) == 1);
    assert(response.status_code == 400);
    assert(strstr(response.body, "\"syntax_error\"") != NULL);
    api_response_destroy(&response);
    db_server_execution_destroy(&execution);

    assert(db_server_execute(&server, "EXIT", &execution) == 1);
    memset(&response, 0, sizeof(response));
    assert(api_build_execution_response(&execution, &response) == 1);
    assert(response.status_code == 400);
    assert(strstr(response.body, "\"query_error\"") != NULL);
    assert(strstr(response.body, "CLI mode") != NULL);
    api_response_destroy(&response);
    db_server_execution_destroy(&execution);

    db_server_destroy(&server);
}

/* Runs all unit tests in a single executable. */
int main(void) {
    test_empty_tree_search();
    test_single_insert_search();
    test_duplicate_key_rejected();
    test_leaf_split_search();
    test_internal_split_new_root();
    test_leaf_next_link();
    test_table_auto_increment();
    test_table_find_by_id();
    test_table_linear_search_fields();
    test_table_condition_search();
    test_sql_execution();
    test_sql_detailed_errors();
    test_db_server_shared_table_execution();
    test_db_server_lock_timeout_and_metrics();
    test_api_http_contract();

    printf("All unit tests passed.\n");
    return 0;
}
