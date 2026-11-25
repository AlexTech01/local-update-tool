#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Ounce 23.0.1: LRP +0xB17E0/+0xB1A80/+0xB3420/+0xB3620.
   All sizes are bounded before using peer data. */
enum {
    LabControlPort = 55555, LabContentPort = 55556,
    LabControlMaxBody = 0x10000, LabDeliveryRecordSize = 0x100,
    LabDeliveryMaxRecords = 16, LabDeliveryMaxKeys = 18, LabApplicationSummarySize = 0x80,
    LabJoinPrefixSize = 0x188, LabApplicationPrefixSize = 0x88,
};
typedef struct {
    uint8_t protocol;
    uint8_t stage;
    char nickname[0x81];
    uint8_t system_info[0x100];
    uint8_t summary[0x80];
    uint8_t delivery[LabDeliveryMaxRecords][LabDeliveryRecordSize];
    unsigned delivery_count;
} LabDeliveryPeer;
uint32_t labReadBe32(const void *data);
uint32_t labReadLe32(const void *data);
uint64_t labReadLe64(const void *data);
void labWriteBe32(void *data, uint32_t value);
void labWriteBe64(void *data, uint64_t value);
void labWriteLe64(void *data, uint64_t value);
void labControlHeader(uint8_t out[8], uint8_t type, uint32_t size);
bool labParseControlHeader(const uint8_t in[8], uint8_t *type, size_t *size);
bool labParseJoin(LabDeliveryPeer *out, const uint8_t *body, size_t size,
                  uint8_t protocol, uint64_t application_id);
size_t labNetworkBody(uint8_t *out, size_t capacity, const LabDeliveryPeer *peer,
                      uint32_t host_node, uint32_t peer_node, const char *host_name, const char *peer_name);
void labSendApplicationBody(uint8_t out[0x18], uint32_t receiver_ip,
                           uint32_t receiver_node, uint64_t application_id);
bool labValidatePatchKeys(const void *keys, unsigned count, uint64_t application_id);
/* Reads Patch ID/version from NS-authenticated donor delivery records. This
   helper does not authenticate records or alter their HMAC/firmware fields. */
bool labPatchKeysFromDelivery(void *keys, unsigned capacity, unsigned *count,
                              const void *records, unsigned record_count, uint64_t application_id);
bool labParseSenderResponse(void *keys, unsigned capacity, unsigned *count,
                            const uint8_t *body, size_t size, uint64_t application_id);
bool labPatchKeySetsEqual(const void *left, unsigned left_count, const void *right, unsigned right_count);
