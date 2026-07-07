// test_shm_helper.c — Enqueue N entries into SHM for bus_logger tests
// Usage: test_shm_helper <count>

#define _POSIX_C_SOURCE 200809L
#include "common/shm_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    int count = (argc > 1) ? atoi(argv[1]) : 50;
    ShmQueue *q = shm_queue_open(SHM_QUEUE_NAME, 0);
    if (!q) { fprintf(stderr, "open failed\n"); return 1; }

    uint8_t buf[SHM_QUEUE_ENTRY_SIZE];
    memset(buf, 'Z', SHM_QUEUE_ENTRY_SIZE);
    for (int i = 0; i < count; i++) {
        buf[0] = (uint8_t)('A' + (i % 26));
        if (!shm_queue_enqueue(q, buf, SHM_QUEUE_ENTRY_SIZE)) {
            fprintf(stderr, "enqueue failed at %d\n", i);
            shm_queue_close(q, SHM_QUEUE_NAME);
            return 1;
        }
    }
    shm_queue_close(q, SHM_QUEUE_NAME);
    printf("enqueued %d entries\n", count);
    return 0;
}
