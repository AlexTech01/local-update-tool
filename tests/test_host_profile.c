#include "host_profile.h"
#include "delivery_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv) {
    assert(argc==2);
    uint8_t control[0x4000]={0},summary[0x80],advert[0x100];
    const uint64_t application=UINT64_C(0x01003a400c3da000);
    const uint8_t attribute[]={0x44,0x33,0x22,0x11};
    memcpy(control+0x3028,attribute,4);memcpy(control+0x3060,"1.2.3",6);
    assert(labBuildApplicationSummary(summary,application,control,sizeof(control),UINT64_C(0x0102030405060708)));
    assert(labReadLe64(summary)==application && !memcmp(summary+8,attribute,4));
    assert(labReadLe64(summary+0x10)==UINT64_C(0x0102030405060708));
    assert(!memcmp(summary+0x18,"1.2.3",6));
    assert(labBuildHostAdvertisement(advert,summary,0xa1b2c3d4,2,"Alex"));
    const uint8_t prefix[]={0x13,1,1,2,1};assert(!memcmp(advert,prefix,5));
    const uint8_t nonce[]={0xd4,0xc3,0xb2,0xa1};assert(!memcmp(advert+0x10,nonce,4));
    assert(!strcmp((char *)advert+0x17,"Alex"));
    const uint8_t entry[]={4,0,0,0,0x11,0x22,0x33,0x44,1,0,0x3a,0x40,0x0c,0x3d,0xa0,0};
    assert(!memcmp(advert+0xd8,entry,sizeof(entry)));
    assert(!memcmp(advert+0xf0,"1.2.3",6));
    for(unsigned i=0x98;i<0xd8;i++)assert(!advert[i]);
    labUpdateHostApplication(advert,summary);
    const uint8_t estimate[]={1,2,3,4,5,6,7,8};
    assert(!memcmp(advert+0xe8,estimate,8));
    labUpdateHostProgress(advert,0x11223344,0x55667788,true,true,true);
    const uint8_t results[]={1,0,0,0,0x11,0x22,0x33,0x44,0,0,0,0,0x55,0x66,0x77,0x88};
    assert(!memcmp(advert+0x98,results,sizeof(results)) && advert[5]==1 && advert[6]==2);
    FILE *fixture=fopen(argv[1],"r");assert(fixture);
    unsigned byte=0;size_t at=0;
    while(fscanf(fixture,"%2x",&byte)==1){assert(at<0x40 && advert[0x98+at]==byte);at++;}
    assert(at==0x40);fclose(fixture);
    for(unsigned i=0xa8;i<0xd8;i++)assert(!advert[i]);
    labUpdateHostProgress(advert,1,2,false,false,false);
    assert(!advert[5] && !advert[6]);
    for(unsigned i=0x98;i<0xd8;i++)assert(!advert[i]);
    assert(!labBuildApplicationSummary(summary,application,control,0x3fff,0));
    assert(!labBuildHostAdvertisement(advert,summary,1,9,"Alex"));
    assert(!labBuildHostAdvertisement(advert,summary,1,1,"Alex"));
    uint8_t records[2*0x100]={0};records[0x22]=records[0x122]=1;
    assert(labDeliveryProtocol(records,2)==4);
    records[0x122]=0;assert(labDeliveryProtocol(records,2)==1);
    assert(labDeliveryProtocol(NULL,0)==4 && labDeliveryProtocol(records,17)==0);
    puts("PASS: official host advertisement, application summary, defaults and protocol selection");
}
