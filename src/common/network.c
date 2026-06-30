#include "common/network.h"
#include "common/logging.h"
#include "common/memory.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <errno.h>

ConnectionBuffer* conn_buf_create(int fd, size_t buf_size) {
    ConnectionBuffer *cb = safe_malloc(sizeof(ConnectionBuffer));
    cb->fd = fd;
    cb->read_buf = safe_malloc(buf_size);
    cb->read_buf_size = buf_size;
    cb->read_buf_offset = 0;
    cb->write_buf = safe_malloc(buf_size);
    cb->write_buf_size = buf_size;
    cb->write_buf_offset = 0;
    cb->write_buf_pending = 0;
    return cb;
}

void conn_buf_destroy(ConnectionBuffer *cb) {
    if (cb) {
        SAFE_FREE(cb->read_buf);
        SAFE_FREE(cb->write_buf);
        SAFE_FREE(cb);
    }
}

void protocol_reader_init(ProtocolReader *pr, ConnectionBuffer *buf) {
    pr->buf = buf;
    pr->read_state = READ_STATE_HEADER;
    pr->payload_read = 0;
    memset(&pr->current_header, 0, sizeof(MessageHeader));
}

int protocol_read_message(ProtocolReader *pr, uint8_t **out_msg, uint16_t *out_len) {
    ConnectionBuffer *buf = pr->buf;
    if (pr->read_state == READ_STATE_HEADER) {
        size_t header_size = sizeof(MessageHeader);
        if (buf->read_buf_offset < header_size) {
            return 0;
        }
        
        // 拷贝出头部，并【立即转为主机序】再进行校验，避免双重 ntohs 翻转
        memcpy(&pr->current_header, buf->read_buf, header_size);
        protocol_ntoh_header(&pr->current_header);
        
        if (pr->current_header.length < header_size || 
            pr->current_header.length > header_size + PAYLOAD_MAX_LEN + 64) {
            LOG_ERROR("Invalid message length: %u, type: %u", 
                      pr->current_header.length, pr->current_header.type);
            return -1;
        }
        pr->read_state = READ_STATE_PAYLOAD;
        pr->payload_read = 0;
    }
    
    size_t total_size = pr->current_header.length;
    if (buf->read_buf_offset < total_size) {
        return 0; 
    }
    
    *out_len = (uint16_t)total_size;
    *out_msg = safe_malloc(total_size);
    // 注意：这里提取出来的 out_msg 依然是网络字节序，由调用者根据 type 自行转换
    memcpy(*out_msg, buf->read_buf, total_size);
    
    memmove(buf->read_buf, buf->read_buf + total_size, buf->read_buf_offset - total_size);
    buf->read_buf_offset -= total_size;
    
    pr->read_state = READ_STATE_HEADER;
    return 1;
}

int create_nonblocking_socket(void) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        LOG_ERROR("socket create failed: %s", strerror(errno));
        return -1;
    }
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    int keepalive = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    return fd;
}

int64_t current_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int try_connect(ReconnectContext *ctx) {
    int ret = connect(ctx->fd, (struct sockaddr*)&ctx->target_addr, sizeof(ctx->target_addr));
    if (ret == 0) {
        ctx->retry_count = 0;
        return 0;
    }
    if (errno == EINPROGRESS) {
        return 1;
    }
    return -1;
}

int get_next_backoff_ms(ReconnectContext *ctx) {
    int backoff = (1 << ctx->retry_count) * 1000;
    if (backoff > ctx->max_retry_interval_ms || backoff <= 0) {
        backoff = ctx->max_retry_interval_ms;
    }
    ctx->retry_count++;
    return backoff;
}

PollHandler handlers[MAX_FDS];
int handler_count = 0;
volatile sig_atomic_t running = 1;

TimerEntry timers[MAX_TIMERS];
int timer_count = 0;

int unregister_timer(int timer_id) {
    if (timer_id < 0 || timer_id >= timer_count) return -1;
    // 将要删除的定时器与末尾交换，并减少数量
    timers[timer_id] = timers[timer_count - 1];
    timer_count--;
    return 0;
}

void event_loop(void) {
    struct pollfd pfds[MAX_FDS];
    while (running) {
        for (int i = 0; i < handler_count; i++) {
            pfds[i].fd = handlers[i].fd;
            pfds[i].events = handlers[i].events;
            pfds[i].revents = 0;
        }
        int ret = poll(pfds, handler_count, 100);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("poll error: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < handler_count; i++) {
            if (pfds[i].revents != 0) {
                handlers[i].cb(handlers[i].fd, pfds[i].revents, handlers[i].arg);
            }
        }
        int64_t now = current_time_ms();
        for (int i = 0; i < timer_count; i++) {
            if (now >= timers[i].next_trigger) {
                timers[i].cb(timers[i].arg);
                timers[i].next_trigger = now + timers[i].interval_ms;
            }
        }
    }
}

int register_handler(int fd, short events, event_callback cb, void *arg) {
    if (handler_count >= MAX_FDS) return -1;
    handlers[handler_count].fd = fd;
    handlers[handler_count].events = events;
    handlers[handler_count].cb = cb;
    handlers[handler_count].arg = arg;
    handler_count++;
    return 0;
}

void unregister_handler(int fd) {
    for (int i = 0; i < handler_count; i++) {
        if (handlers[i].fd == fd) {
            handlers[i] = handlers[handler_count - 1];
            handler_count--;
            return;
        }
    }
}

int register_timer(int interval_ms, timer_callback cb, void *arg) {
    if (timer_count >= MAX_TIMERS) return -1;
    timers[timer_count].interval_ms = interval_ms;
    timers[timer_count].next_trigger = current_time_ms() + interval_ms;
    timers[timer_count].cb = cb;
    timers[timer_count].arg = arg;
    timer_count++;
    return 0;
}

void signal_handler(int sig) {
    (void)sig;
    LOG_INFO("Received signal, shutting down...");
    running = 0;
}

void setup_signals(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
}