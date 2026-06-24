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