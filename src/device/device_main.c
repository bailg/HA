#include "device.h"
#include "common/network.h"
#include "common/logging.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

typedef struct {
    int fd;
    ConnectionBuffer *conn_buf;
    ProtocolReader reader;
    char host[64];
    int port;
    int reconnect_timer_id;
} DeviceBusLink;

static DeviceBusLink pri_link;
static DeviceBusLink sec_link;
static int connect_and_register(const char *host, int port, const char* device_id);
static void on_bus_role_change(int fd, short revents, void *arg);
static void device_try_reconnect(void *arg);

static void device_link_init(DeviceBusLink *link, int fd, const char *host, int port) {
    link->fd = fd;
    link->port = port;
    link->reconnect_timer_id = -1;
    strncpy(link->host, host, sizeof(link->host));
    if (fd >= 0) {
        link->conn_buf = conn_buf_create(fd, 4096);
        protocol_reader_init(&link->reader, link->conn_buf);
    } else {
        link->conn_buf = NULL;
    }
}

static void device_link_destroy(DeviceBusLink *link) {
    if (link->conn_buf) {
        conn_buf_destroy(link->conn_buf);
        link->conn_buf = NULL;
    }
    if (link->fd >= 0) {
        close(link->fd);
        link->fd = -1;
    }
    if (link->reconnect_timer_id < 0) {
        link->reconnect_timer_id = register_timer(2000, device_try_reconnect, link);
    }
}

static void device_try_reconnect(void *arg) {
    DeviceBusLink *link = (DeviceBusLink *)arg;
    link->reconnect_timer_id = -1;
    if (link->fd >= 0) return;

    int fd = connect_and_register(link->host, link->port, dev_ctx.device_id);
    if (fd >= 0) {
        link->fd = fd;
        link->conn_buf = conn_buf_create(fd, 4096);
        protocol_reader_init(&link->reader, link->conn_buf);
        register_handler(fd, POLLIN, on_bus_role_change, link);
    } else {
        link->reconnect_timer_id = register_timer(2000, device_try_reconnect, link);
    }
}

/* 定时向备总线查询同步状态 */
static void timer_query_sync(void *arg) {
    (void)arg;
    device_query_sync_status(sec_link.fd);
}

/* 交互式输入：从 stdin 读取一行并作为业务数据发送 */
static uint64_t interactive_seq = 0;
#define STDIN_BUF_SIZE 512
static char stdin_line_buf[STDIN_BUF_SIZE];
static int stdin_line_pos = 0;

static void on_stdin_input(int fd, short revents, void *arg) {
    (void)arg;
    if (!(revents & POLLIN)) return;
    char c;
    while (read(fd, &c, 1) == 1) {
        if (c == '\n' || c == '\r') {
            if (stdin_line_pos == 0) continue;
            stdin_line_buf[stdin_line_pos] = '\0';
            if (dev_ctx.current_role != ROLE_PRIMARY) {
                /* 仍然尝试发送，由总线裁决是否接受（故障转移后新 PRIMARY 可能刚接管道）*/
            }
            /* 优先使用 pri_link; 若断连则尝试 sec_link（故障转移后备总线接管）*/
            DeviceDataPacketMessage msg;
            memset(&msg, 0, sizeof(msg));
            msg.header.type = MSG_TYPE_DEVICE_DATA_PACKET;
            msg.header.length = sizeof(msg);
            msg.header.timestamp = current_time_ms();
            strncpy(msg.device_id, dev_ctx.device_id, DEVICE_ID_MAX_LEN);
            msg.message_id = ++interactive_seq;
            int plen = snprintf((char*)msg.payload, PAYLOAD_MAX_LEN, "%s", stdin_line_buf);
            msg.payload_size = (plen < 0) ? 0 : (uint32_t)(plen + 1);

            /* 先尝试 pri_link，失败（降级/断连）后自动 fallback 到 sec_link */
            bool sent = false;
            if (pri_link.fd >= 0) {
                sent = device_send_data(pri_link.fd, &msg);
            }
            if (!sent && sec_link.fd >= 0) {
                sent = device_send_data(sec_link.fd, &msg);
            }
            if (sent) {
                LOG_INFO("Sent: %s", stdin_line_buf);
            } else {
                LOG_WARN("Send failed: %s", stdin_line_buf);
            }
            stdin_line_pos = 0;
        } else if (stdin_line_pos < STDIN_BUF_SIZE - 1) {
            stdin_line_buf[stdin_line_pos++] = c;
        }
    }
    /* EAGAIN is expected on non-blocking stdin */
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_WARN("Stdin read error: %s", strerror(errno));
    }
}

/* 尝试连接并注册 */
static int connect_and_register(const char *host, int port, const char* device_id) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    /* 使用阻塞 socket 进行初始连接（避免非阻塞 connect 的 EINPROGRESS 问题） */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("Failed to create socket");
        return -1;
    }

    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        LOG_ERROR("Failed to connect to %s:%d: %s", host, port, strerror(errno));
        close(fd);
        return -1;
    }

    DeviceRegisterMessage reg;
    memset(&reg, 0, sizeof(reg));
    reg.header.type = MSG_TYPE_DEVICE_REGISTER;
    reg.header.length = sizeof(reg);
    reg.header.timestamp = current_time_ms();
    strncpy(reg.device_id, device_id, DEVICE_ID_MAX_LEN);

    DeviceRoleAssignMessage reply;
    if (device_send_register(fd, &reg, &reply)) {
        LOG_INFO("Registered with bus %s:%d, role %d", host, port, reply.role);

        /* 注册成功后切换到非阻塞模式，交给事件循环管理 */
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
        return fd;
    }
    close(fd);
    return -1;
}

