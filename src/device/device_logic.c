#include "device.h"
#include "common/network.h"
#include <string.h>

DeviceContext dev_ctx;

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

bool device_send_register(int bus_fd, const DeviceRegisterMessage *msg, DeviceRoleAssignMessage *reply) {
    if (bus_fd < 0) return false;
    
    DeviceRegisterMessage req = *msg;
    protocol_hton_device_reg(&req);
    send(bus_fd, &req, sizeof(req), 0);
    
    ssize_t n = recv(bus_fd, reply, sizeof(DeviceRoleAssignMessage), MSG_WAITALL);
    if (n == sizeof(DeviceRoleAssignMessage)) {
        protocol_ntoh_device_role_assign(reply);
        
        if (reply->epoch >= dev_ctx.last_accepted_epoch) {
            dev_ctx.last_accepted_epoch = reply->epoch;
            dev_ctx.current_role = reply->role;
            LOG_INFO("Device assigned role %d, epoch %d", reply->role, reply->epoch);
            return true;
        } else {
            LOG_WARN("Rejected stale role assign epoch %d < %d", reply->epoch, dev_ctx.last_accepted_epoch);
            return false;
        }
    }
    return false;
}

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

bool device_send_data(int bus_fd, const DeviceDataPacketMessage *msg) {
    if (dev_ctx.current_role != ROLE_PRIMARY) {
        LOG_WARN("Device is not primary, cannot send data.");
        return false;
    }
    if (bus_fd < 0) return false;

    DeviceDataPacketMessage req = *msg;
    protocol_hton_device_data(&req);
    send(bus_fd, &req, sizeof(req), 0);

    WriteResponseMessage resp;
    ssize_t n = recv(bus_fd, &resp, sizeof(resp), MSG_WAITALL);
    if (n == sizeof(resp)) {
        protocol_ntoh_header(&resp.header);
        resp.message_id = c11_be64toh(resp.message_id);
        return resp.success == 1;
    }
    return false;
}