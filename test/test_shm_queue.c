// test_shm_queue.c — Unit tests for SPMC lock-free SHM queue
//
// Build: clang-7 -std=c11 -Wall -Wextra -Werror -g -O2 -Iinclude \
//            test/test_shm_queue.c src/common/shm_queue.c -o bin/test_shm_queue -lrt
//
// Run:   bin/test_shm_queue

#define _POSIX_C_SOURCE 200809L
#include "common/shm_queue.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

static int g_total  = 0;
static int g_passed = 0;

#define TEST(msg, cond) do {                                             \
    g_total++;                                                           \
    if (cond) { g_passed++; printf("  [PASS] %s\n", msg); }             \
    else      { printf("  [FAIL] %s (line %d)\n", msg, __LINE__); }     \
    fflush(stdout);                                                      \
} while(0)

// ---- helpers ------------------------------------------------------------

static void fill_seq(uint8_t *buf, size_t len, uint64_t seq) {
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(seq + i);
}

static int check_seq(const uint8_t *buf, size_t len, uint64_t seq) {
    for (size_t i = 0; i < len; i++)
        if (buf[i] != (uint8_t)(seq + i)) return 0;
    return 1;
}

static ShmQueue* fresh_queue(void) {
    shm_unlink(SHM_QUEUE_NAME);
    return shm_queue_open(SHM_QUEUE_NAME, 1);
}

// ---- test cases ---------------------------------------------------------

static void test_basic(void) {
    printf("=== S1: Basic enqueue/dequeue ===\n");
    ShmQueue *q = fresh_queue();  if (!q) return;
    int c0 = shm_queue_register(q);
    TEST("S1 register consumer", c0 == 0);

    uint8_t data[SHM_QUEUE_ENTRY_SIZE];
    uint8_t buf[SHM_QUEUE_ENTRY_SIZE];
    memset(data, 0xAB, sizeof(data));
    for (int i = 0; i < 5; i++) {
        data[0] = (uint8_t)i;
        TEST("S1 enqueue", shm_queue_enqueue(q, data, sizeof(data)));
    }
    for (int i = 0; i < 5; i++) {
        size_t n = shm_queue_dequeue(q, buf, sizeof(buf), c0);
        TEST("S1 dequeue", n == SHM_QUEUE_ENTRY_SIZE && buf[0] == (uint8_t)i);
    }
    TEST("S1 dequeue empty",
         shm_queue_dequeue(q, buf, sizeof(buf), c0) == 0);
    shm_queue_close(q, SHM_QUEUE_NAME);
}

static void test_multi_consumer(void) {
    printf("=== S2: Multiple independent consumers ===\n");
    ShmQueue *q = fresh_queue();  if (!q) return;
    int c0 = shm_queue_register(q);
    int c1 = shm_queue_register(q);
    TEST("S2 register c0==0 c1==1", c0 == 0 && c1 == 1);

    uint8_t data[SHM_QUEUE_ENTRY_SIZE];
    uint8_t buf[SHM_QUEUE_ENTRY_SIZE];
    int ok = 1;
    for (int i = 0; i < 10; i++) {
        memset(data, (uint8_t)(i * 17), sizeof(data));
        ok = ok && shm_queue_enqueue(q, data, sizeof(data));
    }
    TEST("S2 enqueued 10", ok);

    // c0 reads first 4, c1 reads all 10, c0 reads remaining 6
    ok = 1;
    for (int i = 0; i < 4; i++) {
        size_t n = shm_queue_dequeue(q, buf, sizeof(buf), c0);
        ok = ok && (n == SHM_QUEUE_ENTRY_SIZE && buf[0] == (uint8_t)(i * 17));
    }
    for (int i = 0; i < 10; i++) {
        size_t n = shm_queue_dequeue(q, buf, sizeof(buf), c1);
        ok = ok && (n == SHM_QUEUE_ENTRY_SIZE && buf[0] == (uint8_t)(i * 17));
    }
    for (int i = 4; i < 10; i++) {
        size_t n = shm_queue_dequeue(q, buf, sizeof(buf), c0);
        ok = ok && (n == SHM_QUEUE_ENTRY_SIZE && buf[0] == (uint8_t)(i * 17));
    }
    TEST("S2 all reads correct", ok);
    TEST("S2 c0 empty",  shm_queue_dequeue(q, buf, sizeof(buf), c0) == 0);
    TEST("S2 c1 empty",  shm_queue_dequeue(q, buf, sizeof(buf), c1) == 0);
    shm_queue_close(q, SHM_QUEUE_NAME);
}

