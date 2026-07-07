// device.h — Device context, bus registration, and data forwarding
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

// Global device context instance
extern DeviceContext dev_ctx;

// Initialize device with configuration
void device_init(const Config *cfg);
// Connect to bus endpoint, returns fd on success
int device_connect_to_bus(const char *bus_endpoint);
// Send registration to bus and wait for role assignment
bool device_send_register(int bus_fd, const DeviceRegisterMessage *msg, DeviceRoleAssignMessage *reply);
// Receive role change notification from bus
bool device_receive_role_change(int bus_fd, RoleChangeMessage *msg);
// Send data packet to bus
bool device_send_data(int bus_fd, const DeviceDataPacketMessage *msg);
// Receive control message from bus
bool device_receive_control(int bus_fd, uint8_t **out_msg, uint16_t *out_len);
// Process sync status from bus
void device_receive_sync_status(uint8_t synced, uint64_t last_committed_log_id);
// Query sync status from bus
bool device_query_sync_status(int bus_fd);