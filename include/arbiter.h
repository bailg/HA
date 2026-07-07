// arbiter.h — HA arbiter: leader election, failover coordination, node management
#pragma once
#include "common/protocol.h"
#include "common/config.h"

// Handle bus login request, returns true if accepted
bool arbiter_login_bus(const BusLoginMessage *msg, LoginAckMessage *reply);
// Process heartbeat from a bus node
bool arbiter_receive_heartbeat(const HeartbeatMessage *msg);
// Prepare failover: select target node and assign new epoch
bool arbiter_prepare_failover(const char **out_target_id, uint32_t *out_new_epoch);
// Confirm promotion of a target node after failover
void arbiter_confirm_promotion(const char *target_id, uint32_t new_epoch);
// Initialize arbiter with configuration
void arbiter_init(const Config *cfg);
// Get ID of the old primary node for degrade command
const char* arbiter_get_old_primary(void);
// Clear stored old primary reference
void arbiter_clear_old_primary(void);
// Check if degrade should be sent to old primary
bool arbiter_should_send_degrade(const char **out_old_primary_id, uint32_t *out_new_epoch);
// Send degrade command to old primary node
void arbiter_send_degrade_to_old_primary(int fd, uint32_t from_epoch, const char *old_primary_id);
// Check if a secondary node has caught up with sync
bool arbiter_is_secondary_synced(const char *node_id);
