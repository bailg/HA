#pragma once
#include "common/protocol.h"
#include "common/config.h"

bool arbiter_login_bus(const BusLoginMessage *msg, LoginAckMessage *reply);
bool arbiter_receive_heartbeat(const HeartbeatMessage *msg);

// 【方向B】拆分的故障接管接口
bool arbiter_prepare_failover(const char **out_target_id, uint32_t *out_new_epoch);
void arbiter_confirm_promotion(const char *target_id, uint32_t new_epoch);

void arbiter_init(const Config *cfg);