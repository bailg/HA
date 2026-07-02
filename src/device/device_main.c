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

typedef struct {
    int fd;
    ConnectionBuffer *conn_buf;
    ProtocolReader reader;
} DeviceBusLink;

static DeviceBusLink pri_link;
static DeviceBusLink sec_link;

static void device_link_init(DeviceBusLink *link, int fd) {
    link->fd = fd;
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
}

/* 定时向备总线查询同步状态 */
static void timer_query_sync(void *arg) {
    (void)arg;
    device_query_sync_status(sec_link.fd);
}

/* 主设备定时发送业务数据（测试数据面直通）*/
static uint64_t test_data_seq = 0;
static void timer_send_test_data(void *arg) {
    (void)arg;
    if (dev_ctx.current_role != ROLE_PRIMARY) return;
    if (pri_link.fd < 0) return;
    DeviceDataPacketMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.type = MSG_TYPE_DEVICE_DATA_PACKET;
    msg.header.length = sizeof(msg);
    msg.header.timestamp = current_time_ms();
    strncpy(msg.device_id, dev_ctx.device_id, DEVICE_ID_MAX_LEN);
    msg.message_id = ++test_data_seq;
    int plen = snprintf((char*)msg.payload, PAYLOAD_MAX_LEN,
                        "test_data_%lu", test_data_seq);
    msg.payload_size = (plen < 0) ? 0 : (uint32_t)(plen + 1);
    if (device_send_data(pri_link.fd, &msg)) {
        LOG_INFO("Data plane: sent test_data_%lu OK", test_data_seq);
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
                LOG_INFO("SYNC_OK from secondary: synced=%d, log_id=%lu",
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
    device_link_init(&pri_link, connect_and_register(pri_host, pri_port, dev_ctx.device_id));
    device_link_init(&sec_link, connect_and_register(sec_host, sec_port, dev_ctx.device_id));

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
    register_timer(3000, timer_send_test_data, NULL);

    /* 进入统一的事件循环 */
    event_loop();

    device_link_destroy(&pri_link);
    device_link_destroy(&sec_link);
    return 0;
}