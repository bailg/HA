#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NODE_ID_MAX_LEN 32
#define DEVICE_ID_MAX_LEN 32
#define PAYLOAD_MAX_LEN 256

typedef enum {
    NODE_STATE_INIT = 0,
    NODE_STATE_OFFLINE,
    NODE_STATE_SOLO,
    NODE_STATE_PRIMARY,
    NODE_STATE_SECONDARY,
} BusState;

typedef enum {
    ROLE_PRIMARY = 0,
    ROLE_SECONDARY,
    ROLE_SOLO,
} NodeRole;

typedef enum {
    MSG_TYPE_BUS_LOGIN = 1,
    MSG_TYPE_HEARTBEAT,
    MSG_TYPE_FAILOVER_COMMAND,
    MSG_TYPE_FAILOVER_ACK,
    MSG_TYPE_DEVICE_REGISTER,
    MSG_TYPE_DEVICE_ROLE_ASSIGN,
    MSG_TYPE_ROLE_CHANGE,
    MSG_TYPE_DEVICE_DATA_PACKET,
    MSG_TYPE_BUS_SYNC_ENTRY,
    MSG_TYPE_BUS_ACK,
    MSG_TYPE_LOGIN_ACK,
    MSG_TYPE_HEARTBEAT_ACK,
    MSG_TYPE_WRITE_RESPONSE,
    MSG_TYPE_CHECK_SYNC_STATUS,
    MSG_TYPE_SYNC_OK,
    MSG_TYPE_DEGRADE_COMMAND,
    MSG_TYPE_SYNC_REQUEST,
} MessageType;

typedef enum {
    ERR_SUCCESS = 0,
    ERR_GENERIC = -1,
    ERR_TIMEOUT = -2,
    ERR_NETWORK = -3,
    ERR_INVALID_MSG = -4,
    ERR_EPOCH_MISMATCH = -5,
    ERR_NOT_PRIMARY = -6,
    ERR_ALREADY_PROCESSED = -7,
} ErrorCode;

#pragma pack(push, 1)
typedef struct {
    uint16_t type;
    uint16_t length;
    uint64_t timestamp;
} MessageHeader;

typedef struct {
    MessageHeader header;
    char node_id[NODE_ID_MAX_LEN];
    uint8_t state;
    uint8_t role;
    uint32_t epoch;
    uint64_t last_committed_log_id;
} BusLoginMessage;

typedef struct {
    MessageHeader header;
    char node_id[NODE_ID_MAX_LEN];
    uint32_t epoch;
} HeartbeatMessage;

typedef struct {
    MessageHeader header;
    char target_node_id[NODE_ID_MAX_LEN];
    uint8_t promote_to;
    uint32_t epoch;
} FailoverCommandMessage;

typedef struct {
    MessageHeader header;
    char node_id[NODE_ID_MAX_LEN];
    uint8_t accepted;
    uint32_t epoch;
} FailoverAckMessage;

typedef struct {
    MessageHeader header;
    char device_id[DEVICE_ID_MAX_LEN];
} DeviceRegisterMessage;

typedef struct {
    MessageHeader header;
    char device_id[DEVICE_ID_MAX_LEN];
    uint8_t role;
    uint32_t epoch;
} DeviceRoleAssignMessage;

typedef struct {
    MessageHeader header;
    char device_id[DEVICE_ID_MAX_LEN];
    uint8_t role;
    uint32_t epoch;
} RoleChangeMessage;

typedef struct {
    MessageHeader header;
    char device_id[DEVICE_ID_MAX_LEN];
    uint64_t message_id;
    uint32_t payload_size;
    uint8_t payload[PAYLOAD_MAX_LEN];
} DeviceDataPacketMessage;

typedef struct {
    MessageHeader header;
    uint64_t log_id;
    uint32_t payload_size;
    uint8_t payload[PAYLOAD_MAX_LEN];
} BusSyncEntryMessage;

typedef struct {
    MessageHeader header;
    uint64_t log_id;
    uint8_t status;
} BusAckMessage;