/* 事件处理器：接收来自总线的 ROLE_CHANGE 消息 */
static void on_bus_role_change(int fd, short revents, void *arg) {
    DeviceBusLink *link = (DeviceBusLink *)arg;
    if (revents & (POLLERR | POLLHUP)) {
        LOG_WARN("Bus connection closed.");
        unregister_handler(fd);
        device_link_destroy(link);
        return;
    }
    if (revents & POLLIN) {
        if (!link->conn_buf) return;

        uint8_t temp_buf[256];
        ssize_t n = recv(fd, temp_buf, sizeof(temp_buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            LOG_WARN("Bus recv error.");
            unregister_handler(fd);
            device_link_destroy(link);
            return;
        }
        if (n == 0) {
            unregister_handler(fd);
            device_link_destroy(link);
            return;
        }

        size_t space = link->conn_buf->read_buf_size - link->conn_buf->read_buf_offset;
        if ((size_t)n > space) {
            LOG_WARN("Bus link buf full, dropping %zu bytes", (size_t)n - space);
            n = space;
        }
        memcpy(link->conn_buf->read_buf + link->conn_buf->read_buf_offset, temp_buf, n);
        link->conn_buf->read_buf_offset += n;

        uint8_t *msg = NULL;
        uint16_t len = 0;
        while (protocol_read_message(&link->reader, &msg, &len) == 1) {
            MessageHeader *hdr = (MessageHeader *)msg;
            uint16_t type = ntohs(hdr->type);
            if (type == MSG_TYPE_ROLE_CHANGE) {
                RoleChangeMessage *rc = (RoleChangeMessage *)msg;
                protocol_ntoh_role_change(rc);
                device_receive_role_change(fd, rc);
            } else if (type == MSG_TYPE_SYNC_OK) {
                SyncOkMessage *sok = (SyncOkMessage *)msg;
                protocol_ntoh_sync_ok(sok);
                LOG_DEBUG("SYNC_OK from secondary: synced=%d, log_id=%lu",
                          sok->synced, sok->last_committed_log_id);
                device_receive_sync_status(sok->synced, sok->last_committed_log_id);
            } else {
                LOG_WARN("Unknown message type %d from bus", type);
            }
            free(msg); msg = NULL;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) LOG_FATAL("Usage: %s <config>", argv[0]);
    
    Config cfg;
    config_load(&cfg, argv[1]);
    config_validate_required(&cfg, "device_id");
    config_validate_int_range(&cfg, "bus_primary_port", 1024, 65535, 5000);
    config_validate_int_range(&cfg, "bus_secondary_port", 1024, 65535, 5001);
    device_init(&cfg);
    setup_signals();
    signal(SIGPIPE, SIG_IGN);

    const char *pri_host = config_get(&cfg, "bus_primary_host");
    if (!pri_host) pri_host = "127.0.0.1";
    int pri_port = config_get_int(&cfg, "bus_primary_port", 5000);
    
    const char *sec_host = config_get(&cfg, "bus_secondary_host");
    if (!sec_host) sec_host = "127.0.0.1";
    int sec_port = config_get_int(&cfg, "bus_secondary_port", 5001);

    /* 同时连接主备总线并注册，保持双活长连接 */
    device_link_init(&pri_link, connect_and_register(pri_host, pri_port, dev_ctx.device_id), pri_host, pri_port);
    device_link_init(&sec_link, connect_and_register(sec_host, sec_port, dev_ctx.device_id), sec_host, sec_port);

    if (pri_link.fd < 0) {
        pri_link.reconnect_timer_id = register_timer(1000, device_try_reconnect, &pri_link);
    }
    if (sec_link.fd < 0) {
        sec_link.reconnect_timer_id = register_timer(1000, device_try_reconnect, &sec_link);
    }
    if (pri_link.fd < 0 && sec_link.fd < 0) {
        LOG_FATAL("Failed to connect to any bus");
    }

    /* 使用统一的事件循环监听两条链路 */
    if (pri_link.fd >= 0) {
        register_handler(pri_link.fd, POLLIN, on_bus_role_change, &pri_link);
    }
    if (sec_link.fd >= 0) {
        register_handler(sec_link.fd, POLLIN, on_bus_role_change, &sec_link);
    }

    register_timer(5000, timer_query_sync, NULL);

    /* 注册 stdin 交互式输入 */
    int stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (stdin_flags >= 0) fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);
    register_handler(STDIN_FILENO, POLLIN, on_stdin_input, NULL);
    LOG_INFO("Interactive mode ready — type a message and press Enter to send data");

    /* 进入统一的事件循环 */
    event_loop();

    device_link_destroy(&pri_link);
    device_link_destroy(&sec_link);
    return 0;
}