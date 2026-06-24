#pragma once
#include <stdint.h>
#include <stddef.h>
#include <poll.h>
#include <netinet/in.h>
#include <signal.h>
#include "common/protocol.h"

#define MAX_TIMERS 32

void* safe_malloc(size_t size);
char* safe_strdup(const char *s);
#define SAFE_FREE(ptr) do { if (ptr) { free(ptr); ptr = NULL; } } while(0)

typedef struct {
    int fd;
    uint8_t *read_buf;
    size_t read_buf_size;
    size_t read_buf_offset;
    uint8_t *write_buf;
    size_t write_buf_size;
    size_t write_buf_offset;
    size_t write_buf_pending;
} ConnectionBuffer;

ConnectionBuffer* conn_buf_create(int fd, size_t buf_size);
void conn_buf_destroy(ConnectionBuffer *cb);

typedef enum {
    READ_STATE_HEADER = 0,
    READ_STATE_PAYLOAD,
} ReadState;

typedef struct {
    ConnectionBuffer *buf;
    ReadState read_state;
    MessageHeader current_header;
    size_t payload_read;
} ProtocolReader;

void protocol_reader_init(ProtocolReader *pr, ConnectionBuffer *buf);
int protocol_read_message(ProtocolReader *pr, uint8_t **out_msg, uint16_t *out_len);

int create_nonblocking_socket(void);
int64_t current_time_ms(void);

typedef struct {
    int fd;
    struct sockaddr_in target_addr;
    int retry_count;
    int max_retry_interval_ms;
} ReconnectContext;

int try_connect(ReconnectContext *ctx);
int get_next_backoff_ms(ReconnectContext *ctx);

#define MAX_FDS 128
typedef void (*event_callback)(int fd, short revents, void *arg);
typedef void (*timer_callback)(void *arg);

typedef struct {
    int fd;
    short events;
    event_callback cb;
    void *arg;
} PollHandler;

typedef struct {
    int interval_ms;
    int64_t next_trigger;
    timer_callback cb;
    void *arg;
} TimerEntry;

void event_loop(void);
int register_handler(int fd, short events, event_callback cb, void *arg);
void unregister_handler(int fd);
int register_timer(int interval_ms, timer_callback cb, void *arg);

extern volatile sig_atomic_t running;
void setup_signals(void);

int unregister_timer(int timer_id);