// arbiter_logic.c — Arbiter state machine, node management, failover logic
//
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

/* 跟踪旧主节点信息，用于发送 DEGRADE_COMMAND */
static char old_primary_id[NODE_ID_MAX_LEN];
static bool has_old_primary = false;

// ========

// Initialize arbiter state from config
void arbiter_init(const Config *cfg) {
    node_count = 0;
    global_max_epoch = 0;
    arbiter_state = ARBITER_STATE_NORMAL;
    heartbeat_timeout_ms = config_get_int(cfg, "heartbeat_timeout_ms", 5000);
    memset(nodes, 0, sizeof(nodes));
}

// ======== Helper functions ========

// Find a node by its node ID
static ArbiterNodeInfo* find_node(const char *node_id) {
    for (int i = 0; i < node_count; i++) {
        if (strcmp(nodes[i].node_id, node_id) == 0) return &nodes[i];
    }
    return NULL;
}

// Find a node with the given state
static ArbiterNodeInfo* find_node_by_state(BusState state) {
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].state == state) return &nodes[i];
    }
    return NULL;
}

// ======== Event handlers ========

// Process a bus login request and assign role; returns true if accepted
bool arbiter_login_bus(const BusLoginMessage *msg, LoginAckMessage *reply) {
    if (arbiter_state == ARBITER_STATE_AUDIT) {
        reply->accepted = 0;
        return false;
    }

    if (msg->epoch < global_max_epoch) {
        LOG_WARN("Login from older epoch node %s (epoch %u < %u), accepting as SECONDARY",
                 msg->node_id, msg->epoch, global_max_epoch);
        /* 旧 epoch 节点（如重启的旧主）仍然允许登录，降级为 SECONDARY */
        ArbiterNodeInfo *node = find_node(msg->node_id);
        if (!node) {
            if (node_count >= MAX_BUS_NODES) {
                reply->accepted = 0;
                return false;
            }
            node = &nodes[node_count++];
        }
        strncpy(node->node_id, msg->node_id, NODE_ID_MAX_LEN);
        node->state = NODE_STATE_SECONDARY;
        node->role = ROLE_SECONDARY;
        node->epoch = global_max_epoch;
        node->last_heartbeat = current_time_ms();
        node->last_committed_log_id = msg->last_committed_log_id;

        reply->accepted = 1;
        reply->assigned_role = ROLE_SECONDARY;
        reply->epoch = global_max_epoch;
        return true;
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

    bool is_primary = (msg->state == NODE_STATE_PRIMARY);
    bool demote_incoming = false;

    if (is_primary) {
        ArbiterNodeInfo *cur_pri = find_node_by_state(NODE_STATE_PRIMARY);
        if (cur_pri && strcmp(cur_pri->node_id, msg->node_id) != 0) {
            if (msg->epoch > cur_pri->epoch) {
                /* 新节点 epoch 更高，降级旧主 */
                cur_pri->state = NODE_STATE_SECONDARY;
                cur_pri->role = ROLE_SECONDARY;
            } else if (msg->epoch == cur_pri->epoch) {
                /* 相同 epoch：比较日志进度决定谁降级 */
                if (msg->last_committed_log_id > cur_pri->last_committed_log_id) {
                    cur_pri->state = NODE_STATE_SECONDARY;
                    cur_pri->role = ROLE_SECONDARY;
                } else {
                    LOG_WARN("Conflict primary (same epoch), demoting incoming node %s", msg->node_id);
                    demote_incoming = true;
                }
            } else {
                /* 新节点 epoch 更低，直接降级它 */
                LOG_WARN("Demoting incoming primary %s (epoch %u < %u)", msg->node_id, msg->epoch, cur_pri->epoch);
                demote_incoming = true;
            }
        }
    }

    /* 统一写入节点信息（已通过 find_node 或者在 line 78 分配） */
    strncpy(node->node_id, msg->node_id, NODE_ID_MAX_LEN);
    node->state = demote_incoming ? NODE_STATE_SECONDARY : msg->state;
    node->role = demote_incoming ? ROLE_SECONDARY : msg->role;
    node->epoch = msg->epoch;
    node->last_heartbeat = current_time_ms();
    node->last_committed_log_id = msg->last_committed_log_id;

    if (msg->epoch > global_max_epoch) {
        global_max_epoch = msg->epoch;
    }

    reply->accepted = 1;
    reply->assigned_role = demote_incoming ? ROLE_SECONDARY : msg->role;
    reply->epoch = global_max_epoch;
    return true;
}

// Process a heartbeat message and update node state
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

// Check if primary has timed out and prepare failover target and new epoch
// 【方向B】供网络层调用：判断是否需要切换，并返回目标节点ID和新Epoch
bool arbiter_prepare_failover(const char **out_target_id, uint32_t *out_new_epoch) {
    if (arbiter_state == ARBITER_STATE_AUDIT) return false;
    int64_t now = current_time_ms();
    ArbiterNodeInfo *pri = find_node_by_state(NODE_STATE_PRIMARY);

    if (pri) {
        int64_t elapsed = now - pri->last_heartbeat;
        LOG_INFO("[FO] pri=%s last_hb=%ld now=%ld elapsed=%ld timeout=%ld",
                 pri->node_id, (long)pri->last_heartbeat, (long)now, (long)elapsed, (long)heartbeat_timeout_ms);
        if (elapsed > heartbeat_timeout_ms) {
            ArbiterNodeInfo *sec = find_node_by_state(NODE_STATE_SECONDARY);
            if (sec) {
                global_max_epoch++;
                *out_target_id = sec->node_id;
                *out_new_epoch = global_max_epoch;
                return true;
            } else {
                LOG_WARN("Primary %s failed, but no secondary available.", pri->node_id);
            }
        }
    }
    return false;
}

// Promote secondary to primary and demote old primary in-memory state
// 【方向B】网络层发送完指令后调用：真正修改内存状态
void arbiter_confirm_promotion(const char *target_id, uint32_t new_epoch) {
    ArbiterNodeInfo *sec = find_node(target_id);
    ArbiterNodeInfo *pri = find_node_by_state(NODE_STATE_PRIMARY);
    if (sec && pri) {
        /* 保存旧主节点信息，用于后续发送 DEGRADE_COMMAND */
        strncpy(old_primary_id, pri->node_id, NODE_ID_MAX_LEN);
        has_old_primary = true;
        LOG_INFO("Old primary %s marked for DEGRADE_COMMAND, new epoch=%u",
                 old_primary_id, new_epoch);

        sec->state = NODE_STATE_PRIMARY;
        sec->role = ROLE_PRIMARY;
        sec->epoch = new_epoch;
        sec->last_heartbeat = current_time_ms();

        pri->state = NODE_STATE_SECONDARY;
        pri->role = ROLE_SECONDARY;
        pri->epoch = new_epoch;
        pri->last_heartbeat = current_time_ms();
    }
}

// Return the old primary node ID if one is tracked
/* 返回旧主节点 ID */
const char* arbiter_get_old_primary(void) {
    return has_old_primary ? old_primary_id : NULL;
}

// Clear the tracked old primary node ID
/* 清除旧主跟踪 */
void arbiter_clear_old_primary(void) {
    has_old_primary = false;
    memset(old_primary_id, 0, sizeof(old_primary_id));
}

// Check whether a degrade command should be sent to the old primary
/* 判断是否需要向旧主发送 DEGRADE_COMMAND */
bool arbiter_should_send_degrade(const char **out_old_primary_id, uint32_t *out_new_epoch) {
    if (!has_old_primary) return false;
    ArbiterNodeInfo *old = find_node(old_primary_id);
    /* 旧主仍然活跃且需要降级 */
    if (old && old->state != NODE_STATE_SECONDARY) {
        *out_old_primary_id = old_primary_id;
        *out_new_epoch = global_max_epoch;
        return true;
    }
    return false;
}

// Send DEGRADE_COMMAND to the old primary node over the given fd
/* 向旧主节点发送 DEGRADE_COMMAND 消息 */
void arbiter_send_degrade_to_old_primary(int fd, uint32_t from_epoch, const char *old_primary_id) {
    DegradeCommandMessage dcmd;
    memset(&dcmd, 0, sizeof(dcmd));
    dcmd.header.type = MSG_TYPE_DEGRADE_COMMAND;
    dcmd.header.length = sizeof(dcmd);
    dcmd.header.timestamp = current_time_ms();
    strncpy(dcmd.target_node_id, old_primary_id, NODE_ID_MAX_LEN);
    dcmd.demote_to = ROLE_SECONDARY;
    dcmd.epoch = from_epoch;
    protocol_hton_degrade_cmd(&dcmd);
    send(fd, &dcmd, sizeof(dcmd), MSG_NOSIGNAL);
    LOG_INFO("Sent DEGRADE_COMMAND to %s, epoch=%u", old_primary_id, from_epoch);
}

// Check if a secondary node is synced with the primary (for audit decisions)
/* 检查备节点同步状态（用于审计决策） */
bool arbiter_is_secondary_synced(const char *node_id) {
    ArbiterNodeInfo *node = find_node(node_id);
    ArbiterNodeInfo *pri = find_node_by_state(NODE_STATE_PRIMARY);
    if (node && pri) {
        return node->last_committed_log_id >= pri->last_committed_log_id;
    }
    return false;
}
