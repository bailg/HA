#include "arbiter.h"
#include "common/network.h"
#include <string.h>
#include <stdlib.h>

#define MAX_BUS_NODES 2
#define ARBITER_STATE_NORMAL 0
#define ARBITER_STATE_AUDIT  1

static ArbiterNodeInfo nodes[MAX_BUS_NODES];
static int node_count = 0;
static uint32_t global_max_epoch = 0;
static int arbiter_state = ARBITER_STATE_NORMAL;
static int64_t heartbeat_timeout_ms = 5000;

void arbiter_init(const Config *cfg) {
    node_count = 0;
    global_max_epoch = 0;
    arbiter_state = ARBITER_STATE_NORMAL;
    heartbeat_timeout_ms = config_get_int(cfg, "heartbeat_timeout_ms", 5000);
    memset(nodes, 0, sizeof(nodes));
}

static ArbiterNodeInfo* find_node(const char *node_id) {
    for (int i = 0; i < node_count; i++) {
        if (strcmp(nodes[i].node_id, node_id) == 0) return &nodes[i];
    }
    return NULL;
}

static ArbiterNodeInfo* find_node_by_state(BusState state) {
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].state == state) return &nodes[i];
    }
    return NULL;
}

static bool is_secondary_synced(void) {
    ArbiterNodeInfo *pri = find_node_by_state(NODE_STATE_PRIMARY);
    ArbiterNodeInfo *sec = find_node_by_state(NODE_STATE_SECONDARY);
    if (!pri || !sec) return false;
    return sec->last_committed_log_id >= pri->last_committed_log_id;
}

bool arbiter_login_bus(const BusLoginMessage *msg, LoginAckMessage *reply) {
    if (arbiter_state == ARBITER_STATE_AUDIT) {
        reply->accepted = 0;
        return false;
    }

    if (msg->epoch < global_max_epoch) {
        LOG_WARN("Reject login from older epoch node %s (epoch %u < %u)", 
                 msg->node_id, msg->epoch, global_max_epoch);
        reply->accepted = 0;
        return false;
    }

    ArbiterNodeInfo *node = find_node(msg->node_id);
    if (!node) {
        if (node_count >= MAX_BUS_NODES) {
            LOG_ERROR("Too many bus nodes");
            reply->accepted = 0;
            return false;
        }
        node = &nodes[node_count++];
    }

    if (msg->state == NODE_STATE_PRIMARY) {
        ArbiterNodeInfo *cur_pri = find_node_by_state(NODE_STATE_PRIMARY);
        if (cur_pri && strcmp(cur_pri->node_id, msg->node_id) != 0) {
            if (msg->epoch == cur_pri->epoch) {
                if (msg->last_committed_log_id > cur_pri->last_committed_log_id) {
                    cur_pri->state = NODE_STATE_SECONDARY;
                    cur_pri->role = ROLE_SECONDARY;
                } else {
                    LOG_WARN("Conflict primary role, demoting incoming node %s", msg->node_id);
                    strncpy(node->node_id, msg->node_id, NODE_ID_MAX_LEN);
                    node->state = NODE_STATE_SECONDARY;
                    node->role = ROLE_SECONDARY;
                    node->epoch = msg->epoch;
                    node->last_heartbeat = current_time_ms();
                    node->last_committed_log_id = msg->last_committed_log_id;
                    
                    reply->accepted = 1;
                    reply->assigned_role = ROLE_SECONDARY;
                    reply->epoch = global_max_epoch;
                    return true;
                }
            }
        }
    }

    strncpy(node->node_id, msg->node_id, NODE_ID_MAX_LEN);
    node->state = msg->state;
    node->role = msg->role;
    node->epoch = msg->epoch;
    node->last_heartbeat = current_time_ms();
    node->last_committed_log_id = msg->last_committed_log_id;

    if (msg->epoch > global_max_epoch) {
        global_max_epoch = msg->epoch;
    }

    reply->accepted = 1;
    reply->assigned_role = msg->role;
    reply->epoch = global_max_epoch;
    return true;
}

bool arbiter_receive_heartbeat(const HeartbeatMessage *msg) {
    ArbiterNodeInfo *node = find_node(msg->node_id);
    if (node) {
        node->last_heartbeat = current_time_ms();
        if (msg->epoch < node->epoch) {
            LOG_WARN("Stale heartbeat from %s (epoch %u < %u), but keeping it alive.", 
                     msg->node_id, msg->epoch, node->epoch);
        } else if (msg->epoch > node->epoch) {
            node->epoch = msg->epoch;
        }
        return true;
    }
    return false;
}

// 【方向B】供网络层调用：判断是否需要切换，并返回目标节点ID和新Epoch
bool arbiter_prepare_failover(const char **out_target_id, uint32_t *out_new_epoch) {
    if (arbiter_state == ARBITER_STATE_AUDIT) return false;
    int64_t now = current_time_ms();
    ArbiterNodeInfo *pri = find_node_by_state(NODE_STATE_PRIMARY);
    
    if (pri && ((int64_t)(now - pri->last_heartbeat) > heartbeat_timeout_ms)) {
        if (is_secondary_synced()) {
            ArbiterNodeInfo *sec = find_node_by_state(NODE_STATE_SECONDARY);
            if (sec) {
                global_max_epoch++;
                *out_target_id = sec->node_id;
                *out_new_epoch = global_max_epoch;
                return true;
            }
        } else {
            LOG_WARN("Primary %s failed, but secondary not synced. Waiting.", pri->node_id);
        }
    }
    return false;
}

// 【方向B】网络层发送完指令后调用：真正修改内存状态
void arbiter_confirm_promotion(const char *target_id, uint32_t new_epoch) {
    ArbiterNodeInfo *sec = find_node(target_id);
    ArbiterNodeInfo *pri = find_node_by_state(NODE_STATE_PRIMARY);
    if (sec && pri) {
        sec->state = NODE_STATE_PRIMARY;
        sec->role = ROLE_PRIMARY;
        sec->epoch = new_epoch;
        sec->last_heartbeat = current_time_ms();
        
        pri->state = NODE_STATE_SECONDARY;
        pri->role = ROLE_SECONDARY;
        pri->epoch = new_epoch;
    }
}