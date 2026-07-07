// shm_queue.h — Lock-free single-producer / multi-consumer ring queue in named
//               shared memory.
//
// The producer (bus) writes entries at `head`.  Each consumer independently
// tracks its read position in `tails[consumer_id]`.  A slot is only reused
// after every consumer has read it (producer checks `min(tails)` before
// advancing).
//
// The queue is a *ring*: indices wrap modulo CAPACITY so storage is bounded.
// Entries are never overwritten before all consumers have passed them.
//
// Consumer registration:  call shm_queue_register() on the opened queue to
// obtain a consumer_id (0 … MAX_CONSUMERS-1).  That ID is valid for the
// lifetime of the consumer.  If a consumer crashes and restarts it should
// register again (getting a new ID); the old slot in tails[] is abandoned
// and will eventually prevent further writes — restart the bus to clear.

#pragma once
#include <stdint.h>
#include <stddef.h>

#define SHM_QUEUE_NAME            "/ha_device_queue"
#define SHM_QUEUE_CAPACITY        65536      // ring slots
#define SHM_QUEUE_ENTRY_SIZE      1024       // 1 KB per line
#define SHM_QUEUE_MAX_CONSUMERS   16         // max parallel consumers

typedef struct {
    volatile uint64_t head;                       // producer write index (monotonic)
    volatile uint64_t tails[SHM_QUEUE_MAX_CONSUMERS]; // per-consumer read index
    volatile uint32_t next_consumer_id;           // atomic counter for registering consumers
    uint8_t entries[SHM_QUEUE_CAPACITY][SHM_QUEUE_ENTRY_SIZE];
} ShmQueue;

// Open or create the named shared-memory queue.
// `create` must be true for exactly one process (the bus) — it zeroes the SHM.
ShmQueue* shm_queue_open(const char *name, int create);

// Unmap shared memory.  Does NOT unlink — the producer may zero everything
// on the next restart.
void shm_queue_close(ShmQueue *q, const char *name);

// Enqueue one entry (copy at most ENTRY_SIZE bytes, zero-pad the rest).
// Returns 0 if all slots are occupied (ring full).
int shm_queue_enqueue(ShmQueue *q, const uint8_t *data, size_t len);

// Register as a consumer.  Returns consumer_id (0 … MAX_CONSUMERS-1) or -1
// if no slot is available.
int shm_queue_register(ShmQueue *q);

// Dequeue one entry for the given consumer.  Returns bytes copied (0 if
// the consumer has caught up with the producer).
size_t shm_queue_dequeue(ShmQueue *q, uint8_t *buf, size_t buf_size,
                         int consumer_id);
