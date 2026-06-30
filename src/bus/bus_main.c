#include "bus.h"
#include "common/network.h"
#include "common/logging.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

typedef struct { 
    int fd; 
    char device_id[DEVICE_ID_MAX_LEN]; 
    NodeRole current_role;
} BusDeviceConn;

static int arbiter_fd = -1;
static int listen_fd = -1;
static int peer_fd = -1;
static Config local_bus_cfg;
static int reconnect_timer_id = -1;
static int heartbeat_timer_id = -1;

#ifndef IS_BUS_PRIMARY
static int peer_poll_timer_id = -1;
#endif

static BusDeviceConn devices[MAX_FDS];
static int device_count = 0;

// 【新增】为 Arbiter 连接引入 ProtocolReader，彻底解决半包和 EAGAIN 误判
static ConnectionBuffer *arbiter_conn_buf = NULL;
static ProtocolReader arbiter_reader;

static void send_heartbeat_to_arbiter(void *arg);
static void try_reconnect_arbiter(void *arg);
static void on_arbiter_connected(int fd, short revents, void *arg);
static void on_arbiter_data(int fd, short revents, void *arg);
static void on_new_connection(int fd, short revents, void *arg);
static void on_device_data(int fd, short revents, void *arg);
static void connect_to_peer(void);

#ifndef IS_BUS_PRIMARY
static void poll_peer_messages(void *arg);
#endif

static void stop_reconnect_timer() { if (reconnect_timer_id >= 0) { unregister_timer(reconnect_timer_id); reconnect_timer_id = -1; } }
static void start_heartbeat_timer() { if (heartbeat_timer_id < 0) heartbeat_timer_id = register_timer(config_get_int(&local_bus_cfg, "heartbeat_interval_ms", 2000), send_heartbeat_to_arbiter, NULL); }
static void stop_heartbeat_timer() { if (heartbeat_timer_id >= 0) { unregister_timer(heartbeat_timer_id); heartbeat_timer_id = -1; } }

static void handle_arbiter_disconnect() {
    LOG_WARN("Arbiter disconnected, starting reconnect.");
    on_arbiter_disconnect();
    if (arbiter_fd >= 0) { unregister_handler(arbiter_fd); close(arbiter_fd); arbiter_fd = -1; }
    
    // 清理 ProtocolReader 状态
    if (arbiter_conn_buf) {
        conn_buf_destroy(arbiter_conn_buf);
        arbiter_conn_buf = NULL;
    }
    
    stop_heartbeat_timer();
    if (reconnect_timer_id < 0) reconnect_timer_id = register_timer(3000, try_reconnect_arbiter, NULL);
}

void try_reconnect_arbiter(void *arg) {
    (void)arg;
    const char *host = config_get(&local_bus_cfg, "arbiter_host");
    if (!host) host = "127.0.0.1";
    int port = config_get_int(&local_bus_cfg, "arbiter_port", 4000);
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    arbiter_fd = create_nonblocking_socket();
    int ret = connect(arbiter_fd, (struct sockaddr*)&addr, sizeof(addr));
    
    if (ret == 0) {
        stop_reconnect_timer();
        on_arbiter_connected(arbiter_fd, POLLOUT, NULL);
    } else if (ret < 0 && errno == EINPROGRESS) {
        register_handler(arbiter_fd, POLLOUT, on_arbiter_connected, NULL);
    } else {
        close(arbiter_fd);
        arbiter_fd = -1;
    }
}

