#include "host_profile.h"
#include "delivery_wire.h"
#include <string.h>

bool labBuildApplicationSummary(uint8_t out[LabApplicationSummaryBytes], uint64_t application_id,
                               const void *control, size_t control_size, uint64_t estimated_size) {
    if (!out || !application_id || !control || control_size < 0x4000) return false;
    const uint8_t *nacp=control;
    /* LRP +0xAE300: application ID, NACP attribute, estimated size, display version. */
    memset(out,0,LabApplicationSummaryBytes);
    labWriteLe64(out,application_id);
    memcpy(out+8,nacp+0x3028,4);
    labWriteLe64(out+0x10,estimated_size);
    memcpy(out+0x18,nacp+0x3060,0x10);
    return true;
}
bool labBuildHostAdvertisement(uint8_t out[LabAdvertisementSize], const uint8_t summary[LabApplicationSummaryBytes],
                               uint32_t room_nonce, unsigned max_players, const char *nickname) {
    if (!out || !summary || !nickname || !labReadLe64(summary) || max_players<2 || max_players>8) return false;
    /* LRP +0xA0700. Reserved bytes stay zero, including completion data +0x98. */
    memset(out,0,LabAdvertisementSize);
    out[0]=LabRoomProtocol;out[1]=1;out[2]=1;out[3]=(uint8_t)max_players;out[4]=1;
    for(unsigned i=0;i<4;i++)out[0x10+i]=(uint8_t)(room_nonce>>(8*i));
    size_t length=0;while(length<0x80 && nickname[length])length++;
    memcpy(out+0x17,nickname,length);
    out[0xd8]=4;
    labWriteBe32(out+0xdc,labReadLe32(summary+8));
    labWriteBe64(out+0xe0,labReadLe64(summary));
    memcpy(out+0xf0,summary+0x18,0x10);
    return true;
}
unsigned labDeliveryProtocol(const void *records,unsigned count) {
    if ((!records && count) || count>LabDeliveryMaxRecords)return 0;
    const uint8_t *p=records;
    for(unsigned i=0;i<count;i++)if(p[i*LabDeliveryRecordSize+0x22]!=1)return 1;
    return 4;
}
void labUpdateHostApplication(uint8_t advertise[LabAdvertisementSize], const uint8_t summary[LabApplicationSummaryBytes]) {
    labWriteBe64(advertise+0xe8,labReadLe64(summary+0x10));
    memcpy(advertise+0xf0,summary+0x18,0x10);
}
void labUpdateHostProgress(uint8_t advertise[LabAdvertisementSize], uint32_t host_node,
    uint32_t peer_node, bool started, bool receiving, bool complete) {
    advertise[5]=complete?1:0;
    advertise[6]=receiving?2:0;
    memset(advertise+0x98,0,0x40);
    if(started) {
        advertise[0x98]=complete?1:0;
        labWriteBe32(advertise+0x9c,host_node);
        labWriteBe32(advertise+0xa4,peer_node);
    }
}
