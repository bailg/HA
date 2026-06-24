#pragma once
#include "common/protocol.h"
#include "common/config.h"

typedef struct {
    int bus_primary_fd;
    int bus_secondary_fd;
    uint32_t last_accepted_epoch;
    uint64_t last_sent_message_id;
    char device_id[DEVICE_ID_MAX_LEN];
    NodeRole current_role;
} DeviceContext;

extern DeviceContext dev_ctx;

void device_init(const Config *cfg);
int device_connect_to_bus(const char *bus_endpoint);
bool device_send_register(int bus_fd, const DeviceRegisterMessage *msg, DeviceRoleAssignMessage *reply);
bool device_receive_role_change(int bus_fd, RoleChangeMessage *msg);
bool device_send_data(int bus_fd, const DeviceDataPacketMessage *msg);
bool device_receive_control(int bus_fd, uint8_t **out_msg, uint16_t *out_len);