static void on_arbiter_connected(int fd, short revents, void *arg) {
    (void)arg;
    if (revents & POLLERR) {
        int err = 0; socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        unregister_handler(fd); close(fd); arbiter_fd = -1; return;
    }
    if (revents & POLLOUT) {
        stop_reconnect_timer();
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

        BusLoginMessage login;
        memset(&login, 0, sizeof(login));
        login.header.type = MSG_TYPE_BUS_LOGIN;
        login.header.length = sizeof(login);
        login.header.timestamp = current_time_ms();
        
        const char* login_node_id = config_get(&local_bus_cfg, "node_id");
        if (!login_node_id) login_node_id = "bus_unk";
        strncpy(login.node_id, login_node_id, NODE_ID_MAX_LEN);
        
        login.state = bus_get_state();
        login.role = (bus_get_state() == NODE_STATE_PRIMARY) ? ROLE_PRIMARY : ROLE_SECONDARY;
        login.epoch = bus_get_epoch();
        login.last_committed_log_id = 0; 
        protocol_hton_bus_login(&login);
        send(fd, &login, sizeof(login), 0);

        LoginAckMessage ack;
        ssize_t n = recv(fd, &ack, sizeof(ack), MSG_WAITALL);
        if (n == sizeof(ack)) {
            protocol_ntoh_header(&ack.header);
            if (ack.accepted) {
                LOG_INFO("Re-login to arbiter successful.");
                fcntl(fd, F_SETFL, flags);
                
                // 初始化 Arbiter 连接的缓冲区解析器
                arbiter_conn_buf = conn_buf_create(fd, 4096);
                protocol_reader_init(&arbiter_reader, arbiter_conn_buf);
                
                unregister_handler(fd);
                register_handler(fd, POLLIN, on_arbiter_data, NULL);
                start_heartbeat_timer();
                connect_to_peer(); 
                return;
            }
        }
        fcntl(fd, F_SETFL, flags);
        unregister_handler(fd); close(fd); arbiter_fd = -1;
        reconnect_timer_id = register_timer(3000, try_reconnect_arbiter, NULL);
    }
}

// 【核心重构】使用 ProtocolReader 解决半包和 EAGAIN 幽灵断连
static void on_arbiter_data(int fd, short revents, void *arg) {
    (void)arg;
    if (revents & (POLLERR | POLLHUP)) { 
        handle_arbiter_disconnect(); 
        return; 
    }
    if (revents & POLLIN) {
        if (!arbiter_conn_buf) return;

        uint8_t temp_buf[256];
        ssize_t n = recv(fd, temp_buf, sizeof(temp_buf), 0);
        
        // 【关键修复】如果是 EAGAIN，说明只是暂时没数据，绝对不能断开连接！
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return; 
            }
            LOG_ERROR("Arbiter recv error: %s", strerror(errno));
            handle_arbiter_disconnect();
            return;
        }
        
        if (n == 0) {
            handle_arbiter_disconnect();
            return;
        }

        // 追加到协议解析器
        size_t space = arbiter_conn_buf->read_buf_size - arbiter_conn_buf->read_buf_offset;
        if ((size_t)n > space) n = space;
        memcpy(arbiter_conn_buf->read_buf + arbiter_conn_buf->read_buf_offset, temp_buf, n);
        arbiter_conn_buf->read_buf_offset += n;

        // 循环解析完整消息
        uint8_t *msg = NULL;
        uint16_t len = 0;
        while (protocol_read_message(&arbiter_reader, &msg, &len) == 1) {
            MessageHeader *hdr = (MessageHeader *)msg;
            uint16_t type = ntohs(hdr->type);
            
            if (type == MSG_TYPE_FAILOVER_COMMAND) {
                FailoverCommandMessage *cmd = (FailoverCommandMessage *)msg;
                protocol_ntoh_failover_cmd(cmd);
                
                FailoverAckMessage ack;
                memset(&ack, 0, sizeof(ack));
                ack.header.type = MSG_TYPE_FAILOVER_ACK;
                ack.header.length = sizeof(ack);
                ack.header.timestamp = current_time_ms();
                
                const char* ack_node_id = config_get(&local_bus_cfg, "node_id");
                if (!ack_node_id) ack_node_id = "unk";
                strncpy(ack.node_id, ack_node_id, NODE_ID_MAX_LEN);
                
                ack.accepted = 1;
                ack.epoch = cmd->epoch;
                protocol_hton_failover_ack(&ack);
                send(fd, &ack, sizeof(ack), 0);
                
                bus_apply_failover(cmd->epoch);
                
                if (bus_get_state() == NODE_STATE_PRIMARY) {
                    for (int i = 0; i < device_count; i++) {
                        if (devices[i].current_role == ROLE_SECONDARY) {
                            devices[i].current_role = ROLE_PRIMARY;
                            RoleChangeMessage rc;
                            memset(&rc, 0, sizeof(rc));
                            rc.header.type = MSG_TYPE_ROLE_CHANGE;
                            rc.header.length = sizeof(rc);
                            rc.header.timestamp = current_time_ms();
                            strncpy(rc.device_id, devices[i].device_id, DEVICE_ID_MAX_LEN);
                            rc.role = ROLE_PRIMARY;
                            rc.epoch = cmd->epoch;
                            protocol_hton_role_change(&rc);
                            send(devices[i].fd, &rc, sizeof(rc), 0);
                            LOG_INFO("Promoted device %s to PRIMARY", devices[i].device_id);
                            break; 
                        }
                    }
                } else if (bus_get_state() == NODE_STATE_SECONDARY) {
                    if (peer_fd >= 0) { close(peer_fd); peer_fd = -1; }
                }
            } else if (type == MSG_TYPE_DEGRADE_COMMAND) {
                DegradeCommandMessage *dcmd = (DegradeCommandMessage *)msg;
                protocol_ntoh_degrade_cmd(dcmd);
                LOG_INFO("Received DEGRADE_COMMAND, epoch=%u, demoting to %d",
                         dcmd->epoch, dcmd->demote_to);
                bus_apply_degrade(dcmd->epoch);
            }
            free(msg); msg = NULL;
        }
    }
}