static void test_ring_full(void) {
    printf("=== S3: Ring full blocks until all consumers read ===\n");
    ShmQueue *q = fresh_queue();  if (!q) return;
    int c0 = shm_queue_register(q);
    int c1 = shm_queue_register(q);

    uint8_t data[SHM_QUEUE_ENTRY_SIZE];
    uint8_t buf[SHM_QUEUE_ENTRY_SIZE];
    memset(data, 0xAA, sizeof(data));

    int written = 0;
    for (int i = 0; i < SHM_QUEUE_CAPACITY; i++) {
        if (!shm_queue_enqueue(q, data, sizeof(data))) break;
        written++;
    }
    TEST("S3 fill all slots", written == SHM_QUEUE_CAPACITY);
    TEST("S3 full (no readers)", !shm_queue_enqueue(q, data, sizeof(data)));

    // c1 reads all — still full because c0 hasn't
    for (int i = 0; i < SHM_QUEUE_CAPACITY; i++)
        shm_queue_dequeue(q, buf, sizeof(buf), c1);
    TEST("S3 still full (c0 behind)", !shm_queue_enqueue(q, data, sizeof(data)));

    // c0 reads all — now space is free
    for (int i = 0; i < SHM_QUEUE_CAPACITY; i++)
        shm_queue_dequeue(q, buf, sizeof(buf), c0);

    memset(data, 0xBB, sizeof(data));
    TEST("S3 can write after all readers",
         shm_queue_enqueue(q, data, sizeof(data)));

    // Verify new entry at wrapped slot
    size_t n = shm_queue_dequeue(q, buf, sizeof(buf), c0);
    TEST("S3 wrapped entry", n == SHM_QUEUE_ENTRY_SIZE && buf[0] == 0xBB);

    n = shm_queue_dequeue(q, buf, sizeof(buf), c1);
    TEST("S3 c1 same entry", n == SHM_QUEUE_ENTRY_SIZE && buf[0] == 0xBB);
    shm_queue_close(q, SHM_QUEUE_NAME);
}

static void test_registration_limit(void) {
    printf("=== S4: Consumer registration limit ===\n");
    ShmQueue *q = fresh_queue();  if (!q) return;

    int last_ok = -1;
    int i;
    for (i = 0; i <= SHM_QUEUE_MAX_CONSUMERS; i++) {
        int id = shm_queue_register(q);
        if (id < 0) break;
        last_ok = id;
    }
    TEST("S4 last success id", last_ok == SHM_QUEUE_MAX_CONSUMERS - 1);
    TEST("S4 max+1 returns -1", i == SHM_QUEUE_MAX_CONSUMERS);

    uint8_t data[SHM_QUEUE_ENTRY_SIZE];
    uint8_t buf[SHM_QUEUE_ENTRY_SIZE];
    memset(data, 0xCC, sizeof(data));
    TEST("S4 enqueue", shm_queue_enqueue(q, data, sizeof(data)));
    int ok = 1;
    for (int j = 0; j < SHM_QUEUE_MAX_CONSUMERS; j++) {
        size_t n = shm_queue_dequeue(q, buf, sizeof(buf), j);
        ok = ok && (n == SHM_QUEUE_ENTRY_SIZE && buf[0] == 0xCC);
    }
    TEST("S4 all read", ok);
    shm_queue_close(q, SHM_QUEUE_NAME);
}

