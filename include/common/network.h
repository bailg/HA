// network.h — TCP networking, connection buffering, protocol reader, and event loop
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <poll.h>
#include <netinet/in.h>
#include <signal.h>
#include "common/protocol.h"
#include "common/memory.h"

#define MAX_TIMERS 32

// ======== Connection Buffer ========
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

// Create a connection buffer for given fd and buffer size
ConnectionBuffer* conn_buf_create(int fd, size_t buf_size);
// Destroy connection buffer and free resources
void conn_buf_destroy(ConnectionBuffer *cb);

// ======== Protocol Reader ========
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

// Initialize protocol reader with a connection buffer
void protocol_reader_init(ProtocolReader *pr, ConnectionBuffer *buf);
// Read one complete message from protocol reader, returns 0 on success
int protocol_read_message(ProtocolReader *pr, uint8_t **out_msg, uint16_t *out_len);

// ======== Socket Helpers ========
// Create a non-blocking TCP socket
int create_nonblocking_socket(void);
// Get current time in milliseconds
int64_t current_time_ms(void);

typedef struct {
    int fd;
    struct sockaddr_in target_addr;
    int retry_count;
    int max_retry_interval_ms;
} ReconnectContext;

// Attempt connection with retry logic
int try_connect(ReconnectContext *ctx);
// Get next exponential backoff interval in milliseconds
int get_next_backoff_ms(ReconnectContext *ctx);

// ======== Event Loop ========
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

// Run the main event loop
void event_loop(void);
// Register a poll handler for the given fd
int register_handler(int fd, short events, event_callback cb, void *arg);
// Unregister a previously registered poll handler
void unregister_handler(int fd);
// Register a periodic timer, returns timer ID
int register_timer(int interval_ms, timer_callback cb, void *arg);
// Unregister a timer by ID
int unregister_timer(int timer_id);

// Global running flag, set to 0 on signal
extern volatile sig_atomic_t running;
// Set up signal handlers for graceful shutdown
void setup_signals(void);