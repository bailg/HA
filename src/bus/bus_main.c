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
    ConnectionBuffer *conn_buf;
    ProtocolReader reader;
    char device_id[DEVICE_ID_MAX_LEN];
    NodeRole current_role;
} BusDeviceConn;

static int arbiter_fd = -1;
static int listen_fd = -1;
static int peer_fd = -1;
static Config local_bus_cfg;
static int reconnect_timer_id = -1;
static int heartbeat_timer_id = -1;
static ReconnectContext arbiter_reconnect;

#ifndef IS_BUS_PRIMARY
static int peer_poll_timer_id = -1;
#endif

static BusDeviceConn devices[MAX_FDS];
static int device_count = 0;

static ConnectionBuffer *arbiter_conn_buf = NULL;
static ProtocolReader arbiter_reader;

#ifndef IS_BUS_PRIMARY
static ConnectionBuffer *peer_conn_buf = NULL;
static ProtocolReader peer_reader;
#endif

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

static void close_peer_connection(void) {
#ifndef IS_BUS_PRIMARY
    if (peer_conn_buf) {
        conn_buf_destroy(peer_conn_buf);
        peer_conn_buf = NULL;
    }
#endif
    if (peer_fd >= 0) {
        close(peer_fd);
        peer_fd = -1;
    }
#ifndef IS_BUS_PRIMARY
    if (peer_poll_timer_id >= 0) {
        unregister_timer(peer_poll_timer_id);
        peer_poll_timer_id = -1;
    }
#endif
}

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
    if (reconnect_timer_id < 0) {
        int backoff = get_next_backoff_ms(&arbiter_reconnect);
        reconnect_timer_id = register_timer(backoff, try_reconnect_arbiter, NULL);
        LOG_INFO("Reconnect arbiter in %d ms (retry=%d)", backoff, arbiter_reconnect.retry_count);
    }
}

