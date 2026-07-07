#include "arbiter.h"
#include "common/network.h"
#include "common/logging.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>

#define MAX_BUS_CONNECTIONS 10

typedef struct {
    int fd;
    ConnectionBuffer *conn_buf;
    ProtocolReader reader;
    char node_id[NODE_ID_MAX_LEN]; 
} ArbiterConnection;

static int listen_fd = -1;
static ArbiterConnection connections[MAX_BUS_CONNECTIONS];
static int conn_count = 0;

static void on_bus_data(int fd, short revents, void *arg);
static ArbiterConnection* find_connection(int fd);
static ArbiterConnection* find_connection_by_node(const char *node_id);

static ArbiterConnection* find_connection(int fd) {
    for (int i = 0; i < conn_count; i++) {
        if (connections[i].fd == fd) return &connections[i];
    }
    return NULL;
}

static ArbiterConnection* find_connection_by_node(const char *node_id) {
    for (int i = 0; i < conn_count; i++) {
        if (strcmp(connections[i].node_id, node_id) == 0) return &connections[i];
    }
    return NULL;
}

static void on_bus_connection(int fd, short revents, void *arg) {
    (void)fd; (void)revents; (void)arg;
    if (revents & POLLIN) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if (client_fd >= 0) {
            if (conn_count >= MAX_BUS_CONNECTIONS) { close(client_fd); return; }
            
            ArbiterConnection *conn = &connections[conn_count];
            conn->fd = client_fd;
            conn->conn_buf = conn_buf_create(client_fd, 4096);
            protocol_reader_init(&conn->reader, conn->conn_buf);
            memset(conn->node_id, 0, NODE_ID_MAX_LEN);

            int flags = fcntl(client_fd, F_GETFL, 0);
            fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK);

            BusLoginMessage login_msg;
            ssize_t total = 0;
            uint8_t *login_buf = (uint8_t *)&login_msg;
            while (total < (ssize_t)sizeof(login_msg)) {
                ssize_t n = recv(client_fd, login_buf + total, sizeof(login_msg) - total, 0);
                if (n <= 0) break;
                total += n;
            }
            if (total == sizeof(login_msg)) {
                protocol_ntoh_bus_login(&login_msg);
                strncpy(conn->node_id, login_msg.node_id, NODE_ID_MAX_LEN); 
                
                LoginAckMessage ack;
                memset(&ack, 0, sizeof(ack));
                ack.header.type = MSG_TYPE_LOGIN_ACK;
                ack.header.length = sizeof(ack);
                ack.header.timestamp = current_time_ms();
                bool login_accepted = arbiter_login_bus(&login_msg, &ack);
                /* 先保存 epoch（主机序），再调用完整的 hton */
                uint32_t ack_epoch_host = ack.epoch;
                protocol_hton_login_ack(&ack);
                send(client_fd, &ack, sizeof(ack), 0);
                
                if (login_accepted) {
                    fcntl(client_fd, F_SETFL, flags);
                    register_handler(client_fd, POLLIN, on_bus_data, NULL);
                    conn_count++;
                } else {
                    /* 登录被拒绝：发送 DEGRADE_COMMAND 后再关闭连接 */
                    const char *old_pri = arbiter_get_old_primary();
                    if (old_pri && strcmp(old_pri, login_msg.node_id) == 0) {
                        arbiter_send_degrade_to_old_primary(client_fd, ack_epoch_host, old_pri);
                        arbiter_clear_old_primary();
                    }
                    conn_buf_destroy(conn->conn_buf);
                    close(client_fd);
                }
            } else {
                conn_buf_destroy(conn->conn_buf);
                close(client_fd);
            }
        }
    }
}

