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

void bus_init(const Config *cfg, BusState initial_state);
BusState bus_get_state(void);
uint32_t bus_get_epoch(void);
void on_arbiter_disconnect(void);

bool bus_register_device(const DeviceRegisterMessage *msg, DeviceRoleAssignMessage *reply);
bool process_device_packet(const DeviceDataPacketMessage *msg, WriteResponseMessage *resp, int peer_fd);

// 【方向A】备节点接收主节点同步日志
void bus_apply_sync_entry(uint64_t log_id, const uint8_t *payload, uint32_t payload_size);
// 【方向B】收到 Arbiter 指令后执行状态机切换
void bus_apply_failover(uint32_t new_epoch);
void bus_apply_degrade(uint32_t new_epoch);

/* bus_sync.c - 主备同步协议函数 */
bool bus_sync_entry_to_secondary(int peer_fd, uint64_t log_id,
                                 const uint8_t *payload, uint32_t payload_size);
bool bus_receive_sync_entry(int peer_fd, uint64_t *out_log_id,
                            uint8_t *out_payload, uint32_t *out_payload_size);
void bus_send_sync_ack(int peer_fd, uint64_t log_id, uint8_t status);
void bus_request_full_sync(int peer_fd, const char *node_id);
void bus_respond_sync_status(int peer_fd, const char *node_id,
                             bool synced, uint64_t log_id);

uint64_t bus_get_last_committed_log_id(void);
void bus_set_last_committed_log_id(uint64_t log_id);
void bus_receive_heartbeat_ack(void);
