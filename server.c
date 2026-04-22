#include "table.h"

#include <stdio.h>

/*
 * Minimal server entrypoint for implementation step 1.
 * The networking/runtime layers will be added in later steps.
 */
int main(void) {
    Table *table = table_create();

    if (table == NULL) {
        fprintf(stderr, "Failed to create shared table for server bootstrap.\n");
        return 1;
    }

    printf("SQL API server bootstrap ready.\n");
    printf("Networking and worker runtime are not implemented in this step.\n");

    table_destroy(table);
    return 0;
}
