#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

/*
 * 성능 실험에서 미리 삽입할 전체 행 수이다.
 * 100만 건을 넣어서 인덱스 효과가 눈에 띄게 보이도록 한다.
 */
#define PERF_INSERT_COUNT 1000000

/*
 * 같은 id 조회를 몇 번 반복 비교할지 나타낸다.
 * README 예시 이미지는 1000회 기준 결과를 사용한다.
 */
#define PERF_SEARCH_COUNT 1000

/*
 * 현재 시간을 밀리초 단위 실수값으로 반환한다.
 *
 * 시작 시각과 종료 시각을 각각 구한 뒤 빼면
 * 특정 작업에 걸린 시간을 대략 측정할 수 있다.
 */
static double now_ms(void) {
    /* 초와 마이크로초를 함께 돌려주는 구조체이다. */
    struct timeval tv;

    /* 현재 시각을 tv에 채운다. */
    gettimeofday(&tv, NULL);

    /* 초와 마이크로초를 모두 밀리초 단위로 바꿔 합친다. */
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

/*
 * 매 실행마다 같은 id 패턴을 쓰기 위해 샘플 id 배열을 만든다.
 *
 * 난수를 쓰면 실행마다 결과가 달라질 수 있으므로,
 * 간단한 수식을 써서 항상 같은 순서의 id를 만들었다.
 */
static void build_sample_ids(int *sample_ids, int count, int max_id) {
    /* 배열을 채울 때 사용할 인덱스이다. */
    int index;

    /* count개 만큼 샘플 id를 만든다. */
    for (index = 0; index < count; index++) {
        /*
         * 97을 곱한 뒤 max_id로 나눈 나머지를 쓰면
         * 1~max_id 범위 안에서 꽤 고르게 섞인 값이 만들어진다.
         */
        sample_ids[index] = ((index * 97) % max_id) + 1;
    }
}

/*
 * B+ 트리 id 검색과 rows 선형 탐색 id 검색을 비교하는 메인 함수이다.
 *
 * 흐름:
 * 1. 테이블 생성
 * 2. 100만 행 삽입
 * 3. 같은 샘플 id 1000개로 B+ 트리 검색 시간 측정
 * 4. 같은 샘플 id 1000개로 rows 선형 탐색 시간 측정
 * 5. 표로 출력
 */
int main(void) {
    /* 실험에 사용할 테이블이다. */
    Table *table = table_create();

    /* 검색에 사용할 샘플 id 배열이다. */
    int *sample_ids;

    /* 반복문 인덱스이다. */
    int index;

    /* 측정 시작 시각을 담는다. */
    double start_ms;

    /* B+ 트리 검색에 걸린 총 시간이다. */
    double bptree_ms;

    /* rows 배열을 훑는 선형 탐색에 걸린 총 시간이다. */
    double row_scan_ms;

    /* rows 선형 탐색 / B+ 트리 시간 비율이다. */
    double speedup;

    /* 각 방식의 조회 1회당 평균 반환 행 수이다. */
    double avg_rows_index;
    double avg_rows_row_scan;

    /*
     * 두 검색 방식이 정말 같은 레코드를 찾았는지 확인하기 위한 체크섬이다.
     * 각 검색 결과의 id를 누적해서 비교한다.
     */
    long long checksum_index = 0;
    long long checksum_row_scan = 0;

    /* 각 방식이 실제로 몇 번 행을 찾았는지 센다. */
    long long hit_count_index = 0;
    long long hit_count_row_scan = 0;

    /* 테이블 생성 실패 시 더 진행할 수 없다. */
    if (table == NULL) {
        fprintf(stderr, "Failed to create table.\n");
        return 1;
    }

    /* 샘플 id 배열을 만들 메모리를 확보한다. */
    sample_ids = (int *)malloc(sizeof(int) * PERF_SEARCH_COUNT);
    if (sample_ids == NULL) {
        fprintf(stderr, "Failed to allocate sample IDs.\n");
        table_destroy(table);
        return 1;
    }

    /* 100만 행을 순서대로 삽입한다. */
    for (index = 0; index < PERF_INSERT_COUNT; index++) {
        /* user1, user2 같은 이름을 만들 임시 버퍼이다. */
        char name[RECORD_NAME_SIZE];

        /* 이름 문자열을 만든다. */
        snprintf(name, sizeof(name), "user%d", index + 1);

        /*
         * age는 0~99를 반복하도록 넣는다.
         * 이 파일의 핵심은 id 검색 비교이므로 age 값은 단순 패턴이면 충분하다.
         */
        if (table_insert(table, name, index % 100) == NULL) {
            fprintf(stderr, "Insert failed at row %d.\n", index + 1);
            free(sample_ids);
            table_destroy(table);
            return 1;
        }
    }

    /* 반복 측정에 사용할 샘플 id들을 미리 만든다. */
    build_sample_ids(sample_ids, PERF_SEARCH_COUNT, PERF_INSERT_COUNT);

    /* B+ 트리 검색 시간을 잰다. */
    start_ms = now_ms();
    for (index = 0; index < PERF_SEARCH_COUNT; index++) {
        /* id 인덱스를 사용하는 검색이다. */
        Record *record = table_find_by_id(table, sample_ids[index]);
        if (record != NULL) {
            checksum_index += record->id;
            hit_count_index++;
        }
    }
    bptree_ms = now_ms() - start_ms;

    /* 같은 샘플들로 rows 선형 탐색 시간을 잰다. */
    start_ms = now_ms();
    for (index = 0; index < PERF_SEARCH_COUNT; index++) {
        /* rows 배열을 처음부터 훑는 검색이다. */
        Record *record = table_scan_by_id(table, sample_ids[index]);
        if (record != NULL) {
            checksum_row_scan += record->id;
            hit_count_row_scan++;
        }
    }
    row_scan_ms = now_ms() - start_ms;

    /*
     * 두 방식의 체크섬이 다르면
     * 빠르기 비교 이전에 검색 결과가 서로 다르다는 뜻이므로 실패 처리한다.
     */
    if (checksum_index != checksum_row_scan || hit_count_index != hit_count_row_scan) {
        fprintf(stderr, "Search results mismatch.\n");
        free(sample_ids);
        table_destroy(table);
        return 1;
    }

    /* 0으로 나누기를 피하면서 배속을 계산한다. */
    speedup = (bptree_ms > 0.0) ? (row_scan_ms / bptree_ms) : 0.0;

    /* id 단건 조회이므로 평균 반환 행 수는 "찾은 횟수 / 총 조회 횟수"이다. */
    avg_rows_index = (double)hit_count_index / (double)PERF_SEARCH_COUNT;
    avg_rows_row_scan = (double)hit_count_row_scan / (double)PERF_SEARCH_COUNT;

    /* 결과를 보기 쉬운 표로 출력한다. */
    printf("perf_test (%d rows, %d id lookups)\n", PERF_INSERT_COUNT, PERF_SEARCH_COUNT);
    printf("+-----------------+-----------+-----------+\n");
    printf("| Method          | Avg. Rows | Time (ms) |\n");
    printf("+-----------------+-----------+-----------+\n");
    printf("| B+Tree ID Lookup| %9.2f | %9.3f |\n", avg_rows_index, bptree_ms);
    printf("| Rows ID Scan    | %9.2f | %9.3f |\n", avg_rows_row_scan, row_scan_ms);
    printf("+-----------------+-----------+-----------+\n");
    printf("Speedup: %.2fx\n", speedup);

    /* 실험용 메모리를 모두 정리한다. */
    free(sample_ids);
    table_destroy(table);
    return 0;
}
