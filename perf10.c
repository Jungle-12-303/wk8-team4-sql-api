#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

/*
 * 성능 실험에서 미리 삽입할 전체 행 수이다.
 * 원본 perf_test와 동일하게 100만 건을 유지한다.
 */
#define PERF_INSERT_COUNT 1000000

/*
 * 빠르게 결과를 확인할 수 있도록 검색 반복 횟수를 10회로 줄인다.
 */
#define PERF_SEARCH_COUNT 10

/*
 * 현재 시간을 밀리초 단위 실수값으로 반환한다.
 */
static double now_ms(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

/*
 * 원본 perf_test와 같은 규칙으로 샘플 id 배열을 만든다.
 */
static void build_sample_ids(int *sample_ids, int count, int max_id) {
    int index;

    for (index = 0; index < count; index++) {
        sample_ids[index] = ((index * 97) % max_id) + 1;
    }
}

/*
 * perf_test의 검색 반복 횟수만 10회로 줄인 별도 벤치마크이다.
 */
int main(void) {
    Table *table = table_create();
    int *sample_ids;
    int index;
    double start_ms;
    double bptree_ms;
    double linear_ms;
    double speedup;
    double avg_rows_index;
    double avg_rows_linear;
    long long checksum_index = 0;
    long long checksum_linear = 0;
    long long hit_count_index = 0;
    long long hit_count_linear = 0;

    if (table == NULL) {
        fprintf(stderr, "Failed to create table.\n");
        return 1;
    }

    sample_ids = (int *)malloc(sizeof(int) * PERF_SEARCH_COUNT);
    if (sample_ids == NULL) {
        fprintf(stderr, "Failed to allocate sample IDs.\n");
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

    build_sample_ids(sample_ids, PERF_SEARCH_COUNT, PERF_INSERT_COUNT);

    start_ms = now_ms();
    for (index = 0; index < PERF_SEARCH_COUNT; index++) {
        Record *record = table_find_by_id(table, sample_ids[index]);
        if (record != NULL) {
            checksum_index += record->id;
            hit_count_index++;
        }
    }
    bptree_ms = now_ms() - start_ms;

    start_ms = now_ms();
    for (index = 0; index < PERF_SEARCH_COUNT; index++) {
        Record *record = table_scan_by_id(table, sample_ids[index]);
        if (record != NULL) {
            checksum_linear += record->id;
            hit_count_linear++;
        }
    }
    linear_ms = now_ms() - start_ms;

    if (checksum_index != checksum_linear || hit_count_index != hit_count_linear) {
        fprintf(stderr, "Search results mismatch.\n");
        free(sample_ids);
        table_destroy(table);
        return 1;
    }

    speedup = (bptree_ms > 0.0) ? (linear_ms / bptree_ms) : 0.0;
    avg_rows_index = (double)hit_count_index / (double)PERF_SEARCH_COUNT;
    avg_rows_linear = (double)hit_count_linear / (double)PERF_SEARCH_COUNT;

    printf("perf_test_10 (%d searches)\n", PERF_SEARCH_COUNT);
    printf("+-----------------+-----------+-----------+\n");
    printf("| Method          | Avg. Rows | Time (ms) |\n");
    printf("+-----------------+-----------+-----------+\n");
    printf("| B+Tree ID Lookup| %9.2f | %9.3f |\n", avg_rows_index, bptree_ms);
    printf("| Rows ID Scan    | %9.2f | %9.3f |\n", avg_rows_linear, linear_ms);
    printf("+-----------------+-----------+-----------+\n");
    printf("Speedup: %.2fx\n", speedup);

    free(sample_ids);
    table_destroy(table);
    return 0;
}
