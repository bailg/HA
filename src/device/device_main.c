#include "device.h"
#include "common/network.h"
#include "common/logging.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <sys/select.h>
#include <stdlib.h>
#include <errno.h>

// 抽离出连接并注册的逻辑，成功返回保持连接的 FD，失败返回 -1
static int connect_and_register(const char *host, int port, const char* device_id) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        DeviceRegisterMessage reg;
        memset(&reg, 0, sizeof(reg));
        reg.header.type = MSG_TYPE_DEVICE_REGISTER;
        reg.header.length = sizeof(reg);
        reg.header.timestamp = current_time_ms();
        strncpy(reg.device_id, device_id, DEVICE_ID_MAX_LEN);

        DeviceRoleAssignMessage reply;
        if (device_send_register(fd, &reg, &reply)) {
            LOG_INFO("Registered with bus %s:%d, role %d", host, port, reply.role);
            return fd; 
        }
    }
    close(fd);
    return -1;
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

    // 【核心重构】真正同时连接主备总线并注册，保持双活长连接
    int pri_fd = connect_and_register(pri_host, pri_port, dev_ctx.device_id);
    int sec_fd = connect_and_register(sec_host, sec_port, dev_ctx.device_id);

    if (pri_fd < 0 && sec_fd < 0) {
        LOG_FATAL("Failed to connect to any bus");
    }

    // 使用标准 select 多路复用，同时监听主备两条链路
    int max_fd = (pri_fd > sec_fd ? pri_fd : sec_fd) + 1;
    
    while (running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        if (pri_fd >= 0) FD_SET(pri_fd, &read_fds);
        if (sec_fd >= 0) FD_SET(sec_fd, &read_fds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(max_fd, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ret > 0) {
            int fds[2] = {pri_fd, sec_fd};
            for (int i = 0; i < 2; i++) {
                int bus_fd = fds[i];
                if (bus_fd >= 0 && FD_ISSET(bus_fd, &read_fds)) {
                    RoleChangeMessage rc;
                    ssize_t n = recv(bus_fd, &rc, sizeof(rc), 0);
                    if (n == sizeof(rc)) {
                        protocol_ntoh_role_change(&rc);
                        device_receive_role_change(bus_fd, &rc);
                    } else if (n <= 0) {
                        // 主节点断开，仅清理 FD，依靠备节点链路继续存活
                        LOG_WARN("Bus connection closed.");
                        close(bus_fd);
                        if (i == 0) pri_fd = -1;
                        else sec_fd = -1;
                    }
                }
            }
        }
    }

    if (pri_fd >= 0) close(pri_fd);
    if (sec_fd >= 0) close(sec_fd);
    return 0;
}