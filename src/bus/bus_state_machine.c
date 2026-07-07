// bus_state_machine.c — Bus node state machine: state transitions, log management, data write path
//
// Implements the bus node state machine including failover, degrade, sync entry application,
// device registration, and idempotent data write with strong-consistency replication.

#include "bus.h"
#include "common/network.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>

// ========================================================================
// Constants
// ========================================================================

#define PEER_ACK_POLL_MS 200
#define PEER_ACK_MAX_RETRY 15

// ========================================================================
// Static state
// ========================================================================

static BusNode self_node;
static Config bus_cfg;

// ========================================================================
// Initialization
// ========================================================================

// Initialize the bus node with the given configuration and initial state
void bus_init(const Config *cfg, BusState initial_state) {
    bus_cfg = *cfg;
    memset(&self_node, 0, sizeof(BusNode));
    self_node.state = initial_state;
    self_node.epoch = 0;
    self_node.last_committed_log_id = 0;
    self_node.current_log_id = 0;
    self_node.is_switching = false;
}

// ========================================================================
// State accessors
// ========================================================================

// Return the current bus node state
BusState bus_get_state(void) { return self_node.state; }

// Return the current epoch number
uint32_t bus_get_epoch(void) { return self_node.epoch; }

// ========================================================================
// State transitions
// ========================================================================

// Transition the node to a new state and log the change
static void transition_to(BusNode *node, BusState target_state) {
    LOG_INFO("Bus transitioning from %d to %d", node->state, target_state);
    node->state = target_state;
}

// ========================================================================
// Arbiter disconnect handling
// ========================================================================

// Handle arbiter disconnection: keep current role if active, else go offline
void on_arbiter_disconnect(void) {
    if (self_node.state == NODE_STATE_PRIMARY || self_node.state == NODE_STATE_SECONDARY || self_node.state == NODE_STATE_SOLO) {
        LOG_INFO("Arbiter disconnected, keeping current role %d", self_node.state);
    } else {
        transition_to(&self_node, NODE_STATE_OFFLINE);
    }
}

// ========================================================================
// Log management
// ========================================================================

// Append an entry to the local write-ahead log; returns the new log ID
static uint64_t append_local_log(const uint8_t *payload, uint32_t size) {
    (void)payload; (void)size;
    self_node.current_log_id++;
    return self_node.current_log_id;
}

// Mark a log entry as committed
static void commit_log(uint64_t log_id) {
    self_node.last_committed_log_id = log_id;
}

#define MAX_MSG_CACHE 1024
typedef struct {
    char device_id[DEVICE_ID_MAX_LEN];
    uint64_t message_id;
    uint8_t status;
    bool valid;
} CachedResponse;

// ========================================================================
// Idempotency cache
// ========================================================================

static CachedResponse cached_responses[MAX_MSG_CACHE];
static int cache_next = 0;

// Check whether a given message has already been processed (idempotency guard)
static bool has_processed_message(const char *device_id, uint64_t message_id) {
    for (int i = 0; i < MAX_MSG_CACHE; i++) {
        if (cached_responses[i].valid &&
            cached_responses[i].message_id == message_id &&
            strcmp(cached_responses[i].device_id, device_id) == 0) return true;
    }
    return false;
}

// Return the cached success status for a previously processed message
static bool send_cached_response(const char *device_id, uint64_t message_id) {
    for (int i = 0; i < MAX_MSG_CACHE; i++) {
        if (cached_responses[i].valid &&
            cached_responses[i].message_id == message_id &&
            strcmp(cached_responses[i].device_id, device_id) == 0)
            return cached_responses[i].status == 1;
    }
    return false;
}

// Cache a response for idempotent replay protection
static void cache_response(const char *device_id, uint64_t message_id, bool success) {
    cached_responses[cache_next].valid = true;
    strncpy(cached_responses[cache_next].device_id, device_id, DEVICE_ID_MAX_LEN);
    cached_responses[cache_next].message_id = message_id;
    cached_responses[cache_next].status = success ? 1 : 0;
    cache_next = (cache_next + 1) % MAX_MSG_CACHE;
}

// ========================================================================
// Data write path
// ========================================================================

