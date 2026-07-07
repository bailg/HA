// device_logic.c — Device logic: registration, role handling, sync queries, data send
//

#include "device.h"
#include "common/network.h"
#include <string.h>
#include <poll.h>
#include <errno.h>

DeviceContext dev_ctx;

// ========
// device_init — Initialize device context with config defaults
// ========

void device_init(const Config *cfg) {
    memset(&dev_ctx, 0, sizeof(DeviceContext));
    dev_ctx.bus_primary_fd = -1;
    dev_ctx.bus_secondary_fd = -1;
    dev_ctx.last_accepted_epoch = 0;
    dev_ctx.last_sent_message_id = 0;

    const char *id = config_get(cfg, "device_id");
    if (!id) id = "device_unk";
    strncpy(dev_ctx.device_id, id, DEVICE_ID_MAX_LEN);
    dev_ctx.current_role = ROLE_SECONDARY;
}

// ========
// device_send_register — Send registration message and wait for role assignment
// ========

bool device_send_register(int bus_fd, const DeviceRegisterMessage *msg, DeviceRoleAssignMessage *reply) {
    if (bus_fd < 0) return false;

    DeviceRegisterMessage req = *msg;
    protocol_hton_device_reg(&req);
    send(bus_fd, &req, sizeof(req), 0);

    ssize_t n = recv(bus_fd, reply, sizeof(DeviceRoleAssignMessage), MSG_WAITALL);
    if (n == sizeof(DeviceRoleAssignMessage)) {
        protocol_ntoh_device_role_assign(reply);
        dev_ctx.last_accepted_epoch = reply->epoch;
        dev_ctx.current_role = reply->role;
        LOG_INFO("Device assigned role %d, epoch %d", reply->role, reply->epoch);
        return true;
    }
    return false;
}

// ========
// device_receive_role_change — Process role change message from bus
// ========

bool device_receive_role_change(int bus_fd, RoleChangeMessage *msg) {
    (void)bus_fd;
    if (msg->epoch >= dev_ctx.last_accepted_epoch) {
        dev_ctx.last_accepted_epoch = msg->epoch;
        dev_ctx.current_role = msg->role;
        LOG_INFO("Device role changed to %d, epoch %d", msg->role, msg->epoch);
        return true;
    }
    LOG_WARN("Ignored stale role change epoch %d < %d", msg->epoch, dev_ctx.last_accepted_epoch);
    return false;
}

// ========
// device_receive_sync_status — Update sync status from secondary bus
// ========

void device_receive_sync_status(uint8_t synced, uint64_t last_committed_log_id) {
    dev_ctx.last_accepted_epoch = (uint32_t)(last_committed_log_id & 0xFFFFFFFF);
    LOG_DEBUG("Sync status: synced=%d, log_id=%lu", synced, last_committed_log_id);
}

// ========
// device_query_sync_status — Send sync status query to the bus
// ========

bool device_query_sync_status(int bus_fd) {
    if (bus_fd < 0) return false;
    CheckSyncStatusMessage req;
    memset(&req, 0, sizeof(req));
    req.header.type = MSG_TYPE_CHECK_SYNC_STATUS;
    req.header.length = sizeof(req);
    req.header.timestamp = current_time_ms();
    strncpy(req.node_id, dev_ctx.device_id, NODE_ID_MAX_LEN);
    req.check_type = 0;
    protocol_hton_header(&req.header);
    send(bus_fd, &req, sizeof(req), 0);
    return true;
}

// ========
// device_send_data — Send a data packet and wait for write acknowledgment
// ========

bool device_send_data(int bus_fd, const DeviceDataPacketMessage *msg) {
    if (bus_fd < 0) return false;

    DeviceDataPacketMessage req = *msg;
    protocol_hton_device_data(&req);
    send(bus_fd, &req, sizeof(req), 0);

    WriteResponseMessage resp;
    /* 使用 poll 轮询等待，避免非阻塞 socket 上 MSG_WAITALL 返回 EAGAIN */
    for (int i = 0; i < 50; i++) {
        struct pollfd pfd;
        pfd.fd = bus_fd;
        pfd.events = POLLIN;
        int pret = poll(&pfd, 1, 100);
        if (pret > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = recv(bus_fd, &resp, sizeof(resp), MSG_DONTWAIT);
            if (n == sizeof(resp)) {
                protocol_ntoh_header(&resp.header);
                resp.message_id = c11_be64toh(resp.message_id);
                return resp.success == 1;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            break;
        }
        if (pret == 0) continue;
        break;
    }
    return false;
}