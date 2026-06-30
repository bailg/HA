#pragma once
#include "common/protocol.h"
#include "common/config.h"

bool arbiter_login_bus(const BusLoginMessage *msg, LoginAckMessage *reply);
bool arbiter_receive_heartbeat(const HeartbeatMessage *msg);

// 【方向B】拆分的故障接管接口
bool arbiter_prepare_failover(const char **out_target_id, uint32_t *out_new_epoch);
void arbiter_confirm_promotion(const char *target_id, uint32_t new_epoch);

void arbiter_init(const Config *cfg);

/* DEGRADE_COMMAND: failover 后向旧主节点发送降级指令 */
const char* arbiter_get_old_primary(void);
void arbiter_clear_old_primary(void);
bool arbiter_should_send_degrade(const char **out_old_primary_id, uint32_t *out_new_epoch);
void arbiter_send_degrade_to_old_primary(int fd, uint32_t from_epoch, const char *old_primary_id);

/* 获取备节点的同步状态（用于审计决定） */
bool arbiter_is_secondary_synced(const char *node_id);