// Process a data packet from a device: write to local log, sync to secondary, cache response
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
            /* 使用 poll 超时轮询替代 MSG_WAITALL 阻塞，防止备节点离线导致主总线数据面死锁 */
            BusAckMessage ack;
            int retries = 0;
            bool ack_received = false;
            while (retries < PEER_ACK_MAX_RETRY && !ack_received) {
                struct pollfd pfd;
                pfd.fd = peer_fd;
                pfd.events = POLLIN;
                int pret = poll(&pfd, 1, PEER_ACK_POLL_MS);
                    if (pret > 0 && (pfd.revents & POLLIN)) {
                        uint8_t ack_buf[sizeof(BusAckMessage)];
                        ssize_t n = recv(peer_fd, ack_buf, sizeof(ack_buf), MSG_DONTWAIT);
                        if (n == sizeof(BusAckMessage)) {
                            memcpy(&ack, ack_buf, sizeof(ack));
                            ack_received = true;
                        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                            retries++;
                            continue;
                        } else {
                            LOG_WARN("ACK recv unexpected: n=%zd errno=%d", n, errno);
                            break;
                        }
                    } else if (pret == 0) {
                        retries++;
                    } else {
                        LOG_WARN("ACK poll error: pret=%d revents=0x%x", pret, pfd.revents);
                        break;
                    }
            }
            if (ack_received) {
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
        cache_response(msg->device_id, msg->message_id, true);
        resp->success = 1;
        LOG_INFO("Data write OK: device=%s msg_id=%lu log_id=%lu",
                 msg->device_id, msg->message_id, log_id);
    } else {
        cache_response(msg->device_id, msg->message_id, false);
        resp->success = 0;
        LOG_WARN("Data write FAILED: device=%s msg_id=%lu",
                 msg->device_id, msg->message_id);
    }

    protocol_hton_header(&resp->header);
    resp->message_id = c11_htobe64(resp->message_id);
    return sync_ok;
}

// ========================================================================
// Sync entry application (secondary side)
// ========================================================================

// Apply a sync entry received from the primary node [Direction A]
void bus_apply_sync_entry(uint64_t log_id, const uint8_t *payload, uint32_t payload_size) {
    (void)payload; (void)payload_size;
    self_node.last_committed_log_id = log_id;
    LOG_INFO("Applied sync entry log_id=%lu, size=%u", log_id, payload_size);
}

// ========================================================================
// Failover / degrade
// ========================================================================

// Apply a failover command from arbiter: toggle primary/secondary state [Direction B]
void bus_apply_failover(uint32_t new_epoch) {
    LOG_INFO("Applying FAILOVER_COMMAND, new epoch: %u", new_epoch);
    self_node.epoch = new_epoch;
    if (self_node.state == NODE_STATE_SECONDARY) {
        transition_to(&self_node, NODE_STATE_PRIMARY);
    } else if (self_node.state == NODE_STATE_PRIMARY) {
        transition_to(&self_node, NODE_STATE_SECONDARY);
    }
}

// ========================================================================
// Device registration
// ========================================================================

// Register a device and assign a role (first device becomes PRIMARY)
bool bus_register_device(const DeviceRegisterMessage *msg, DeviceRoleAssignMessage *reply, int current_device_count) {
    memset(reply, 0, sizeof(DeviceRoleAssignMessage));
    reply->header.type = MSG_TYPE_DEVICE_ROLE_ASSIGN;
    reply->header.length = sizeof(DeviceRoleAssignMessage);
    reply->header.timestamp = current_time_ms();
    strncpy(reply->device_id, msg->device_id, DEVICE_ID_MAX_LEN);
    reply->epoch = self_node.epoch;

    if (current_device_count == 0) { reply->role = ROLE_PRIMARY; }
    else { reply->role = ROLE_SECONDARY; }
    return true;
}

// Apply a degrade command: primary demotes to secondary, stops accepting writes
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

// ========================================================================
// Accessors
// ========================================================================

// Return the ID of the last committed log entry
uint64_t bus_get_last_committed_log_id(void) {
    return self_node.last_committed_log_id;
}

// Set the last committed log ID; also advances current_log_id if needed
void bus_set_last_committed_log_id(uint64_t log_id) {
    self_node.last_committed_log_id = log_id;
    if (log_id > self_node.current_log_id) {
        self_node.current_log_id = log_id;
    }
}

// Update last heartbeat ack timestamp and reset miss count
void bus_receive_heartbeat_ack(void) {
    self_node.last_heartbeat_ack_ms = current_time_ms();
    self_node.heartbeat_ack_miss_count = 0;
}

// Set the bus node state (wraps transition_to)
void bus_set_state(BusState state) {
    transition_to(&self_node, state);
}

// Set the current epoch number
void bus_set_epoch(uint32_t epoch) {
    self_node.epoch = epoch;
}
