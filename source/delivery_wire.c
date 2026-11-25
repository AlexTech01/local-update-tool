#include "delivery_wire.h"
#include <string.h>

uint32_t labReadBe32(const void *data) {
    const uint8_t *p = data;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
uint32_t labReadLe32(const void *data) {
    const uint8_t *p = data;
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
uint64_t labReadLe64(const void *data) {
    const uint8_t *p = data;
    return (uint64_t)labReadLe32(p + 4) << 32 | labReadLe32(p);
}
void labWriteBe32(void *data, uint32_t value) {
    uint8_t *p = data;
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (24 - i * 8));
}
void labWriteBe64(void *data, uint64_t value) {
    labWriteBe32(data, (uint32_t)(value >> 32));
    labWriteBe32((uint8_t *)data + 4, (uint32_t)value);
}
void labWriteLe64(void *data, uint64_t value) {
    uint8_t *p = data;
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(value >> (i * 8));
}
void labControlHeader(uint8_t out[8], uint8_t type, uint32_t size) {
    memset(out, 0, 8); out[0] = 0x10; out[1] = type;
    labWriteBe32(out + 4, size);
}
bool labParseControlHeader(const uint8_t in[8], uint8_t *type, size_t *size) {
    if (!in || !type || !size || in[0] != 0x10 || in[2] || in[3]) return false;
    uint32_t length = labReadBe32(in + 4);
    if (length > LabControlMaxBody) return false;
    *type = in[1]; *size = length; return true;
}
bool labParseJoin(LabDeliveryPeer *out, const uint8_t *body, size_t size,
                  uint8_t protocol, uint64_t application_id) {
    if (!out || !body || size < LabJoinPrefixSize + LabApplicationPrefixSize) return false;
    if ((body[0] >> 4) != (protocol >> 4) || body[1] != 1) return false;
    const uint8_t *app = body + LabJoinPrefixSize;
    unsigned count = app[0];
    if (count > LabDeliveryMaxRecords ||
        size != LabJoinPrefixSize + LabApplicationPrefixSize + count * LabDeliveryRecordSize ||
        labReadLe64(app + 8) != application_id) return false;
    /* Keep authenticated records intact. They are opaque, including S2 fields
       that the installed SDK may not describe yet. */
    memset(out, 0, sizeof(*out));
    out->protocol = body[0];
    out->stage = body[2];
    memcpy(out->nickname, body + 3, 0x80);
    memcpy(out->system_info, body + 0x88, 0x100);
    memcpy(out->summary, app + 8, 0x80);
    out->delivery_count = count;
    memcpy(out->delivery, app + LabApplicationPrefixSize, count * LabDeliveryRecordSize);
    return true;
}
static void nodeInfo(uint8_t out[0x8c], uint32_t node, const char *name) {
    memset(out, 0, 0x8c); labWriteBe32(out, node);
    size_t n = 0;
    while (n < 0x80 && name[n]) n++;
    memcpy(out + 0xb, name, n);
}
size_t labNetworkBody(uint8_t *out, size_t capacity, const LabDeliveryPeer *peer,
                      uint32_t host_node, uint32_t peer_node, const char *host_name, const char *peer_name) {
    if (!out || !peer || !host_name || !peer_name || peer->delivery_count > LabDeliveryMaxRecords ||
        !host_node || !peer_node || host_node == peer_node) return 0;
    size_t app_size = LabApplicationPrefixSize + peer->delivery_count * LabDeliveryRecordSize;
    size_t size = 8 + app_size + 2 * 0x8c;
    if (capacity < size) return 0;
    memset(out, 0, size); out[0] = 1; out[1] = 2;
    out[8] = (uint8_t)peer->delivery_count;
    memcpy(out + 16, peer->summary, 0x80);
    memcpy(out + 8 + LabApplicationPrefixSize, peer->delivery,
           peer->delivery_count * LabDeliveryRecordSize);
    nodeInfo(out + 8 + app_size, host_node, host_name);
    nodeInfo(out + 8 + app_size + 0x8c, peer_node, peer_name);
    return size;
}
void labSendApplicationBody(uint8_t out[0x18], uint32_t receiver_ip,
                           uint32_t receiver_node, uint64_t application_id) {
    memset(out, 0, 0x18);
    out[0] = 1; out[1] = 4; out[2] = 0x14;
    labWriteBe32(out + 8, receiver_ip); labWriteBe32(out + 12, receiver_node);
    labWriteBe64(out + 16, application_id);
}
bool labValidatePatchKeys(const void *data, unsigned count, uint64_t application_id) {
    if(!data || !application_id || !count || count>LabDeliveryMaxKeys)return false;
    const uint8_t *keys=data;
    for(unsigned i=0;i<count;i++) {
        const uint8_t *key=keys+i*16;
        if(key[12]!=0x81 || labReadLe64(key)!=(application_id|UINT64_C(0x800)))return false;
        for(unsigned j=0;j<i;j++)if(!memcmp(key,keys+j*16,16))return false;
    }
    return true;
}
bool labPatchKeysFromDelivery(void *keys, unsigned capacity, unsigned *count,
                              const void *records, unsigned record_count, uint64_t application_id) {
    if(count)*count=0;
    if(!keys || !count || !records || !application_id || !record_count ||
       record_count>LabDeliveryMaxRecords || record_count>capacity)return false;
    uint8_t selected[LabDeliveryMaxRecords*16]={0};
    const uint8_t *info=records;
    for(unsigned i=0;i<record_count;i++) {
        const uint8_t *record=info+(size_t)i*LabDeliveryRecordSize;
        uint32_t attributes=labReadLe32(record+0x1c);
        /* NS +0x22D54C copies the selected content-meta version to +0x10
           and sets bit 30 for Patch metadata. Bit 1 requests patch delivery;
           bit 0 denotes a different delivery mode. Require the mapped form. */
        if(labReadLe64(record+8)!=application_id || (attributes&1) ||
           (attributes&UINT32_C(0x40000002))!=UINT32_C(0x40000002))return false;
        uint8_t *key=selected+(size_t)i*16;
        labWriteLe64(key,application_id|UINT64_C(0x800));
        memcpy(key+8,record+0x10,4);key[12]=0x81;
    }
    if(!labValidatePatchKeys(selected,record_count,application_id))return false;
    memcpy(keys,selected,(size_t)record_count*16);*count=record_count;return true;
}
bool labParseSenderResponse(void *keys, unsigned capacity, unsigned *count,
                            const uint8_t *body, size_t size, uint64_t application_id) {
    if(!keys || !count || !body || size<8 || body[0]!=2 || !body[1] || body[1]>capacity ||
       body[1]>LabDeliveryMaxKeys || size!=8+(size_t)body[1]*16)return false;
    if(!labValidatePatchKeys(body+8,body[1],application_id))return false;
    memcpy(keys,body+8,(size_t)body[1]*16);*count=body[1];return true;
}
bool labPatchKeySetsEqual(const void *left, unsigned left_count, const void *right, unsigned right_count) {
    if(!left || !right || !left_count || left_count!=right_count || left_count>LabDeliveryMaxKeys)return false;
    const uint8_t *a=left,*b=right;bool used[LabDeliveryMaxKeys]={0};
    for(unsigned i=0;i<left_count;i++) {
        unsigned j=0;
        while(j<right_count && (used[j] || memcmp(a+i*16,b+j*16,16)))j++;
        if(j==right_count)return false;
        used[j]=true;
    }
    return true;
}
