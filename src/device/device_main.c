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
    RoleChangeMessage pending_rc;
    bool has_pending;
} DeviceBusLink;

static DeviceBusLink pri_link;
static DeviceBusLink sec_link;

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
        close(fd);
        link->fd = -1;
        return;
    }
    if (revents & POLLIN) {
        RoleChangeMessage rc;
        ssize_t n = recv(fd, &rc, sizeof(rc), MSG_DONTWAIT);
        if (n == sizeof(rc)) {
            protocol_ntoh_role_change(&rc);
            device_receive_role_change(fd, &rc);
        } else if (n <= 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_WARN("Bus connection closed.");
            unregister_handler(fd);
            close(fd);
            link->fd = -1;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) LOG_FATAL("Usage: %s <config>", argv[0]);
    
    Config cfg;
    config_load(&cfg, argv[1]);
    device_init(&cfg);
    setup_signals();

    const char *pri_host = config_get(&cfg, "bus_primary_host");
    if (!pri_host) pri_host = "127.0.0.1";
    int pri_port = config_get_int(&cfg, "bus_primary_port", 5000);
    
    const char *sec_host = config_get(&cfg, "bus_secondary_host");
    if (!sec_host) sec_host = "127.0.0.1";
    int sec_port = config_get_int(&cfg, "bus_secondary_port", 5001);

    /* 同时连接主备总线并注册，保持双活长连接 */
    pri_link.fd = connect_and_register(pri_host, pri_port, dev_ctx.device_id);
    sec_link.fd = connect_and_register(sec_host, sec_port, dev_ctx.device_id);
    pri_link.has_pending = false;
    sec_link.has_pending = false;

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

    /* 进入统一的事件循环（替代原来的独立 select 循环） */
    event_loop();

    if (pri_link.fd >= 0) close(pri_link.fd);
    if (sec_link.fd >= 0) close(sec_link.fd);
    return 0;
}