#include "bus.h"
#include "common/network.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static BusNode self_node;
static Config bus_cfg;

void bus_init(const Config *cfg, BusState initial_state) {
    bus_cfg = *cfg;
    memset(&self_node, 0, sizeof(BusNode));
    self_node.state = initial_state;
    self_node.epoch = 0;
    self_node.last_committed_log_id = 0;
    self_node.current_log_id = 0;
    self_node.is_switching = false;
}

BusState bus_get_state(void) { return self_node.state; }
uint32_t bus_get_epoch(void) { return self_node.epoch; }

static void transition_to(BusNode *node, BusState target_state) {
    LOG_INFO("Bus transitioning from %d to %d", node->state, target_state);
    node->state = target_state;
}

void on_arbiter_disconnect(void) {
    if (self_node.state == NODE_STATE_PRIMARY || self_node.state == NODE_STATE_SECONDARY || self_node.state == NODE_STATE_SOLO) {
        LOG_INFO("Arbiter disconnected, keeping current role %d", self_node.state);
    } else {
        transition_to(&self_node, NODE_STATE_OFFLINE);
    }
}

static uint64_t append_local_log(const uint8_t *payload, uint32_t size) {
    (void)payload; (void)size;
    self_node.current_log_id++;
    return self_node.current_log_id;
}

static void commit_log(uint64_t log_id) {
    self_node.last_committed_log_id = log_id;
}

#define MAX_MSG_CACHE 1024
static uint64_t processed_msg_ids[MAX_MSG_CACHE];
static uint8_t processed_msg_statuses[MAX_MSG_CACHE];
static int processed_count = 0;

static bool has_processed_message(const char *device_id, uint64_t message_id) {
    (void)device_id;
    for (int i = 0; i < processed_count; i++) {
        if (processed_msg_ids[i] == message_id) return true;
    }
    return false;
}

static bool send_cached_response(const char *device_id, uint64_t message_id) {
    (void)device_id;
    for (int i = 0; i < processed_count; i++) {
        if (processed_msg_ids[i] == message_id) return processed_msg_statuses[i] == 1;
    }
    return false;
}

static void cache_response(uint64_t message_id, bool success) {
    if (processed_count < MAX_MSG_CACHE) {
        processed_msg_ids[processed_count] = message_id;
        processed_msg_statuses[processed_count] = success ? 1 : 0;
        processed_count++;
    }
}

bool process_device_packet(const DeviceDataPacketMessage *msg, WriteResponseMessage *resp, int peer_fd) {
    memset(resp, 0, sizeof(WriteResponseMessage));
    resp->header.type = MSG_TYPE_WRITE_RESPONSE;
    resp->header.length = sizeof(WriteResponseMessage);
    resp->header.timestamp = current_time_ms();
    resp->message_id = msg->message_id;

    if (has_processed_message(msg->device_id, msg->message_id)) {
        resp->success = send_cached_response(msg->device_id, msg->message_id) ? 1 : 0;
        protocol_hton_header(&resp->header);
        resp->message_id = c11_htobe64(resp->message_id);
        return true;
    }

    if (self_node.state != NODE_STATE_PRIMARY && self_node.state != NODE_STATE_SOLO) {
        resp->success = 0;
        protocol_hton_header(&resp->header);
        resp->message_id = c11_htobe64(resp->message_id);
        return false;
    }

    uint64_t log_id = append_local_log(msg->payload, msg->payload_size);
    bool sync_ok = false;

    // 【方向A】强一致性同步：主节点必须阻塞等待备节点 ACK
    if (peer_fd >= 0) {
        BusSyncEntryMessage sync_msg;
        memset(&sync_msg, 0, sizeof(sync_msg));
        sync_msg.header.type = MSG_TYPE_BUS_SYNC_ENTRY;
        sync_msg.header.length = sizeof(sync_msg);
        sync_msg.header.timestamp = current_time_ms();
        sync_msg.log_id = log_id;
        sync_msg.payload_size = msg->payload_size;
        memcpy(sync_msg.payload, msg->payload, msg->payload_size);
        protocol_hton_sync_entry(&sync_msg);

        ssize_t sent = send(peer_fd, &sync_msg, sizeof(sync_msg), 0);
        if (sent == sizeof(sync_msg)) {
            BusAckMessage ack;
            ssize_t n = recv(peer_fd, &ack, sizeof(ack), MSG_WAITALL);
            if (n == sizeof(ack)) {
                protocol_ntoh_bus_ack(&ack);
                if (ack.status == 1 && ack.log_id == log_id) {
                    sync_ok = true;
                }
            }
        }
    } else {
        sync_ok = true; // SOLO 模式
    }
    
    if (sync_ok) {
        commit_log(log_id);
        cache_response(msg->message_id, true);
        resp->success = 1;
    } else {
        cache_response(msg->message_id, false);
        resp->success = 0;
    }
    
    protocol_hton_header(&resp->header);
    resp->message_id = c11_htobe64(resp->message_id);
    return sync_ok;
}

// 【方向A】备节点被调用，应用主节点发来的日志
void bus_apply_sync_entry(uint64_t log_id, const uint8_t *payload, uint32_t payload_size) {
    (void)payload; (void)payload_size;
    self_node.last_committed_log_id = log_id;
}

// 【方向B】收到 Arbiter 指令后的状态机翻转
void bus_apply_failover(uint32_t new_epoch) {
    LOG_INFO("Applying FAILOVER_COMMAND, new epoch: %u", new_epoch);
    self_node.epoch = new_epoch;
    if (self_node.state == NODE_STATE_SECONDARY) {
        transition_to(&self_node, NODE_STATE_PRIMARY);
    } else if (self_node.state == NODE_STATE_PRIMARY) {
        transition_to(&self_node, NODE_STATE_SECONDARY);
    }
}

bool bus_register_device(const DeviceRegisterMessage *msg, DeviceRoleAssignMessage *reply) {
    static int device_count = 0;
    memset(reply, 0, sizeof(DeviceRoleAssignMessage));
    reply->header.type = MSG_TYPE_DEVICE_ROLE_ASSIGN;
    reply->header.length = sizeof(DeviceRoleAssignMessage);
    reply->header.timestamp = current_time_ms();
    strncpy(reply->device_id, msg->device_id, DEVICE_ID_MAX_LEN);
    reply->epoch = self_node.epoch;

    if (device_count == 0) { reply->role = ROLE_PRIMARY; device_count++; }
    else { reply->role = ROLE_SECONDARY; }
    return true;
}

void bus_apply_degrade(uint32_t new_epoch) {
    LOG_INFO("Applying DEGRADE_COMMAND, new epoch: %u", new_epoch);
    self_node.epoch = new_epoch;
    /* 主节点降级为备节点，停止接受写请求 */
    if (self_node.state == NODE_STATE_PRIMARY) {
        self_node.is_switching = true;
        transition_to(&self_node, NODE_STATE_SECONDARY);
        self_node.is_switching = false;
    }
}

uint64_t bus_get_last_committed_log_id(void) {
    return self_node.last_committed_log_id;
}
