// bus.h — Bus node state machine, sync protocol, and device management
#pragma once
#include "common/protocol.h"
#include "common/config.h"

typedef struct {
    BusState state;
    int heartbeat_miss_count;
    uint32_t epoch;
    uint64_t last_committed_log_id;
    uint64_t current_log_id;
    bool is_switching;
    uint64_t last_heartbeat_ack_ms;
    int heartbeat_ack_miss_count;
} BusNode;

// ======== Bus Lifecycle ========
// Initialize bus with config and initial state
void bus_init(const Config *cfg, BusState initial_state);
// Get current bus state
BusState bus_get_state(void);
// Get current bus epoch
uint32_t bus_get_epoch(void);
// Handle arbiter disconnection event
void on_arbiter_disconnect(void);

// ======== Device Management ========
// Register a device via bus, returns true on success
bool bus_register_device(const DeviceRegisterMessage *msg, DeviceRoleAssignMessage *reply, int current_device_count);
// Process a device data packet, returns true on success
bool process_device_packet(const DeviceDataPacketMessage *msg, WriteResponseMessage *resp, int peer_fd);

// ======== Sync Protocol ========
// Apply a sync entry received from primary (direction A)
void bus_apply_sync_entry(uint64_t log_id, const uint8_t *payload, uint32_t payload_size);
// Apply failover state machine transition from arbiter (direction B)
void bus_apply_failover(uint32_t new_epoch);
// Apply degrade state machine transition from arbiter
void bus_apply_degrade(uint32_t new_epoch);
// Send a sync entry to secondary node
bool bus_sync_entry_to_secondary(int peer_fd, uint64_t log_id,
                                 const uint8_t *payload, uint32_t payload_size);
// Receive a sync entry from primary node
bool bus_receive_sync_entry(int peer_fd, uint64_t *out_log_id,
                            uint8_t *out_payload, uint32_t *out_payload_size);
// Send sync acknowledgment to peer
void bus_send_sync_ack(int peer_fd, uint64_t log_id, uint8_t status);
// Request full sync from secondary node
void bus_request_full_sync(int peer_fd, const char *node_id);
// Respond with sync status to requesting node
void bus_respond_sync_status(int peer_fd, const char *node_id,
                             bool synced, uint64_t log_id);

// ======== Accessors ========
// Get last committed log ID
uint64_t bus_get_last_committed_log_id(void);
// Set last committed log ID
void bus_set_last_committed_log_id(uint64_t log_id);
// Mark heartbeat ack received
void bus_receive_heartbeat_ack(void);
// Set current bus state
void bus_set_state(BusState state);
// Set current epoch
void bus_set_epoch(uint32_t epoch);