static void connect_to_peer(void) {
    if (peer_fd >= 0) return;
    
    #ifndef IS_BUS_PRIMARY
    const char *pri_host = config_get(&local_bus_cfg, "bus_primary_host");
    if (!pri_host) pri_host = "127.0.0.1";
    int pri_port = config_get_int(&local_bus_cfg, "bus_primary_port", 5000);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(pri_port);
    inet_pton(AF_INET, pri_host, &addr.sin_addr);

    peer_fd = create_nonblocking_socket();
    int flags = fcntl(peer_fd, F_GETFL, 0);
    fcntl(peer_fd, F_SETFL, flags & ~O_NONBLOCK); 

    if (connect(peer_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        BusLoginMessage login;
        memset(&login, 0, sizeof(login));
        login.header.type = MSG_TYPE_BUS_LOGIN;
        login.header.length = sizeof(login);
        login.header.timestamp = current_time_ms();
        
        const char* peer_node_id = config_get(&local_bus_cfg, "node_id");
        if (!peer_node_id) peer_node_id = "bus_secondary";
        strncpy(login.node_id, peer_node_id, NODE_ID_MAX_LEN);
        
        login.state = NODE_STATE_SECONDARY;
        login.role = ROLE_SECONDARY;
        login.epoch = bus_get_epoch();
        protocol_hton_bus_login(&login);
        send(peer_fd, &login, sizeof(login), 0);
        
        peer_poll_timer_id = register_timer(50, poll_peer_messages, NULL);
        LOG_INFO("Connected to primary bus for data sync.");
    } else {
        LOG_ERROR("Failed to connect to primary bus.");
        close(peer_fd);
        peer_fd = -1;
    }
    #endif
}

#ifndef IS_BUS_PRIMARY
static void poll_peer_messages(void *arg) {
    (void)arg;
    if (peer_fd < 0) return;

    if (bus_get_state() == NODE_STATE_SECONDARY) {
        BusSyncEntryMessage sync_msg;
        ssize_t n = recv(peer_fd, &sync_msg, sizeof(sync_msg), MSG_DONTWAIT);
        if (n == sizeof(sync_msg)) {
            protocol_ntoh_sync_entry(&sync_msg);
            bus_apply_sync_entry(sync_msg.log_id, sync_msg.payload, sync_msg.payload_size);
            
            BusAckMessage ack;
            memset(&ack, 0, sizeof(ack));
            ack.header.type = MSG_TYPE_BUS_ACK;
            ack.header.length = sizeof(ack);
            ack.header.timestamp = current_time_ms();
            ack.log_id = sync_msg.log_id;
            ack.status = 1;
            protocol_hton_bus_ack(&ack);
            send(peer_fd, &ack, sizeof(ack), 0);
        }
    }
}
#endif

static void on_new_connection(int fd, short revents, void *arg) {
    (void)fd; (void)revents; (void)arg;
    if (revents & POLLIN) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if (client_fd >= 0) {
            int flags = fcntl(client_fd, F_GETFL, 0);
            fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK);

            MessageHeader hdr;
            ssize_t n = recv(client_fd, &hdr, sizeof(hdr), MSG_WAITALL);
            if (n == sizeof(hdr)) {
                uint16_t type = ntohs(hdr.type);
                
                if (type == MSG_TYPE_BUS_LOGIN) {
                    if (peer_fd == -1) {
                        uint8_t trash[sizeof(BusLoginMessage) - sizeof(MessageHeader)];
                        recv(client_fd, trash, sizeof(trash), MSG_WAITALL);
                        peer_fd = client_fd;
                        LOG_INFO("Secondary bus connected for data sync.");
                    } else {
                        close(client_fd);
                    }
                    return;
                } else if (type == MSG_TYPE_DEVICE_REGISTER) {
                    DeviceRegisterMessage reg;
                    memcpy(&reg, &hdr, sizeof(hdr));
                    recv(client_fd, ((uint8_t*)&reg) + sizeof(hdr), sizeof(reg) - sizeof(hdr), MSG_WAITALL);
                    
                    protocol_ntoh_device_reg(&reg);
                    DeviceRoleAssignMessage reply;
                    bus_register_device(&reg, &reply);
                    protocol_hton_device_role_assign(&reply);
                    send(client_fd, &reply, sizeof(reply), 0);
                    
                    fcntl(client_fd, F_SETFL, flags);
                    if (device_count < MAX_FDS) {
                        strncpy(devices[device_count].device_id, reg.device_id, DEVICE_ID_MAX_LEN);
                        devices[device_count].fd = client_fd;
                        devices[device_count].current_role = reply.role;
                        device_count++;
                        register_handler(client_fd, POLLIN, on_device_data, NULL);
                    } else {
                        close(client_fd);
                    }
                    return;
                }
            }
            fcntl(client_fd, F_SETFL, flags);
            close(client_fd);
        }
    }
}

