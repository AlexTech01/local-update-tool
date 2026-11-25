#include "delivery_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint64_t app = UINT64_C(0x0400000012340000);
static size_t readFixture(const char *path,uint8_t *out,size_t capacity) {
    FILE *file=fopen(path,"r");assert(file);unsigned byte;size_t size=0;
    while(fscanf(file,"%2x",&byte)==1){assert(size<capacity);out[size++]=(uint8_t)byte;}
    fclose(file);return size;
}
int main(int argc,char **argv) {
    assert(argc==4);
    uint8_t header[16], type; size_t size;
    labControlHeader(header, 20, 0x18);
    const uint8_t expected_header[] = {0x10, 0x14, 0, 0, 0, 0, 0, 0x18};
    assert(!memcmp(header, expected_header, 8));
    assert(labParseControlHeader(header, &type, &size) && type == 20 && size == 24);
    header[4] = 1; assert(!labParseControlHeader(header, &type, &size));
    header[4] = 0; header[2] = 1; assert(!labParseControlHeader(header, &type, &size));

    uint8_t send_body[24]; labSendApplicationBody(send_body, 0xa9fe0101, 1, app);
    const uint8_t expected_body[] = {1,4,0x14,0,0,0,0,0,0xa9,0xfe,1,1,0,0,0,1,4,0,0,0,0x12,0x34,0,0};
    assert(!memcmp(send_body, expected_body, sizeof(expected_body)));

    uint8_t join[LabJoinPrefixSize + LabApplicationPrefixSize + LabDeliveryRecordSize] = {0};
    join[0] = 0x13; join[1] = 1;
    memcpy(join + 3, "Donor", 5);
    join[0x88] = 0xa5;
    join[LabJoinPrefixSize] = 1;
    labWriteLe64(join + LabJoinPrefixSize + 8, app);
    memset(join + LabJoinPrefixSize + LabApplicationPrefixSize, 0x5a, LabDeliveryRecordSize);
    LabDeliveryPeer peer;
    assert(labParseJoin(&peer, join, sizeof(join), 0x13, app));
    assert(!strcmp(peer.nickname, "Donor") && peer.system_info[0] == 0xa5);
    assert(peer.delivery_count == 1 && peer.delivery[0][0xff] == 0x5a);
    for (size_t i = 0; i < sizeof(join); i++) assert(!labParseJoin(&peer, join, i, 0x13, app));
    assert(!labParseJoin(&peer, join, sizeof(join), 0x23, app));
    assert(!labParseJoin(&peer, join, sizeof(join), 0x13, app + 1));
    join[LabJoinPrefixSize] = 17; assert(!labParseJoin(&peer, join, sizeof(join), 0x13, app));
    join[LabJoinPrefixSize] = 1;
    join[2]=1;
    assert(labParseJoin(&peer, join, sizeof(join), 0x13, app));
    assert(peer.stage==1);
    uint8_t network[0x2000];
    size_t used = labNetworkBody(network, sizeof(network), &peer, 1, 2, "Receiver", "Donor");
    assert(used == 8 + 0x188 + 2 * 0x8c);
    assert(network[0] == 1 && network[1] == 2 && network[8] == 1);
    assert(labReadLe64(network + 16) == app);
    assert(!memcmp(network + 8 + 0x88, peer.delivery, 0x100));
    assert(labReadBe32(network + 8 + 0x188) == 1);
    assert(labReadBe32(network + 8 + 0x188 + 0x8c) == 2);
    assert(!strcmp((char *)network + 8 + 0x188 + 0x8c + 0xb, "Donor"));
    assert(!labNetworkBody(network, used - 1, &peer, 1, 2, "Receiver", "Donor"));

    /* Independent fixture emitted by the supplied firmware's actual serializer
       under emulation, rather than an encoder/decoder round trip. */
    LabDeliveryPeer fixture_peer={.protocol=0x13,.delivery_count=1};
    labWriteLe64(fixture_peer.summary,UINT64_C(0x01003a400c3da000));
    memcpy(fixture_peer.summary+0x18,"2.0.0",6);
    for(unsigned i=0;i<256;i++)fixture_peer.delivery[0][i]=(uint8_t)i;
    used=labNetworkBody(network+8,sizeof(network)-8,&fixture_peer,0x11223344,0x55667788,"Receiver","Donor");
    labControlHeader(network,9,(uint32_t)used);
    FILE *fixture=fopen(argv[1],"r");assert(fixture);
    unsigned byte=0;size_t at=0;
    while(fscanf(fixture,"%2x",&byte)==1){assert(at<used+8 && network[at]==byte);at++;}
    assert(at==used+8);fclose(fixture);

    uint8_t reply[8+18*16]={2,1},keys[18*16];unsigned key_count=0;
    labWriteLe64(reply+8,app|0x800);reply[20]=0x81;
    assert(labParseSenderResponse(keys,18,&key_count,reply,24,app) && key_count==1);
    reply[20]=3;assert(!labParseSenderResponse(keys,18,&key_count,reply,24,app));
    reply[20]=0x81;reply[1]=2;
    assert(!labParseSenderResponse(keys,18,&key_count,reply,24,app));
    memcpy(reply+24,reply+8,16);reply[32]=1;
    assert(labParseSenderResponse(keys,18,&key_count,reply,40,app) && key_count==2);
    assert(!labParseSenderResponse(keys,1,&key_count,reply,40,app));
    for(size_t n=0;n<40;n++)assert(!labParseSenderResponse(keys,18,&key_count,reply,n,app));
    uint8_t reversed[32];memcpy(reversed,keys+16,16);memcpy(reversed+16,keys,16);
    assert(labPatchKeySetsEqual(keys,2,reversed,2));
    assert(!labPatchKeySetsEqual(keys,2,reversed,1));
    reversed[8]++;assert(!labPatchKeySetsEqual(keys,2,reversed,2));
    memcpy(reply+24,reply+8,16);assert(!labParseSenderResponse(keys,18,&key_count,reply,40,app));
    for(unsigned i=0;i<18;i++) {
        labWriteLe64(reply+8+i*16,app|0x800);reply[8+i*16+8]=(uint8_t)i;reply[8+i*16+12]=0x81;
    }
    reply[1]=18;assert(labParseSenderResponse(keys,18,&key_count,reply,sizeof(reply),app));
    for(unsigned i=0;i<18;i++) {
        reply[8+i*16+12]=3;assert(!labParseSenderResponse(keys,18,&key_count,reply,sizeof(reply),app));
        reply[8+i*16+12]=0x81;
    }
    reply[1]=19;assert(!labParseSenderResponse(keys,18,&key_count,reply,sizeof(reply),app));
    /* Actual application metadata from the failing 0.2.8 run: the remote
       record and sender agree on A4, whereas command 2007 returned local A3. */
    const uint64_t minecraft=UINT64_C(0x0100d71004694000);
    uint8_t record[256],before[256],offered[24],derived[18*16];
    assert(readFixture(argv[2],record,sizeof(record))==sizeof(record));
    assert(readFixture(argv[3],offered,sizeof(offered))==sizeof(offered));
    memcpy(before,record,sizeof(record));
    assert(labPatchKeysFromDelivery(derived,18,&key_count,record,1,minecraft) && key_count==1);
    assert(!memcmp(derived,offered+8,16) && labReadLe32(derived+8)==0xa40000);
    assert(!memcmp(record,before,sizeof(record))); /* No field/HMAC rewriting. */
    for(unsigned fault=0;fault<5;fault++) {
        memcpy(record,before,sizeof(record));
        if(fault==0)record[8]^=1;
        if(fault==1)record[0x1f]&=0xbf; /* Missing Patch source flag. */
        if(fault==2)record[0x1c]&=0xfd; /* No patch delivery requested. */
        if(fault==3)record[0x1c]|=1;    /* Different delivery mode. */
        assert(!labPatchKeysFromDelivery(derived,18,&key_count,record,1,
            fault==4?minecraft+1:minecraft) && !key_count);
    }
    memcpy(record,before,sizeof(record));
    assert(!labPatchKeysFromDelivery(derived,0,&key_count,record,1,minecraft) && !key_count);
    assert(!labPatchKeysFromDelivery(derived,18,&key_count,record,0,minecraft));
    assert(!labPatchKeysFromDelivery(derived,18,&key_count,record,17,minecraft));
    assert(!labPatchKeysFromDelivery(NULL,18,&key_count,record,1,minecraft));
    assert(!labPatchKeysFromDelivery(derived,18,NULL,record,1,minecraft));
    assert(!labPatchKeysFromDelivery(derived,18,&key_count,NULL,1,minecraft));
    uint8_t multiple[LabDeliveryMaxRecords*256];
    for(unsigned i=0;i<LabDeliveryMaxRecords;i++) {
        memcpy(multiple+i*256,record,256);multiple[i*256+0x10]=(uint8_t)i;
    }
    assert(labPatchKeysFromDelivery(derived,18,&key_count,multiple,LabDeliveryMaxRecords,minecraft));
    assert(key_count==LabDeliveryMaxRecords);
    memcpy(multiple+256,multiple,256);
    assert(!labPatchKeysFromDelivery(derived,18,&key_count,multiple,2,minecraft) && !key_count);
    puts("PASS: S2 framing, joins, sender roles, key-set matching, bounds and system-update-key rejection");
}
