// protocol.c — Network byte-order conversion for message types
#include "common/protocol.h"
#include <arpa/inet.h>

// Convert 64-bit value from host to big-endian byte order
uint64_t c11_htobe64(uint64_t val) {
    uint32_t lo = htonl((uint32_t)val);
    uint32_t hi = htonl((uint32_t)(val >> 32));
    return ((uint64_t)lo << 32) | hi;
}

// Convert 64-bit value from big-endian to host byte order
uint64_t c11_be64toh(uint64_t val) {
    uint32_t lo = ntohl((uint32_t)(val >> 32));
    uint32_t hi = ntohl((uint32_t)val);
    return ((uint64_t)lo << 32) | hi;
}

// ======== hton/ntoh message conversion ========
void protocol_hton_header(MessageHeader *hdr) {
    hdr->type = htons(hdr->type);
    hdr->length = htons(hdr->length);
    hdr->timestamp = c11_htobe64(hdr->timestamp);
}

void protocol_ntoh_header(MessageHeader *hdr) {
    hdr->type = ntohs(hdr->type);
    hdr->length = ntohs(hdr->length);
    hdr->timestamp = c11_be64toh(hdr->timestamp);
}

void protocol_hton_bus_login(BusLoginMessage *msg) {
    protocol_hton_header(&msg->header);
    msg->epoch = htonl(msg->epoch);
    msg->last_committed_log_id = c11_htobe64(msg->last_committed_log_id);
}

void protocol_ntoh_bus_login(BusLoginMessage *msg) {
    protocol_ntoh_header(&msg->header);
    msg->epoch = ntohl(msg->epoch);
    msg->last_committed_log_id = c11_be64toh(msg->last_committed_log_id);
}

void protocol_hton_heartbeat(HeartbeatMessage *msg) {
    protocol_hton_header(&msg->header);
    msg->epoch = htonl(msg->epoch);
}

void protocol_ntoh_heartbeat(HeartbeatMessage *msg) {
    protocol_ntoh_header(&msg->header);
    msg->epoch = ntohl(msg->epoch);
}

void protocol_hton_heartbeat_ack(HeartbeatAckMessage *msg) {
    protocol_hton_header(&msg->header);
    msg->timestamp = c11_htobe64(msg->timestamp);
}

void protocol_ntoh_heartbeat_ack(HeartbeatAckMessage *msg) {
    protocol_ntoh_header(&msg->header);
    msg->timestamp = c11_be64toh(msg->timestamp);
}

void protocol_hton_failover_cmd(FailoverCommandMessage *msg) {
    protocol_hton_header(&msg->header);
    msg->epoch = htonl(msg->epoch);
}

void protocol_ntoh_failover_cmd(FailoverCommandMessage *msg) {
    protocol_ntoh_header(&msg->header);
    msg->epoch = ntohl(msg->epoch);
}

void protocol_hton_failover_ack(FailoverAckMessage *msg) {
    protocol_hton_header(&msg->header);
    msg->epoch = htonl(msg->epoch);
}

void protocol_ntoh_failover_ack(FailoverAckMessage *msg) {
    protocol_ntoh_header(&msg->header);
    msg->epoch = ntohl(msg->epoch);
}

void protocol_hton_device_reg(DeviceRegisterMessage *msg) {
    protocol_hton_header(&msg->header);
}

void protocol_ntoh_device_reg(DeviceRegisterMessage *msg) {
    protocol_ntoh_header(&msg->header);
}

void protocol_hton_device_role_assign(DeviceRoleAssignMessage *msg) {
    protocol_hton_header(&msg->header);
    msg->epoch = htonl(msg->epoch);
}

void protocol_ntoh_device_role_assign(DeviceRoleAssignMessage *msg) {
    protocol_ntoh_header(&msg->header);
    msg->epoch = ntohl(msg->epoch);
}

void protocol_hton_role_change(RoleChangeMessage *msg) {
    protocol_hton_header(&msg->header);
    msg->epoch = htonl(msg->epoch);
}

void protocol_ntoh_role_change(RoleChangeMessage *msg) {
    protocol_ntoh_header(&msg->header);
    msg->epoch = ntohl(msg->epoch);
}

void protocol_hton_device_data(DeviceDataPacketMessage *msg) {
    protocol_hton_header(&msg->header);
    msg->message_id = c11_htobe64(msg->message_id);
    msg->payload_size = htonl(msg->payload_size);
}

void protocol_ntoh_device_data(DeviceDataPacketMessage *msg) {
    protocol_ntoh_header(&msg->header);
    msg->message_id = c11_be64toh(msg->message_id);
    msg->payload_size = ntohl(msg->payload_size);
}

void protocol_hton_sync_entry(BusSyncEntryMessage *msg) {
    protocol_hton_header(&msg->header);
    msg->log_id = c11_htobe64(msg->log_id);
    msg->payload_size = htonl(msg->payload_size);
}

void protocol_ntoh_sync_entry(BusSyncEntryMessage *msg) {
    protocol_ntoh_header(&msg->header);
    msg->log_id = c11_be64toh(msg->log_id);
    msg->payload_size = ntohl(msg->payload_size);
}

void protocol_hton_bus_ack(BusAckMessage *msg) {
    protocol_hton_header(&msg->header);
    msg->log_id = c11_htobe64(msg->log_id);
}

void protocol_ntoh_bus_ack(BusAckMessage *msg) {
    protocol_ntoh_header(&msg->header);
    msg->log_id = c11_be64toh(msg->log_id);
}

void protocol_hton_login_ack(LoginAckMessage *msg) {
    protocol_hton_header(&msg->header);
    msg->epoch = htonl(msg->epoch);
}

void protocol_ntoh_login_ack(LoginAckMessage *msg) {
    protocol_ntoh_header(&msg->header);
    msg->epoch = ntohl(msg->epoch);
}

void protocol_hton_degrade_cmd(DegradeCommandMessage *msg) {
    protocol_hton_header(&msg->header);
    msg->epoch = htonl(msg->epoch);
}

void protocol_ntoh_degrade_cmd(DegradeCommandMessage *msg) {
    protocol_ntoh_header(&msg->header);
    msg->epoch = ntohl(msg->epoch);
}

void protocol_hton_sync_request(SyncRequestMessage *msg) {
    protocol_hton_header(&msg->header);
}

void protocol_ntoh_sync_request(SyncRequestMessage *msg) {
    protocol_ntoh_header(&msg->header);
}

void protocol_hton_sync_ok(SyncOkMessage *msg) {
    protocol_hton_header(&msg->header);
    msg->last_committed_log_id = c11_htobe64(msg->last_committed_log_id);
}

void protocol_ntoh_sync_ok(SyncOkMessage *msg) {
    protocol_ntoh_header(&msg->header);
    msg->last_committed_log_id = c11_be64toh(msg->last_committed_log_id);
}