static void on_device_data(int fd, short revents, void *arg) {
    (void)arg;
    if (revents & (POLLIN | POLLHUP | POLLERR)) {
        if (revents & (POLLHUP | POLLERR)) {
            for(int i=0; i<device_count; i++) {
                if(devices[i].fd == fd) { devices[i] = devices[device_count-1]; device_count--; break; }
            }
            unregister_handler(fd); close(fd); return;
        }
        
        DeviceDataPacketMessage msg;
        uint8_t temp_buf[sizeof(DeviceDataPacketMessage)];
        ssize_t n = recv(fd, temp_buf, sizeof(temp_buf), 0);
        if (n == sizeof(DeviceDataPacketMessage)) {
            memcpy(&msg, temp_buf, sizeof(msg));
            protocol_ntoh_device_data(&msg);
            WriteResponseMessage resp;
            process_device_packet(&msg, &resp, peer_fd);
            send(fd, &resp, sizeof(resp), 0);
        } else if (n <= 0) {
            for(int i=0; i<device_count; i++) {
                if(devices[i].fd == fd) { devices[i] = devices[device_count-1]; device_count--; break; }
            }
            unregister_handler(fd); close(fd);
        }
    }
}

void send_heartbeat_to_arbiter(void *arg) {
    (void)arg;
    if (arbiter_fd < 0) return;
    HeartbeatMessage hb;
    memset(&hb, 0, sizeof(hb));
    hb.header.type = MSG_TYPE_HEARTBEAT;
    hb.header.length = sizeof(hb);
    hb.header.timestamp = current_time_ms();
    
    const char* hb_node_id = config_get(&local_bus_cfg, "node_id");
    if (!hb_node_id) hb_node_id = "bus_unk";
    strncpy(hb.node_id, hb_node_id, NODE_ID_MAX_LEN);
    
    hb.epoch = bus_get_epoch();
    protocol_hton_heartbeat(&hb);
    if (send(arbiter_fd, &hb, sizeof(hb), 0) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        handle_arbiter_disconnect();
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) LOG_FATAL("Usage: %s <config>", argv[0]);
    config_load(&local_bus_cfg, argv[1]);
    
#ifdef IS_BUS_PRIMARY
    bus_init(&local_bus_cfg, NODE_STATE_PRIMARY);
#else
    bus_init(&local_bus_cfg, NODE_STATE_SECONDARY);
#endif

    setup_signals();
    signal(SIGPIPE, SIG_IGN);
    try_reconnect_arbiter(NULL);

    int port = config_get_int(&local_bus_cfg, "listen_port", 5000);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    listen_fd = create_nonblocking_socket();
    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 10);
    register_handler(listen_fd, POLLIN, on_new_connection, NULL);

    event_loop();
    if (arbiter_fd >= 0) close(arbiter_fd);
    if (listen_fd >= 0) close(listen_fd);
    if (peer_fd >= 0) close(peer_fd);
    return 0;
}