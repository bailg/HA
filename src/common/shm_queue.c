// shm_queue.c — Lock-free SPMC ring queue over POSIX named shared memory
//
// The producer (bus) writes new entries at `head` and advances.  Each
// consumer holds its own `tail[i]` so they can advance independently.
// Before writing, the producer computes `min(tails)` and only advances
// when at least one slot is free — i.e. when `head - min_tail < CAPACITY`.
// This guarantees that no entry is overwritten before every consumer has
// read it.

#define _POSIX_C_SOURCE 200809L
#include "common/shm_queue.h"
#include "common/logging.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

// ---- Open / close -------------------------------------------------------

ShmQueue* shm_queue_open(const char *name, int create) {
    int fd;
    if (create) {
        fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd < 0) {
            LOG_ERROR("shm_open O_CREAT %s failed: %s", name, strerror(errno));
            return NULL;
        }
        if (ftruncate(fd, sizeof(ShmQueue)) < 0) {
            LOG_ERROR("ftruncate %s failed: %s", name, strerror(errno));
            close(fd); shm_unlink(name);
            return NULL;
        }
    } else {
        fd = shm_open(name, O_RDWR, 0);
        if (fd < 0) {
            LOG_ERROR("shm_open %s failed: %s", name, strerror(errno));
            return NULL;
        }
    }
    ShmQueue *q = mmap(NULL, sizeof(ShmQueue), PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
    close(fd);
    if (q == MAP_FAILED) {
        LOG_ERROR("mmap %s failed: %s", name, strerror(errno));
        if (create) shm_unlink(name);
        return NULL;
    }
    if (create) {
        memset(q, 0, sizeof(ShmQueue));
        // Unregistered consumer slots are UINT64_MAX so the producer can
        // distinguish "no consumer" from "consumer at position 0".
        for (int i = 0; i < SHM_QUEUE_MAX_CONSUMERS; i++) {
            __atomic_store_n(&q->tails[i], UINT64_MAX, __ATOMIC_RELAXED);
        }
    }
    return q;
}

void shm_queue_close(ShmQueue *q, const char *name) {
    (void)name;
    if (q) {
        munmap(q, sizeof(ShmQueue));
    }
}

// ---- Producer ------------------------------------------------------------

int shm_queue_enqueue(ShmQueue *q, const uint8_t *data, size_t len) {
    uint64_t head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);

    // Compute the minimum tail across all registered consumers.
    // Unregistered slots (UINT64_MAX) are skipped so they never block the
    // producer.
    uint64_t min_tail = head;
    for (int i = 0; i < SHM_QUEUE_MAX_CONSUMERS; i++) {
        uint64_t t = __atomic_load_n(&q->tails[i], __ATOMIC_RELAXED);
        if (t == UINT64_MAX) continue;         // unregistered slot
        if (t < min_tail) min_tail = t;
    }

    if (head - min_tail >= SHM_QUEUE_CAPACITY) {
        LOG_WARN("shm_queue full (all slots occupied), dropping message");
        return 0;
    }

    uint32_t idx = (uint32_t)(head % SHM_QUEUE_CAPACITY);
    size_t copy_len = len < SHM_QUEUE_ENTRY_SIZE ? len : SHM_QUEUE_ENTRY_SIZE;
    memcpy(q->entries[idx], data, copy_len);
    if (copy_len < SHM_QUEUE_ENTRY_SIZE) {
        memset(q->entries[idx] + copy_len, 0,
               SHM_QUEUE_ENTRY_SIZE - copy_len);
    }
    __atomic_store_n(&q->head, head + 1, __ATOMIC_RELEASE);
    return 1;
}

// ---- Consumer registration -----------------------------------------------

int shm_queue_register(ShmQueue *q) {
    uint32_t id = __atomic_fetch_add(&q->next_consumer_id, 1,
                                     __ATOMIC_RELAXED);
    if (id >= SHM_QUEUE_MAX_CONSUMERS) {
        LOG_ERROR("shm_queue: no free consumer slot (max=%d)",
                  SHM_QUEUE_MAX_CONSUMERS);
        return -1;
    }
    // The slot was zeroed by the bus at creation; the consumer starts
    // from the current head (catches up from where the queue is now).
    __atomic_store_n(&q->tails[id], 0, __ATOMIC_RELEASE);
    return (int)id;
}

// ---- Consumer ------------------------------------------------------------

size_t shm_queue_dequeue(ShmQueue *q, uint8_t *buf, size_t buf_size,
                         int consumer_id) {
    if (consumer_id < 0 || consumer_id >= SHM_QUEUE_MAX_CONSUMERS) return 0;

    uint64_t tail = __atomic_load_n(&q->tails[consumer_id],
                                    __ATOMIC_RELAXED);
    uint64_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);

    if (head == tail) return 0;

    uint32_t idx = (uint32_t)(tail % SHM_QUEUE_CAPACITY);
    size_t copy_len = buf_size < SHM_QUEUE_ENTRY_SIZE
                          ? buf_size
                          : SHM_QUEUE_ENTRY_SIZE;
    memcpy(buf, q->entries[idx], copy_len);

    __atomic_store_n(&q->tails[consumer_id], tail + 1, __ATOMIC_RELEASE);
    return copy_len;
}