static void on_bus_data(int fd, short revents, void *arg) {
    (void)arg;
    if (revents & (POLLIN | POLLHUP | POLLERR)) {
        ArbiterConnection *conn = find_connection(fd);
        if (!conn) return;
        uint8_t temp_buf[1024];
        ssize_t n = recv(fd, temp_buf, sizeof(temp_buf), 0);
        
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            goto disconnect;
        } else if (n == 0) {
            goto disconnect;
        }

        size_t space = conn->conn_buf->read_buf_size - conn->conn_buf->read_buf_offset;
        if ((size_t)n > space) {
            LOG_WARN("Bus %s buffer full, dropping %zu bytes", conn->node_id, (size_t)n - space);
            n = space;
        }
        memcpy(conn->conn_buf->read_buf + conn->conn_buf->read_buf_offset, temp_buf, n);
        conn->conn_buf->read_buf_offset += n;

        uint8_t *msg = NULL;
        uint16_t len = 0;
        while (protocol_read_message(&conn->reader, &msg, &len) == 1) {
            MessageHeader *hdr = (MessageHeader *)msg;
            uint16_t type = ntohs(hdr->type);
            if (type == MSG_TYPE_HEARTBEAT) {
                HeartbeatMessage *hb = (HeartbeatMessage *)msg;
                protocol_ntoh_heartbeat(hb);
                arbiter_receive_heartbeat(hb);
                HeartbeatAckMessage ack;
                memset(&ack, 0, sizeof(ack));
                ack.header.type = MSG_TYPE_HEARTBEAT_ACK;
                ack.header.length = sizeof(ack);
                ack.header.timestamp = current_time_ms();
                ack.timestamp = current_time_ms();
                protocol_hton_heartbeat_ack(&ack);
                ssize_t sent = send(fd, &ack, sizeof(ack), 0);
                if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_WARN("Failed to send HEARTBEAT_ACK to %s", conn->node_id);
                }
            } else if (type == MSG_TYPE_FAILOVER_ACK) {
                LOG_INFO("Received FAILOVER_ACK from %s", conn->node_id);
            } else {
                LOG_WARN("Unknown message type %d from bus", type);
            }
            free(msg); msg = NULL;
        }
        return;
disconnect:
        LOG_WARN("Bus node disconnected, fd=%d", fd);
        unregister_handler(fd);
        conn_buf_destroy(conn->conn_buf);
        for (int i = 0; i < conn_count; i++) {
            if (connections[i].fd == fd) {
                connections[i] = connections[conn_count - 1];
                conn_count--;
                break;
            }
        }
        close(fd);
    }
}

static void timer_detect_failures(void *arg) {
    (void)arg;
    const char *target_id = NULL;
    uint32_t new_epoch = 0;
    
    if (arbiter_prepare_failover(&target_id, &new_epoch)) {
        ArbiterConnection *target_conn = find_connection_by_node(target_id);
        if (target_conn) {
            LOG_INFO("Sending FAILOVER_COMMAND to %s, epoch %u", target_id, new_epoch);
            
            FailoverCommandMessage cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.header.type = MSG_TYPE_FAILOVER_COMMAND;
            cmd.header.length = sizeof(cmd);
            cmd.header.timestamp = current_time_ms();
            strncpy(cmd.target_node_id, target_id, NODE_ID_MAX_LEN);
            cmd.promote_to = ROLE_PRIMARY;
            cmd.epoch = new_epoch;
            protocol_hton_failover_cmd(&cmd);
            
            // 【关键修复】对于关键的切换指令，临时切为阻塞发送，确保绝对到达内核缓冲区
            int flags = fcntl(target_conn->fd, F_GETFL, 0);
            fcntl(target_conn->fd, F_SETFL, flags & ~O_NONBLOCK);
            
            ssize_t sent = send(target_conn->fd, &cmd, sizeof(cmd), 0);
            
            fcntl(target_conn->fd, F_SETFL, flags); // 恢复非阻塞
            
            if (sent != sizeof(cmd)) {
                LOG_ERROR("Failed to send FAILOVER_COMMAND to %s (sent %zd)", target_id, sent);
            } else {
                arbiter_confirm_promotion(target_id, new_epoch);
                
                /* 【方向B+D】发送 DEGRADE_COMMAND 给旧主节点 */
                const char *old_primary = arbiter_get_old_primary();
                if (old_primary) {
                    ArbiterConnection *old_conn = find_connection_by_node(old_primary);
                    if (old_conn && old_conn->fd > 0) {
                        arbiter_send_degrade_to_old_primary(old_conn->fd, new_epoch, old_primary);
                    }
                    arbiter_clear_old_primary();
                }
            }
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) LOG_FATAL("Usage: %s <config_file>", argv[0]);
    Config cfg;
    config_load(&cfg, argv[1]);
    config_validate_int_range(&cfg, "listen_port", 1024, 65535, 4000);
    config_validate_int_range(&cfg, "heartbeat_timeout_ms", 100, 60000, 5000);
    arbiter_init(&cfg);
    setup_signals();
    signal(SIGPIPE, SIG_IGN);

    int port = config_get_int(&cfg, "listen_port", 4000);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    listen_fd = create_nonblocking_socket();
    if (listen_fd < 0) LOG_FATAL("Failed to create listen socket");
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_FATAL("Failed to bind: %s", strerror(errno));
    }
    if (listen(listen_fd, 10) < 0) {
        LOG_FATAL("Failed to listen: %s", strerror(errno));
    }

    register_handler(listen_fd, POLLIN, on_bus_connection, NULL);
    register_timer(1000, timer_detect_failures, NULL);
    LOG_INFO("Arbiter started on port %d", port);
    event_loop();
    close(listen_fd);
    return 0;
}