static void test_data_integrity(void) {
    printf("=== S5: Data integrity across ring wrap ===\n");
    ShmQueue *q = fresh_queue();  if (!q) return;
    int c0 = shm_queue_register(q);

    uint8_t data[SHM_QUEUE_ENTRY_SIZE];
    uint8_t buf[SHM_QUEUE_ENTRY_SIZE];

    int total = SHM_QUEUE_CAPACITY * 5 / 2;
    int ok = 1;
    for (uint64_t i = 0; i < (uint64_t)total; i++) {
        fill_seq(data, sizeof(data), i);
        if (!shm_queue_enqueue(q, data, sizeof(data))) { ok = 0; break; }
        size_t n = shm_queue_dequeue(q, buf, sizeof(buf), c0);
        if (n != SHM_QUEUE_ENTRY_SIZE)                 { ok = 0; break; }
        if (!check_seq(buf, sizeof(buf), i))            { ok = 0; break; }
    }
    TEST("S5 all wrap cycles passed", ok);
    TEST("S5 empty after wrap",
         shm_queue_dequeue(q, buf, sizeof(buf), c0) == 0);
    shm_queue_close(q, SHM_QUEUE_NAME);
}

static void test_partial_consume(void) {
    printf("=== S6: Producer resumes after partial consumption ===\n");
    ShmQueue *q = fresh_queue();  if (!q) return;
    int c0 = shm_queue_register(q);
    int c1 = shm_queue_register(q);

    uint8_t data[SHM_QUEUE_ENTRY_SIZE];
    uint8_t buf[SHM_QUEUE_ENTRY_SIZE];
    int half = SHM_QUEUE_CAPACITY / 2;

    int ok = 1;
    for (int i = 0; i < half; i++) {
        memset(data, (uint8_t)i, sizeof(data));
        ok = ok && shm_queue_enqueue(q, data, sizeof(data));
    }
    TEST("S6 write first half", ok);

    // c0 reads first half
    ok = 1;
    for (int i = 0; i < half; i++) {
        size_t n = shm_queue_dequeue(q, buf, sizeof(buf), c0);
        ok = ok && (n == SHM_QUEUE_ENTRY_SIZE && buf[0] == (uint8_t)i);
    }
    TEST("S6 c0 reads half", ok);

    // Fill the second half (should work — c1 hasn't read, but c1's tail
    // is at 0, so head - min_tail = half < CAPACITY)
    ok = 1;
    for (int i = half; i < half * 2; i++) {
        memset(data, (uint8_t)i, sizeof(data));
        ok = ok && shm_queue_enqueue(q, data, sizeof(data));
    }
    TEST("S6 write second half", ok);

    // c1 reads all entries
    ok = 1;
    for (int i = 0; i < half * 2; i++) {
        size_t n = shm_queue_dequeue(q, buf, sizeof(buf), c1);
        ok = ok && (n == SHM_QUEUE_ENTRY_SIZE && buf[0] == (uint8_t)i);
    }
    TEST("S6 c1 reads all", ok);

    // c0 reads second half
    ok = 1;
    for (int i = half; i < half * 2; i++) {
        size_t n = shm_queue_dequeue(q, buf, sizeof(buf), c0);
        ok = ok && (n == SHM_QUEUE_ENTRY_SIZE && buf[0] == (uint8_t)i);
    }
    TEST("S6 c0 reads second half", ok);

    shm_queue_close(q, SHM_QUEUE_NAME);
}

int main(void) {
    printf("========================================================\n");
    printf("  SHM Queue SPMC Unit Tests\n");
    printf("========================================================\n\n");

    test_basic();
    printf("\n");
    test_multi_consumer();
    printf("\n");
    test_ring_full();
    printf("\n");
    test_registration_limit();
    printf("\n");
    test_data_integrity();
    printf("\n");
    test_partial_consume();

    shm_unlink(SHM_QUEUE_NAME);

    printf("\n========================================================\n");
    printf("  Results: %d / %d passed\n", g_passed, g_total);
    printf("========================================================\n");
    return g_passed == g_total ? 0 : 1;
}