void try_reconnect_arbiter(void *arg) {
    (void)arg;
    const char *host = config_get(&local_bus_cfg, "arbiter_host");
    if (!host) host = "127.0.0.1";
    int port = config_get_int(&local_bus_cfg, "arbiter_port", 4000);
    
    arbiter_fd = create_nonblocking_socket();
    arbiter_reconnect.fd = arbiter_fd;
    arbiter_reconnect.target_addr.sin_family = AF_INET;
    arbiter_reconnect.target_addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &arbiter_reconnect.target_addr.sin_addr);

    int ret = try_connect(&arbiter_reconnect);
    
    if (ret == 0) {
        arbiter_reconnect.retry_count = 0;
        stop_reconnect_timer();
        on_arbiter_connected(arbiter_fd, POLLOUT, NULL);
    } else if (ret == 1) {
        arbiter_reconnect.retry_count = 0;
        stop_reconnect_timer();
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
        int backoff = get_next_backoff_ms(&arbiter_reconnect);
        LOG_INFO("Reconnect arbiter in %d ms (retry=%d)", backoff, arbiter_reconnect.retry_count);
        reconnect_timer_id = register_timer(backoff, try_reconnect_arbiter, NULL);
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

        size_t space = arbiter_conn_buf->read_buf_size - arbiter_conn_buf->read_buf_offset;
        if ((size_t)n > space) {
            LOG_WARN("Arbiter buf full, dropping %zu bytes", (size_t)n - space);
            n = space;
        }
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
                        if (devices[i].current_role == ROLE_PRIMARY) {
                            devices[i].current_role = ROLE_SECONDARY;
                            RoleChangeMessage rc;
                            memset(&rc, 0, sizeof(rc));
                            rc.header.type = MSG_TYPE_ROLE_CHANGE;
                            rc.header.length = sizeof(rc);
                            rc.header.timestamp = current_time_ms();
                            strncpy(rc.device_id, devices[i].device_id, DEVICE_ID_MAX_LEN);
                            rc.role = ROLE_SECONDARY;
                            rc.epoch = cmd->epoch;
                            protocol_hton_role_change(&rc);
                            send(devices[i].fd, &rc, sizeof(rc), 0);
                            LOG_INFO("Demoted device %s to SECONDARY", devices[i].device_id);
                        }
                    }
                    if (device_count > 0) {
                        int promote_idx = device_count - 1;
                        devices[promote_idx].current_role = ROLE_PRIMARY;
                        RoleChangeMessage rc;
                        memset(&rc, 0, sizeof(rc));
                        rc.header.type = MSG_TYPE_ROLE_CHANGE;
                        rc.header.length = sizeof(rc);
                        rc.header.timestamp = current_time_ms();
                        strncpy(rc.device_id, devices[promote_idx].device_id, DEVICE_ID_MAX_LEN);
                        rc.role = ROLE_PRIMARY;
                        rc.epoch = cmd->epoch;
                        protocol_hton_role_change(&rc);
                        send(devices[promote_idx].fd, &rc, sizeof(rc), 0);
                        LOG_INFO("Promoted device %s to PRIMARY", devices[promote_idx].device_id);
                    }
                } else if (bus_get_state() == NODE_STATE_SECONDARY) {
                    close_peer_connection();
                }
            } else if (type == MSG_TYPE_DEGRADE_COMMAND) {
                DegradeCommandMessage *dcmd = (DegradeCommandMessage *)msg;
                protocol_ntoh_degrade_cmd(dcmd);
                LOG_INFO("Received DEGRADE_COMMAND, epoch=%u, demoting to %d",
                         dcmd->epoch, dcmd->demote_to);
                bus_apply_degrade(dcmd->epoch);
            } else if (type == MSG_TYPE_HEARTBEAT_ACK) {
                HeartbeatAckMessage *ack = (HeartbeatAckMessage *)msg;
                protocol_ntoh_heartbeat_ack(ack);
                LOG_DEBUG("Received HEARTBEAT_ACK, ts=%lu", ack->timestamp);
                bus_receive_heartbeat_ack();
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
        
        /* 发送全量同步请求 */
        SyncRequestMessage sync_req;
        memset(&sync_req, 0, sizeof(sync_req));
        sync_req.header.type = MSG_TYPE_SYNC_REQUEST;
        sync_req.header.length = sizeof(sync_req);
        sync_req.header.timestamp = current_time_ms();
        const char* peer_node_id2 = config_get(&local_bus_cfg, "node_id");
        if (!peer_node_id2) peer_node_id2 = "bus_secondary";
        strncpy(sync_req.node_id, peer_node_id2, NODE_ID_MAX_LEN);
        protocol_hton_sync_request(&sync_req);
        send(peer_fd, &sync_req, sizeof(sync_req), 0);
        
        peer_conn_buf = conn_buf_create(peer_fd, 4096);
        protocol_reader_init(&peer_reader, peer_conn_buf);
        peer_poll_timer_id = register_timer(50, poll_peer_messages, NULL);
        LOG_INFO("Connected to primary bus for data sync.");
    } else {
        LOG_ERROR("Failed to connect to primary bus.");
        close_peer_connection();
    }
    #endif
}