typedef struct {
    MessageHeader header;
    uint8_t accepted;
    uint8_t assigned_role;
    uint32_t epoch;
} LoginAckMessage;

typedef struct {
    MessageHeader header;
    uint64_t timestamp;
} HeartbeatAckMessage;

typedef struct {
    MessageHeader header;
    uint64_t message_id;
    uint8_t success;
} WriteResponseMessage;

typedef struct {
    MessageHeader header;
    char target_node_id[NODE_ID_MAX_LEN];
    uint8_t demote_to;
    uint32_t epoch;
} DegradeCommandMessage;

typedef struct {
    MessageHeader header;
    char node_id[NODE_ID_MAX_LEN];
    uint8_t check_type;
} CheckSyncStatusMessage;

typedef struct {
    MessageHeader header;
    char node_id[NODE_ID_MAX_LEN];
    uint8_t synced;
    uint64_t last_committed_log_id;
} SyncOkMessage;

typedef struct {
    MessageHeader header;
    char node_id[NODE_ID_MAX_LEN];
} SyncRequestMessage;
#pragma pack(pop)

typedef struct {
    char node_id[NODE_ID_MAX_LEN];
    BusState state;
    NodeRole role;
    uint32_t epoch;
    uint64_t last_heartbeat;
    uint64_t last_committed_log_id;
} ArbiterNodeInfo;

typedef struct {
    char device_id[DEVICE_ID_MAX_LEN];
    NodeRole role;
    uint32_t epoch;
} DeviceRole;

/* Protocol conversion function declarations */
void protocol_hton_header(MessageHeader *hdr);
void protocol_ntoh_header(MessageHeader *hdr);
void protocol_hton_bus_login(BusLoginMessage *msg);
void protocol_ntoh_bus_login(BusLoginMessage *msg);
void protocol_hton_heartbeat(HeartbeatMessage *msg);
void protocol_ntoh_heartbeat(HeartbeatMessage *msg);
void protocol_hton_heartbeat_ack(HeartbeatAckMessage *msg);
void protocol_ntoh_heartbeat_ack(HeartbeatAckMessage *msg);
void protocol_hton_failover_cmd(FailoverCommandMessage *msg);
void protocol_ntoh_failover_cmd(FailoverCommandMessage *msg);
void protocol_hton_failover_ack(FailoverAckMessage *msg);
void protocol_ntoh_failover_ack(FailoverAckMessage *msg);
void protocol_hton_device_reg(DeviceRegisterMessage *msg);
void protocol_ntoh_device_reg(DeviceRegisterMessage *msg);
void protocol_hton_device_role_assign(DeviceRoleAssignMessage *msg);
void protocol_ntoh_device_role_assign(DeviceRoleAssignMessage *msg);
void protocol_hton_role_change(RoleChangeMessage *msg);
void protocol_ntoh_role_change(RoleChangeMessage *msg);
void protocol_hton_device_data(DeviceDataPacketMessage *msg);
void protocol_ntoh_device_data(DeviceDataPacketMessage *msg);
void protocol_hton_sync_entry(BusSyncEntryMessage *msg);
void protocol_ntoh_sync_entry(BusSyncEntryMessage *msg);
void protocol_hton_bus_ack(BusAckMessage *msg);
void protocol_ntoh_bus_ack(BusAckMessage *msg);
void protocol_hton_login_ack(LoginAckMessage *msg);
void protocol_ntoh_login_ack(LoginAckMessage *msg);
void protocol_hton_degrade_cmd(DegradeCommandMessage *msg);
void protocol_ntoh_degrade_cmd(DegradeCommandMessage *msg);
void protocol_hton_sync_request(SyncRequestMessage *msg);
void protocol_ntoh_sync_request(SyncRequestMessage *msg);
void protocol_hton_sync_ok(SyncOkMessage *msg);
void protocol_ntoh_sync_ok(SyncOkMessage *msg);

/* Pure C11 64-bit byte swap fallbacks */
uint64_t c11_htobe64(uint64_t val);
uint64_t c11_be64toh(uint64_t val);