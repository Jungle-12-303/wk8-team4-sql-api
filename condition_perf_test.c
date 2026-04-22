#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

/*
 * 벤치마크 전에 미리 삽입할 전체 행 수이다.
 * 큰 데이터셋에서 id 조건의 접근 경로 차이를 보기 위해 100만 건을 사용한다.
 */
#define PERF_INSERT_COUNT 1000000

/*
 * 정확히 같은 값(=) 조건을 몇 번 반복 측정할지 나타낸다.
 */
#define EXACT_QUERY_COUNT 1000

/*
 * 범위 조건(>=)을 몇 번 반복 측정할지 나타낸다.
 */
#define RANGE_QUERY_COUNT 100

/*
 * 현재 시각을 밀리초 단위로 반환한다.
 * 두 시각의 차이를 이용해 쿼리 실행 시간을 잰다.
 */
static double now_ms(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

/*
 * exact-match에서 사용할 앞쪽 id 샘플을 만든다.
 *
 * rows 선형 탐색은 값을 찾는 즉시 종료하므로,
 * 1, 2, 3, ... 처럼 앞부분 id를 사용해 그 케이스를 비교한다.
 */
static void build_exact_id_samples(int *sample_ids, int count, int max_id) {
    int index;

    for (index = 0; index < count; index++) {
        sample_ids[index] = (index % max_id) + 1;
    }
}

/*
 * 비교 연산 하나를 직접 수행한다.
 * rows 선형 탐색 기반 조건 비교에서 사용한다.
 */
static int compare_int(int left, TableComparison comparison, int right) {
    if (comparison == TABLE_COMPARISON_EQ) {
        return left == right;
    }
    if (comparison == TABLE_COMPARISON_LT) {
        return left < right;
    }
    if (comparison == TABLE_COMPARISON_LE) {
        return left <= right;
    }
    if (comparison == TABLE_COMPARISON_GT) {
        return left > right;
    }
    return left >= right;
}

/*
 * 결과 배열 뒤에 레코드 포인터 하나를 추가한다.
 */
static int append_record(Record ***records, size_t *count, size_t *capacity, Record *record) {
    Record **new_records;
    size_t new_capacity;

    if (records == NULL || count == NULL || capacity == NULL || record == NULL) {
        return 0;
    }

    if (*count >= *capacity) {
        new_capacity = (*capacity == 0) ? 16 : (*capacity * 2);
        new_records = (Record **)realloc(*records, sizeof(Record *) * new_capacity);
        if (new_records == NULL) {
            return 0;
        }

        *records = new_records;
        *capacity = new_capacity;
    }

    (*records)[*count] = record;
    (*count)++;
    return 1;
}

/*
 * id 조건을 rows 배열 선형 탐색으로 평가해 결과를 모은다.
 *
 * exact-match(=)는 table_scan_by_id()를 그대로 사용해서
 * 값을 찾는 즉시 종료하는 경로를 비교한다.
 */
static int collect_id_condition_by_row_scan(
    Table *table,
    TableComparison comparison,
    int id,
    Record ***records,
    size_t *count
) {
    size_t index;
    size_t capacity = 0;

    if (table == NULL || records == NULL || count == NULL) {
        return 0;
    }

    *records = NULL;
    *count = 0;

    if (comparison == TABLE_COMPARISON_EQ) {
        Record *record = table_scan_by_id(table, id);

        if (record == NULL) {
            return 1;
        }

        return append_record(records, count, &capacity, record);
    }

    for (index = 0; index < table->size; index++) {
        if (compare_int(table->rows[index]->id, comparison, id)) {
            if (!append_record(records, count, &capacity, table->rows[index])) {
                free(*records);
                *records = NULL;
                *count = 0;
                return 0;
            }
        }
    }

    return 1;
}

/*
 * id = ? exact-match를 여러 번 실행한다.
 *
 * use_btree가 1이면 인덱스를, 0이면 rows 선형 탐색을 사용한다.
 */
static double benchmark_exact_id_lookups(
    Table *table,
    const int *values,
    int query_count,
    int use_btree,
    long long *row_checksum,
    long long *first_id_checksum
) {
    int index;
    double start_ms = now_ms();

    *row_checksum = 0;
    *first_id_checksum = 0;

    for (index = 0; index < query_count; index++) {
        Record *record = use_btree ? table_find_by_id(table, values[index]) : table_scan_by_id(table, values[index]);

        if (record != NULL) {
            *row_checksum += 1;
            *first_id_checksum += record->id;
        }
    }

    return now_ms() - start_ms;
}

/*
 * id 범위 조건을 여러 번 반복 실행한다.
 *
 * use_btree가 1이면 B+Tree 기반 id 조건 수집을,
 * 0이면 rows 선형 탐색 기반 id 조건 수집을 사용한다.
 */
static double benchmark_id_condition_queries(
    Table *table,
    TableComparison comparison,
    int id,
    int repeat_count,
    int use_btree,
    long long *row_checksum,
    long long *first_id_checksum
) {
    int index;
    double start_ms = now_ms();

    *row_checksum = 0;
    *first_id_checksum = 0;

    for (index = 0; index < repeat_count; index++) {
        Record **records = NULL;
        size_t count = 0;
        int ok;

        if (use_btree) {
            ok = table_find_by_id_condition(table, comparison, id, &records, &count);
        } else {
            ok = collect_id_condition_by_row_scan(table, comparison, id, &records, &count);
        }

        if (!ok) {
            free(records);
            return -1.0;
        }

        *row_checksum += (long long)count;
        if (count > 0) {
            *first_id_checksum += records[0]->id;
        }

        free(records);
    }

    return now_ms() - start_ms;
}

/*
 * id 조건을 B+Tree 경로와 rows 스캔 경로로 비교하는 메인 함수이다.
 */
int main(void) {
    Table *table = table_create();
    int *sample_ids;
    int index;
    long long id_exact_btree_rows;
    long long id_exact_row_scan_rows;
    long long id_exact_btree_first_ids;
    long long id_exact_row_scan_first_ids;
    long long id_range_btree_rows;
    long long id_range_row_scan_rows;
    long long id_range_btree_first_ids;
    long long id_range_row_scan_first_ids;
    double id_exact_btree_ms;
    double id_exact_row_scan_ms;
    double id_range_btree_ms;
    double id_range_row_scan_ms;

    if (table == NULL) {
        fprintf(stderr, "Failed to create table.\n");
        return 1;
    }

    sample_ids = (int *)malloc(sizeof(int) * EXACT_QUERY_COUNT);
    if (sample_ids == NULL) {
        fprintf(stderr, "Failed to allocate benchmark samples.\n");
        table_destroy(table);
        return 1;
    }

    for (index = 0; index < PERF_INSERT_COUNT; index++) {
        char name[RECORD_NAME_SIZE];

        snprintf(name, sizeof(name), "user%d", index + 1);
        if (table_insert(table, name, index % 100) == NULL) {
            fprintf(stderr, "Insert failed at row %d.\n", index + 1);
            free(sample_ids);
            table_destroy(table);
            return 1;
        }
    }

    build_exact_id_samples(sample_ids, EXACT_QUERY_COUNT, PERF_INSERT_COUNT);

    id_exact_btree_ms = benchmark_exact_id_lookups(
        table,
        sample_ids,
        EXACT_QUERY_COUNT,
        1,
        &id_exact_btree_rows,
        &id_exact_btree_first_ids
    );

    id_exact_row_scan_ms = benchmark_exact_id_lookups(
        table,
        sample_ids,
        EXACT_QUERY_COUNT,
        0,
        &id_exact_row_scan_rows,
        &id_exact_row_scan_first_ids
    );

    id_range_btree_ms = benchmark_id_condition_queries(
        table,
        TABLE_COMPARISON_GE,
        990001,
        RANGE_QUERY_COUNT,
        1,
        &id_range_btree_rows,
        &id_range_btree_first_ids
    );

    id_range_row_scan_ms = benchmark_id_condition_queries(
        table,
        TABLE_COMPARISON_GE,
        990001,
        RANGE_QUERY_COUNT,
        0,
        &id_range_row_scan_rows,
        &id_range_row_scan_first_ids
    );

    if (id_exact_btree_ms < 0.0 || id_exact_row_scan_ms < 0.0 ||
        id_range_btree_ms < 0.0 || id_range_row_scan_ms < 0.0) {
        free(sample_ids);
        table_destroy(table);
        return 1;
    }

    printf("Exact-match ID benchmark (%d queries)\n", EXACT_QUERY_COUNT);
    printf("+-------------------------------+-----------+------------------+----------------+\n");
    printf("| Method                        | Avg. Rows | Time (ms)        | First-ID Check |\n");
    printf("+-------------------------------+-----------+------------------+----------------+\n");
    printf("| B+Tree WHERE id = ?           | %9.2f | %16.3f | %14lld |\n",
           (double)id_exact_btree_rows / (double)EXACT_QUERY_COUNT,
           id_exact_btree_ms,
           id_exact_btree_first_ids);
    printf("| Rows Scan WHERE id = ?        | %9.2f | %16.3f | %14lld |\n",
           (double)id_exact_row_scan_rows / (double)EXACT_QUERY_COUNT,
           id_exact_row_scan_ms,
           id_exact_row_scan_first_ids);
    printf("+-------------------------------+-----------+------------------+----------------+\n");
    printf("\n");

    printf("Range ID benchmark (%d repeated queries, 10,000 rows/query)\n", RANGE_QUERY_COUNT);
    printf("+-------------------------------+-----------+------------------+----------------+\n");
    printf("| Method                        | Avg. Rows | Time (ms)        | First-ID Check |\n");
    printf("+-------------------------------+-----------+------------------+----------------+\n");
    printf("| B+Tree WHERE id >= 990001     | %9.2f | %16.3f | %14lld |\n",
           (double)id_range_btree_rows / (double)RANGE_QUERY_COUNT,
           id_range_btree_ms,
           id_range_btree_first_ids);
    printf("| Rows Scan WHERE id >= 990001  | %9.2f | %16.3f | %14lld |\n",
           (double)id_range_row_scan_rows / (double)RANGE_QUERY_COUNT,
           id_range_row_scan_ms,
           id_range_row_scan_first_ids);
    printf("+-------------------------------+-----------+------------------+----------------+\n");

    free(sample_ids);
    table_destroy(table);
    return 0;
}