#ifndef IS_BUS_PRIMARY
static void poll_peer_messages(void *arg) {
    (void)arg;
    if (peer_fd < 0 || !peer_conn_buf) return;

    if (bus_get_state() == NODE_STATE_SECONDARY) {
        uint8_t temp_buf[512];
        ssize_t n = recv(peer_fd, temp_buf, sizeof(temp_buf), MSG_DONTWAIT);
        if (n > 0) {
            size_t space = peer_conn_buf->read_buf_size - peer_conn_buf->read_buf_offset;
            if ((size_t)n > space) {
                LOG_WARN("Peer sync buf full, dropping %zu bytes", (size_t)n - space);
                n = space;
            }
            memcpy(peer_conn_buf->read_buf + peer_conn_buf->read_buf_offset, temp_buf, n);
            peer_conn_buf->read_buf_offset += n;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR("Peer recv error: %s", strerror(errno));
            return;
        } else if (n == 0) {
            return;
        }

        uint8_t *msg = NULL;
        uint16_t len = 0;
        while (protocol_read_message(&peer_reader, &msg, &len) == 1) {
            MessageHeader *hdr = (MessageHeader *)msg;
            uint16_t type = ntohs(hdr->type);
            if (type == MSG_TYPE_BUS_SYNC_ENTRY) {
                BusSyncEntryMessage *sync_msg = (BusSyncEntryMessage *)msg;
                protocol_ntoh_sync_entry(sync_msg);
                bus_apply_sync_entry(sync_msg->log_id, sync_msg->payload, sync_msg->payload_size);

                BusAckMessage ack;
                memset(&ack, 0, sizeof(ack));
                ack.header.type = MSG_TYPE_BUS_ACK;
                ack.header.length = sizeof(ack);
                ack.header.timestamp = current_time_ms();
                ack.log_id = sync_msg->log_id;
                ack.status = 1;
                protocol_hton_bus_ack(&ack);
                send(peer_fd, &ack, sizeof(ack), 0);
            } else if (type == MSG_TYPE_SYNC_OK) {
                SyncOkMessage *sok = (SyncOkMessage *)msg;
                protocol_ntoh_sync_ok(sok);
                LOG_INFO("Received SYNC_OK from primary, synced=%d, log_id=%lu",
                         sok->synced, sok->last_committed_log_id);
                if (sok->synced) {
                    bus_set_last_committed_log_id(sok->last_committed_log_id);
                }
            }
            free(msg); msg = NULL;
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
                        BusLoginMessage peer_login;
                        memcpy(&peer_login, &hdr, sizeof(hdr));
                        ssize_t rest = recv(client_fd, ((uint8_t*)&peer_login) + sizeof(hdr),
                                            sizeof(peer_login) - sizeof(hdr), MSG_WAITALL);
                        if (rest == sizeof(peer_login) - sizeof(hdr)) {
                            protocol_ntoh_bus_login(&peer_login);
                            if (peer_login.state != NODE_STATE_SECONDARY) {
                                LOG_WARN("Rejected peer login from %s (state=%d)", peer_login.node_id, peer_login.state);
                                fcntl(client_fd, F_SETFL, flags);
                                close(client_fd);
                                return;
                            }
                            peer_fd = client_fd;
                            LOG_INFO("Secondary bus %s connected for data sync.", peer_login.node_id);
                            /* 尝试接收 SYNC_REQUEST（非阻塞） */
                            uint8_t sync_buf[sizeof(SyncRequestMessage)];
                            ssize_t sr = recv(client_fd, sync_buf, sizeof(sync_buf), MSG_DONTWAIT);
                            if (sr >= (ssize_t)sizeof(MessageHeader)) {
                                MessageHeader *sh = (MessageHeader *)sync_buf;
                                if (ntohs(sh->type) == MSG_TYPE_SYNC_REQUEST) {
                                    SyncRequestMessage *srm = (SyncRequestMessage *)sync_buf;
                                    protocol_ntoh_sync_request(srm);
                                    LOG_INFO("Received SYNC_REQUEST from %s, sending SYNC_OK", srm->node_id);
                                    SyncOkMessage sok;
                                    memset(&sok, 0, sizeof(sok));
                                    sok.header.type = MSG_TYPE_SYNC_OK;
                                    sok.header.length = sizeof(sok);
                                    sok.header.timestamp = current_time_ms();
                                    const char *my_id = config_get(&local_bus_cfg, "node_id");
                                    strncpy(sok.node_id, my_id ? my_id : "bus_primary", NODE_ID_MAX_LEN);
                                    sok.synced = 1;
                                    sok.last_committed_log_id = bus_get_last_committed_log_id();
                                    protocol_hton_sync_ok(&sok);
                                    send(client_fd, &sok, sizeof(sok), 0);
                                }
                            }
                        } else {
                            close(client_fd);
                            return;
                        }
                    } else {
                        close(client_fd);
                    }
                    return;
                } else if (type == MSG_TYPE_DEVICE_REGISTER) {
                    DeviceRegisterMessage reg;
                    memcpy(&reg, &hdr, sizeof(hdr));
                    ssize_t rest = recv(client_fd, ((uint8_t*)&reg) + sizeof(hdr), sizeof(reg) - sizeof(hdr), 0);
                    if (rest != sizeof(reg) - sizeof(hdr)) { close(client_fd); return; }
                    
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
                        devices[device_count].conn_buf = conn_buf_create(client_fd, 4096);
                        protocol_reader_init(&devices[device_count].reader, devices[device_count].conn_buf);
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
        int idx = -1;
        for (int i = 0; i < device_count; i++) {
            if (devices[i].fd == fd) { idx = i; break; }
        }
        if (idx < 0) return;

        if (revents & (POLLHUP | POLLERR)) {
            if (devices[idx].conn_buf) conn_buf_destroy(devices[idx].conn_buf);
            devices[idx] = devices[device_count - 1];
            device_count--;
            unregister_handler(fd);
            close(fd);
            return;
        }

        if (!devices[idx].conn_buf) return;

        uint8_t temp_buf[256];
        ssize_t n = recv(fd, temp_buf, sizeof(temp_buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (devices[idx].conn_buf) conn_buf_destroy(devices[idx].conn_buf);
            devices[idx] = devices[device_count - 1];
            device_count--;
            unregister_handler(fd);
            close(fd);
            return;
        }
        if (n == 0) {
            if (devices[idx].conn_buf) conn_buf_destroy(devices[idx].conn_buf);
            devices[idx] = devices[device_count - 1];
            device_count--;
            unregister_handler(fd);
            close(fd);
            return;
        }

        ConnectionBuffer *cb = devices[idx].conn_buf;
        size_t space = cb->read_buf_size - cb->read_buf_offset;
        if ((size_t)n > space) {
            LOG_WARN("Device %s buf full, dropping %zu bytes", devices[idx].device_id, (size_t)n - space);
            n = space;
        }
        memcpy(cb->read_buf + cb->read_buf_offset, temp_buf, n);
        cb->read_buf_offset += n;

        uint8_t *msg = NULL;
        uint16_t len = 0;
        while (protocol_read_message(&devices[idx].reader, &msg, &len) == 1) {
            MessageHeader *hdr = (MessageHeader *)msg;
            uint16_t type = ntohs(hdr->type);
            if (type == MSG_TYPE_DEVICE_DATA_PACKET) {
                DeviceDataPacketMessage *ddp = (DeviceDataPacketMessage *)msg;
                protocol_ntoh_device_data(ddp);
                WriteResponseMessage resp;
                process_device_packet(ddp, &resp, peer_fd);
                send(fd, &resp, sizeof(resp), 0);
            } else if (type == MSG_TYPE_CHECK_SYNC_STATUS) {
                CheckSyncStatusMessage *csm = (CheckSyncStatusMessage *)msg;
                protocol_ntoh_header(&csm->header);
                LOG_INFO("Received CHECK_SYNC_STATUS from %s", csm->node_id);
                SyncOkMessage sok;
                memset(&sok, 0, sizeof(sok));
                sok.header.type = MSG_TYPE_SYNC_OK;
                sok.header.length = sizeof(sok);
                sok.header.timestamp = current_time_ms();
                const char *my_id = config_get(&local_bus_cfg, "node_id");
                strncpy(sok.node_id, my_id ? my_id : "bus_secondary", NODE_ID_MAX_LEN);
                sok.synced = (bus_get_last_committed_log_id() > 0) ? 1 : 0;
                sok.last_committed_log_id = bus_get_last_committed_log_id();
                protocol_hton_sync_ok(&sok);
                send(fd, &sok, sizeof(sok), 0);
            } else {
                LOG_WARN("Unknown message type %d from device fd=%d", type, fd);
            }
            free(msg);
            msg = NULL;
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
    config_validate_required(&local_bus_cfg, "arbiter_host");
    config_validate_required(&local_bus_cfg, "node_id");
    config_validate_int_range(&local_bus_cfg, "arbiter_port", 1024, 65535, 4000);
    config_validate_int_range(&local_bus_cfg, "listen_port", 1024, 65535, 5000);
    config_validate_int_range(&local_bus_cfg, "heartbeat_interval_ms", 100, 60000, 2000);
    
#ifdef IS_BUS_PRIMARY
    bus_init(&local_bus_cfg, NODE_STATE_PRIMARY);
#else
    bus_init(&local_bus_cfg, NODE_STATE_SECONDARY);
#endif

    setup_signals();
    signal(SIGPIPE, SIG_IGN);
    memset(&arbiter_reconnect, 0, sizeof(arbiter_reconnect));
    arbiter_reconnect.max_retry_interval_ms = 30000;
    try_reconnect_arbiter(NULL);

    int port = config_get_int(&local_bus_cfg, "listen_port", 5000);
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
    register_handler(listen_fd, POLLIN, on_new_connection, NULL);

    event_loop();
    if (arbiter_fd >= 0) close(arbiter_fd);
    if (listen_fd >= 0) close(listen_fd);
    if (peer_fd >= 0) close(peer_fd);
    return 0;
}