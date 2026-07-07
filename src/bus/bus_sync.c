#include "bus.h"
#include "common/network.h"
#include "common/logging.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

// bus_sync.c — Bus primary-secondary sync module: strong-consistency replication
//
// Implements the strong-consistency primary-backup sync protocol:
// - Primary sends write log to secondary, blocks on ACK before committing
// - Secondary receives SYNC_ENTRY, applies locally, replies ACK
// - Secondary requests full sync from primary after recovery
//
// 总线主备同步模块 — 实现第6章设计的强一致性主备同步协议

// ========================================================================
// Primary-side sync
// ========================================================================

// Send a sync entry to the secondary and wait for ACK (blocking, strong consistency)
bool bus_sync_entry_to_secondary(int peer_fd, uint64_t log_id,
                                 const uint8_t *payload, uint32_t payload_size) {
    if (peer_fd < 0) return false;

    BusSyncEntryMessage sync_msg;
    memset(&sync_msg, 0, sizeof(sync_msg));
    sync_msg.header.type = MSG_TYPE_BUS_SYNC_ENTRY;
    sync_msg.header.length = sizeof(sync_msg);
    sync_msg.header.timestamp = current_time_ms();
    sync_msg.log_id = log_id;
    sync_msg.payload_size = payload_size;
    memcpy(sync_msg.payload, payload, payload_size);
    protocol_hton_sync_entry(&sync_msg);

    /* 阻塞发送并等待 ACK - 保证强一致性 */
    ssize_t sent = send(peer_fd, &sync_msg, sizeof(sync_msg), MSG_NOSIGNAL);
    if (sent != sizeof(sync_msg)) {
        LOG_ERROR("Failed to send sync entry log_id=%lu", (unsigned long)log_id);
        return false;
    }

    BusAckMessage ack;
    ssize_t n = recv(peer_fd, &ack, sizeof(ack), MSG_WAITALL);
    if (n != sizeof(ack)) {
        LOG_ERROR("Failed to receive sync ACK for log_id=%lu", (unsigned long)log_id);
        return false;
    }

    protocol_ntoh_bus_ack(&ack);
    if (ack.status != 1 || ack.log_id != log_id) {
        LOG_ERROR("Sync ACK mismatch: expected log_id=%lu, got status=%d log_id=%lu",
                  (unsigned long)log_id, ack.status, (unsigned long)ack.log_id);
        return false;
    }

    return true;
}

// ========================================================================
// Secondary-side sync
// ========================================================================

// Receive a sync entry from the primary (non-blocking, caller must poll first)
bool bus_receive_sync_entry(int peer_fd, uint64_t *out_log_id,
                            uint8_t *out_payload, uint32_t *out_payload_size) {
    if (peer_fd < 0) return false;

    BusSyncEntryMessage sync_msg;
    ssize_t n;

    /*
     * 备节点侧使用非阻塞方式轮询接收。
     * 调用者负责在 poll() 检测到可读事件后再调用本函数。
     */
    n = recv(peer_fd, &sync_msg, sizeof(sync_msg), MSG_DONTWAIT);
    if (n == 0) {
        return false;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        LOG_ERROR("Failed to receive sync entry: %s", strerror(errno));
        return false;
    }
    if (n != sizeof(sync_msg)) {
        LOG_ERROR("Incomplete sync entry: got %zd bytes", n);
        return false;
    }

    protocol_ntoh_sync_entry(&sync_msg);

    *out_log_id = sync_msg.log_id;
    if (sync_msg.payload_size <= PAYLOAD_MAX_LEN) {
        memcpy(out_payload, sync_msg.payload, sync_msg.payload_size);
        *out_payload_size = sync_msg.payload_size;
    } else {
        *out_payload_size = 0;
    }

    return true;
}

// Send a sync ACK back to the primary node
void bus_send_sync_ack(int peer_fd, uint64_t log_id, uint8_t status) {
    BusAckMessage ack;
    memset(&ack, 0, sizeof(ack));
    ack.header.type = MSG_TYPE_BUS_ACK;
    ack.header.length = sizeof(ack);
    ack.header.timestamp = current_time_ms();
    ack.log_id = log_id;
    ack.status = status;
    protocol_hton_bus_ack(&ack);
    send(peer_fd, &ack, sizeof(ack), MSG_NOSIGNAL);
}

// ========================================================================
// Full sync
// ========================================================================

// Request a full sync from the primary (called by secondary after recovery)
void bus_request_full_sync(int peer_fd, const char *node_id) {
    SyncRequestMessage req;
    memset(&req, 0, sizeof(req));
    req.header.type = MSG_TYPE_SYNC_REQUEST;
    req.header.length = sizeof(req);
    req.header.timestamp = current_time_ms();
    strncpy(req.node_id, node_id, NODE_ID_MAX_LEN);
    protocol_hton_sync_request(&req);
    send(peer_fd, &req, sizeof(req), MSG_NOSIGNAL);
    LOG_INFO("Sent sync request from %s to primary", node_id);
}

// Respond to a secondary node's sync status query
void bus_respond_sync_status(int peer_fd, const char *node_id,
                             bool synced, uint64_t log_id) {
    SyncOkMessage resp;
    memset(&resp, 0, sizeof(resp));
    resp.header.type = MSG_TYPE_SYNC_OK;
    resp.header.length = sizeof(resp);
    resp.header.timestamp = current_time_ms();
    strncpy(resp.node_id, node_id, NODE_ID_MAX_LEN);
    resp.synced = synced ? 1 : 0;
    resp.last_committed_log_id = log_id;
    protocol_hton_sync_ok(&resp);
    send(peer_fd, &resp, sizeof(resp), MSG_NOSIGNAL);
}