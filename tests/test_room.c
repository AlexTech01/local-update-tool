/* Exercise the actual coordinator with mocked IPC and real local socket streams. */
#include <assert.h>
#include <stdarg.h>
#include <sys/socket.h>
static int testSetSockOpt(int,int,int,const void *,socklen_t);
#define setsockopt testSetSockOpt
#include "../source/game_room.c"
#undef setsockopt

static const u64 test_app=UINT64_C(0x01003a400c3da000);
static int client_fd=-1;
static u64 tick;
static s32 comparison;
static bool complete,async_ready,wrong_keys,received_task;
static unsigned requests,commits,cancels,content_queries;
static unsigned send_key_queries;
static unsigned avm_updates, ns_updates;
static bool avm_open, manager_open;
static Result commit_rc, ns_update_rc;
static u32 local_version,donor_version;
static Result self_compare_rc;
static unsigned vendor_options,reuse_options;
static u32 linked_peer=0xa9fe0102;
static u8 advertisement[256];
static char sender_log[8192];
static size_t sender_log_size,reply_log_size;
static u8 reply_log[8+LabDeliveryMaxKeys*16];
static int testSetSockOpt(int fd,int level,int option,const void *value,socklen_t length) {
    assert(level==SOL_SOCKET);
    /* Model the observed BSD rejection, rather than silently accepting every
       vendor option. 0.2.3 passed 0x80000000 for accepted connections. */
    if((u32)option==UINT32_C(0x80000000)) {errno=10092;return -1;}
    if((u32)option==UINT32_C(0x80000001)) {
        const u32 expected[2]={1,1};
        assert(length==sizeof(expected) && !memcmp(value,expected,sizeof(expected)));
        vendor_options++;return 0;
    }
    assert(option==SO_REUSEADDR && length==sizeof(int) && *(const int *)value==1);
    reuse_options++;return setsockopt(fd,level,option,value,length);
}
static NcmContentMetaKey patch(void) {
    return (NcmContentMetaKey){.id=test_app|0x800,.version=donor_version+(wrong_keys?1:0),.type=0x81};
}
void labLog(const char *format,...) {
    if(strncmp(format,"Sender ",7) && strncmp(format,"%s[%u]",6))return;
    assert(sender_log_size<sizeof(sender_log));
    va_list args;va_start(args,format);
    int n=vsnprintf(sender_log+sender_log_size,sizeof(sender_log)-sender_log_size,format,args);
    va_end(args);
    assert(n>=0 && (size_t)n+1<sizeof(sender_log)-sender_log_size);
    sender_log_size+=(size_t)n;sender_log[sender_log_size++]='\n';sender_log[sender_log_size]=0;
}
void labLogResult(const char *name,Result rc) {(void)name;(void)rc;}
void labLogBytes(const char *name,const void *data,size_t size) {
    if(strcmp(name,"Game room sender response body"))return;
    assert(size<=sizeof(reply_log));reply_log_size=size;
    if(size)memcpy(reply_log,data,size);
}
u64 armGetSystemTick(void) {return tick;}
u64 armNsToTicks(u64 ns) {return ns;}
void randomGet(void *out,size_t size) {memset(out,1,size);}
SmServiceName smEncodeName(const char *name) {SmServiceName out=0;memcpy(&out,name,strlen(name)>8?8:strlen(name));return out;}
bool smServiceNamesAreEqual(SmServiceName a,SmServiceName b) {return a==b;}
Result __real_smGetServiceWrapper(Service *out,SmServiceName name) {(void)out;(void)name;return 1;}
Result labGetService(Service *sm,const char *name,Service *out) {(void)sm;(void)name;(void)out;return 1;}
Result avmInitialize(void) {
    assert(commits == 1 && !avm_open);
    avm_open = true;
    return 0;
}
void avmExit(void) {
    assert(avm_open && !manager_open);
    avm_open = false;
}
Result nsGetApplicationManagerInterface(Service *out) {
    assert(avm_open && !manager_open);
    *out = (Service){.active=true, .kind=36};
    manager_open = true;
    return 0;
}
void serviceClose(Service *service) {
    assert(manager_open && service->active && service->kind == 36);
    manager_open = false;
    service->active = false;
}
Result avmPushLaunchVersion(u64 app, u32 version) {
    assert(avm_open && manager_open && app == test_app && version == 0 && !ns_updates);
    avm_updates++;
    return 0;
}
Result serviceDispatchImpl(Service *service, unsigned cmd, const void *input, size_t size,
    void *output, size_t output_size, SfDispatchParams params) {
    assert(manager_open && service->active && service->kind == 36 && cmd == 36);
    assert(!output && !output_size && !params.out_num_objects && avm_updates == 1);
    assert(size == 24 && labReadLe64(input) == 0);
    assert(labReadLe64((const u8 *)input + 8) == test_app);
    assert(labReadLe64((const u8 *)input + 16) == 0);
    ns_updates++;
    return ns_update_rc;
}
Result nsVerifyDeliveryProtocolVersion(const NsSystemDeliveryInfo *info) {assert(info);return 0;}
Result nsCompareApplicationDeliveryInfo(const NsApplicationDeliveryInfo *a,s32 ac,
    const NsApplicationDeliveryInfo *b,s32 bc,s32 *out) {
    assert(a && b && ac==1 && bc==1);*out=a==b?0:comparison;return a==b?self_compare_rc:0;
}
Result nsHasAllContentsToDeliver(const NsApplicationDeliveryInfo *info,s32 count,bool *out) {
    assert(info && count==1);content_queries++;*out=complete;return 0;
}
Result nsListContentMetaKeyToDeliverApplication(NcmContentMetaKey *out,s32 cap,s32 index,
    const NsApplicationDeliveryInfo *info,s32 count,s32 *total) {
    /* Actual NS command 2007 reads the LOCAL installed Patch after validating
       the input record. A mock returning the donor version hid the old bug. */
    assert(out && cap==18 && !index && info && count==1);
    *out=patch();out->version=local_version;*total=1;send_key_queries++;return 0;
}
Result nsRequestReceiveApplication(AsyncResult *out,u32 ip,u16 port,u64 app,
    const NcmContentMetaKey *keys,s32 count,NcmStorageId storage) {
    assert(ip==0xa9fe0102 && port==55556 && app==test_app && count==1 && storage==6);
    NcmContentMetaKey expected=patch();assert(!memcmp(keys,&expected,16));
    requests++;out->s.active=true;return 0;
}
Result nsCommitReceiveApplication(u64 app) {
    assert(app==test_app);commits++;if(!received_task)return 0x6cc10;
    received_task=false;return commit_rc;
}
Result nsGetReceiveApplicationProgress(u64 app,NsReceiveApplicationProgress *out) {
    assert(app==test_app);*out=(NsReceiveApplicationProgress){32,64};return 0;
}
Result asyncResultWait(AsyncResult *a,u64 timeout) {assert(a->s.active && !timeout);return async_ready?0:KERNELRESULT(TimedOut);}
Result asyncResultGet(AsyncResult *a) {assert(a->s.active && async_ready);received_task=true;return 0;}
void asyncResultClose(AsyncResult *a) {
    assert(a->s.active);if(!async_ready)cancels++;received_task=false;a->s.active=false;
}
Result nsListApplicationContentMetaStatus(u64 app,s32 index,NsApplicationContentMetaStatus *out,s32 cap,s32 *total) {
    assert(app==test_app && !index && out && cap==32);*total=0;return 0;
}
Result ldnSetAdvertiseData(const void *data,size_t size) {assert(size==256);memcpy(advertisement,data,size);return 0;}
Result ldnSetStationAcceptPolicy(int policy) {assert(policy==LdnAcceptPolicy_AlwaysReject);return 0;}
Result ldnGetState(LdnState *out) {*out=LdnState_AccessPointCreated;return 0;}
Result ldnGetNetworkInfo(LdnNetworkInfo *out) {
    memset(out,0,sizeof(*out));out->nodes[0].is_connected=out->nodes[1].is_connected=true;
    out->nodes[1].ip_addr.addr=linked_peer;return 0;
}
Result socketGetLastResult(void) {return 0;}
void socketExit(void) {}
void ldnExit(void) {}
Result ldnDestroyNetwork(void) {return 0;}
Result ldnCloseAccessPoint(void) {return 0;}
void nsRequestServerStopperClose(NsRequestServerStopper *s) {(void)s;}

/* Unused room-open platform boundaries; setup below starts with an established
   stream so failures are localized to the management state machine. */
Result nsGetApplicationDeliveryInfo(NsApplicationDeliveryInfo *o,s32 c,u64 a,u32 attr,s32 *n) {(void)o;(void)c;(void)a;(void)attr;(void)n;assert(false);return 1;}
Result nsGetApplicationControlData2(NsApplicationControlSource s,u64 a,void *o,size_t c,u32 p,u32 i,u64 *n,u32 *x) {(void)s;(void)a;(void)o;(void)c;(void)p;(void)i;(void)n;(void)x;assert(false);return 1;}
Result nsEstimateRequiredSize(const NcmContentMetaKey *k,s32 c,s64 *o) {(void)k;(void)c;(void)o;assert(false);return 1;}
Result nsGetRequestServerStopper(NsRequestServerStopper *s) {(void)s;assert(false);return 1;}
Result ldnInitialize(int t) {(void)t;assert(false);return 1;}
Result ldnSetProtocol(LdnProtocol p) {(void)p;assert(false);return 1;}
Result ldnSetOperationMode(int m) {(void)m;assert(false);return 1;}
Result ldnOpenAccessPoint(void) {assert(false);return 1;}
Result ldnGetIpv4Address(LdnIpv4Address *a,LdnSubnetMask *m) {(void)a;(void)m;assert(false);return 1;}
Result labCreateGameNetwork(Service *s,const LdnSecurityConfig *c,const LdnUserConfig *u,const LdnNetworkConfig *n) {(void)s;(void)c;(void)u;(void)n;assert(false);return 1;}
const SocketInitConfig *socketGetDefaultInitConfig(void) {static SocketInitConfig c;return &c;}
Result socketInitialize(const SocketInitConfig *c) {(void)c;assert(false);return 1;}

static void cleanup(void) {
    labGameRoomClose();
    if(client_fd>=0)close(client_fd);
    client_fd=-1;
}
static void setup(void) {
    cleanup();int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    assert(nonblocking(pair[0]) && nonblocking(pair[1]));
    memset(&s_room,0,sizeof(s_room));s_room.listener=-1;s_room.peer=pair[0];client_fd=pair[1];
    s_room.application_id=test_app;s_room.host_node=0x11223344;s_room.peer_node=0x55667788;
    s_room.local_ip=0xa9fe0101;s_room.peer_ip=0xa9fe0102;s_room.advertise_size=256;
    s_room.advertise[0]=0x13;s_room.advertise[1]=1;s_room.advertise[2]=1;
    s_room.status.open=true;
    strcpy(s_room.status.local_name,"Receiver");
    s_room.local_delivery_count=1;labWriteLe64(s_room.local_summary,test_app);
    s_room.local_summary[8]=0x11;memcpy(s_room.local_summary+0x18,"2.0.0",6);
    s_room.next_link_check=UINT64_MAX;
    tick=0;comparison=0;complete=true;async_ready=wrong_keys=received_task=false;
    requests=commits=cancels=content_queries=0;
    avm_updates=ns_updates=0;
    commit_rc=ns_update_rc=0;
    assert(!avm_open && !manager_open);
    send_key_queries=0;local_version=donor_version=0x40000;self_compare_rc=0;
    sender_log_size=reply_log_size=0;sender_log[0]=0;memset(reply_log,0,sizeof(reply_log));
    linked_peer=0xa9fe0102;vendor_options=reuse_options=0;
}
static size_t drain(u8 *out,size_t capacity) {
    size_t total=0;
    for(;;) {
        ssize_t n=recv(client_fd,out+total,capacity-total,0);
        if(n<0){assert(errno==EAGAIN || errno==EWOULDBLOCK);return total;}
        assert(n>0);total+=(size_t)n;assert(total<capacity);
    }
}
static size_t receiveUntil(u8 *out,size_t capacity,size_t used,size_t target) {
    assert(target<=capacity);
    while(used<target) {
        struct pollfd pending={.fd=client_fd,.events=POLLIN};
        assert(poll(&pending,1,1000)==1 && (pending.revents&POLLIN));
        ssize_t n=recv(client_fd,out+used,capacity-used,0);
        assert(n>0);used+=(size_t)n;
    }
    return used;
}
static void expectFrames(const u8 *types,unsigned count) {
    u8 data[0x4000];size_t used=0,at=0;
    for(unsigned i=0;i<count;i++) {
        used=receiveUntil(data,sizeof(data),used,at+8);
        u8 type=0;size_t size=0;assert(labParseControlHeader(data+at,&type,&size));
        assert(type==types[i]);used=receiveUntil(data,sizeof(data),used,at+8+size);at+=8+size;
    }
    used+=drain(data+used,sizeof(data)-used);
    assert(at==used);
}
static size_t joinBody(u8 *out,u8 stage) {
    size_t size=LabJoinPrefixSize+LabApplicationPrefixSize+LabDeliveryRecordSize;
    memset(out,0,size);out[0]=0x13;out[1]=1;out[2]=stage;memcpy(out+3,"Donor",6);
    out[LabJoinPrefixSize]=1;labWriteLe64(out+LabJoinPrefixSize+8,test_app);
    out[LabJoinPrefixSize+16]=0x22;memcpy(out+LabJoinPrefixSize+8+0x18,"2.0.0",6);
    u8 *record=out+LabJoinPrefixSize+LabApplicationPrefixSize;
    record[0]=1;labWriteLe64(record+8,test_app);
    u32 version=donor_version+(wrong_keys?1:0),attributes=UINT32_C(0x50000002);
    memcpy(record+0x10,&version,4);memcpy(record+0x14,&version,4);
    memcpy(record+0x1c,&attributes,4);
    return size;
}
static void acceptedConnection(void) {
    setup();
    close(s_room.peer);s_room.peer=-1;close(client_fd);client_fd=-1;
    s_room.listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);assert(s_room.listener>=0);
    assert(configureSocket(s_room.listener,true));
    struct sockaddr_in address={0};address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!bind(s_room.listener,(struct sockaddr *)&address,sizeof(address)));
    assert(!listen(s_room.listener,7));
    socklen_t length=sizeof(address);
    assert(!getsockname(s_room.listener,(struct sockaddr *)&address,&length));
    client_fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);assert(client_fd>=0);
    assert(!connect(client_fd,(struct sockaddr *)&address,sizeof(address)));
    linked_peer=INADDR_LOOPBACK;
    struct pollfd accepting={.fd=s_room.listener,.events=POLLIN};
    assert(poll(&accepting,1,1000)==1 && (accepting.revents&POLLIN));
    labGameRoomPoll();
    assert(s_room.peer>=0 && s_room.peer_ip==INADDR_LOOPBACK);
    assert(vendor_options==2 && reuse_options==2);
    assert((fcntl(s_room.listener,F_GETFL,0)&O_NONBLOCK) && (fcntl(s_room.peer,F_GETFL,0)&O_NONBLOCK));
    u8 frame[1032];size_t size=joinBody(frame+8,0);labControlHeader(frame,5,(u32)size);
    assert(send(client_fd,frame,size+8,0)==(ssize_t)(size+8));
    assert(nonblocking(client_fd));
    struct pollfd pending={.fd=s_room.peer,.events=POLLIN};
    assert(poll(&pending,1,1000)==1 && (pending.revents&POLLIN));
    labGameRoomPoll();
    assert(s_room.status.phase==LabGameRoom_Joining && s_room.status.peer_joined && !requests);
    expectFrames((const u8[]){7},1);
    tick+=100000000;labGameRoomPoll();
    assert(s_room.status.phase==LabGameRoom_Ready);
    expectFrames((const u8[]){9},1);
    cleanup();
}
static void joined(void) {
    u8 data[1024];size_t size=joinBody(data,0);handleControl(5,data,size);
    assert(s_room.status.phase==LabGameRoom_Joining && s_room.status.peer_joined && !content_queries && !requests);
    /* Equal versions retain the local canonical record, independently of the donor nickname. */
    assert(s_room.canonical.summary[8]==(comparison>0?0x22:0x11));
    const u8 ack[]={7};expectFrames(ack,1);
    labGameRoomStart();assert(s_room.status.phase==LabGameRoom_Joining && !requests);
    tick+=100000000;labGameRoomPoll();
    assert(s_room.status.phase==LabGameRoom_Ready);
    const u8 types[]={9};expectFrames(types,1);
}
static void ready(void) {
    labGameRoomStart();assert(s_room.status.phase==LabGameRoom_Preparing && !requests);
    const u8 start[]={10};expectFrames(start,1);
    u8 frame[1032];size_t size=joinBody(frame+8,1);labControlHeader(frame,24,(u32)size);
    /* A fragmented native readiness update must be accepted, and sender
       startup must wait until the full update has arrived. */
    assert(send(client_fd,frame,7,0)==7);labGameRoomPoll();
    assert(s_room.status.phase==LabGameRoom_Preparing && !requests);expectFrames(NULL,0);
    assert(send(client_fd,frame+7,size+1,0)==(ssize_t)(size+1));labGameRoomPoll();
    assert(s_room.status.phase==LabGameRoom_WaitingSender && !requests);
    const u8 types[]={9,20};expectFrames(types,2);
}
static void receiving(void) {
    u8 response[24]={2,1};NcmContentMetaKey key=patch();memcpy(response+8,&key,16);
    handleControl(21,response,sizeof(response));
    assert(s_room.status.phase==LabGameRoom_Receiving && requests==1 && !commits && advertisement[6]==2);
    assert(reply_log_size==sizeof(response) && !memcmp(reply_log,response,sizeof(response)));
}
static void rejectedSenderReplies(void) {
    /* Preserve the actual donor bytes on both rejection paths. A structurally
       valid reply with another version must never start native reception. */
    setup();comparison=1;joined();ready();
    u8 reply[8+(LabDeliveryMaxKeys+1)*16]={2,1};
    NcmContentMetaKey key=patch();key.version++;memcpy(reply+8,&key,16);
    handleControl(21,reply,24);
    assert(s_room.status.phase==LabGameRoom_Failed && !requests && !commits);
    assert(strstr(s_room.status.message,"differs from selection"));
    assert(reply_log_size==24 && !memcmp(reply_log,reply,24));
    assert(strstr(sender_log,"Sender offered Patch keys[0]") && strstr(sender_log,"version=00040001"));
    assert(strstr(sender_log,"Sender expected Patch keys[0]") && strstr(sender_log,"version=00040000"));
    assert(strstr(sender_log,"offered Patch key set differs"));
    for(unsigned fault=0;fault<8;fault++) {
        setup();comparison=1;joined();ready();
        memset(reply,0,sizeof(reply));reply[0]=2;reply[1]=1;
        key=patch();memcpy(reply+8,&key,16);size_t size=24;
        switch(fault) {
            case 0:reply[0]=1;break;
            case 1:reply[1]=0;break;
            case 2:size=23;break;
            case 3:size=7;break;
            case 4:reply[20]=3;break; /* SystemUpdate metadata */
            case 5:reply[8]^=1;break; /* Another title */
            case 6:reply[1]=2;memcpy(reply+24,reply+8,16);size=40;break;
            case 7:reply[1]=LabDeliveryMaxKeys+1;size=sizeof(reply);break;
        }
        handleControl(21,reply,size);
        assert(s_room.status.phase==LabGameRoom_Failed && !requests && !commits);
        assert(strstr(s_room.status.message,"Invalid sender reply"));
        assert(strstr(sender_log,"Sender reply rejected: invalid"));
        size_t logged=size<sizeof(reply_log)?size:sizeof(reply_log);
        assert(reply_log_size==logged && !memcmp(reply_log,reply,logged));
        if(size>logged)assert(strstr(sender_log,"log truncated"));
    }
}
static void newerDonor(void) {
    /* Reproduce the hardware version difference independently of the local
       send-key API. The selected/received version must come from the donor. */
    setup();local_version=0x00a30000;donor_version=0x00a40000;comparison=1;
    joined();ready();
    assert(s_room.expected_key_count==1 && s_room.expected_keys[0].version==donor_version);
    assert(!send_key_queries);
    receiving();async_ready=true;labGameRoomPoll();labGameRoomPoll();
    assert(s_room.status.phase==LabGameRoom_Finishing && commits==1 && !send_key_queries);
    expectFrames((const u8[]){9},1);
    u8 update[1024];size_t size=joinBody(update,2);handleControl(24,update,size);
    assert(s_room.status.phase==LabGameRoom_Finishing && commits==1 && !send_key_queries);
    expectFrames((const u8[]){9},1);
    tick+=500000000;labGameRoomPoll();
    assert(s_room.status.phase==LabGameRoom_Done && requests==1 && commits==1 && !avm_updates && !ns_updates);
    expectFrames((const u8[]){28},1);

    setup();comparison=1;joined();ready();
    self_compare_rc=0x74410;size=joinBody(update,2);handleControl(24,update,size);
    assert(s_room.status.phase==LabGameRoom_Failed && !requests && !commits);
}
static void olderDonor(void) {
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        setup();local_version=0x50000;comparison=-1;joined();ready();
        assert(s_room.older_update && s_room.canonical.summary[8] == 0x22);
        assert(s_room.expected_key_count == 1 && s_room.expected_keys[0].version == donor_version);
        receiving();
        async_ready=true;labGameRoomPoll();
        assert(s_room.status.phase == LabGameRoom_Committing && !commits && !avm_updates && !ns_updates);
        if (scenario == 0) {
            labGameRoomClose();
            assert(!commits && !avm_updates && !ns_updates);
            continue;
        }
        if (scenario == 1) commit_rc=0x6cc10;
        if (scenario == 2) ns_update_rc=0x1234;
        labGameRoomPoll();
        if (scenario == 1) {
            assert(s_room.status.phase == LabGameRoom_Failed && commits == 1 && !avm_updates && !ns_updates);
            continue;
        }
        assert(s_room.status.phase == LabGameRoom_Finishing && commits == 1);
        assert(avm_updates == 1 && ns_updates == 1 && !avm_open && !manager_open);
        expectFrames((const u8[]){9},1);
        tick+=500000000;labGameRoomPoll();
        assert(s_room.status.phase == LabGameRoom_Done && avm_updates == 1 && ns_updates == 1);
        assert(strstr(s_room.status.message, scenario == 2 ? "reset failed" : "version reset."));
        expectFrames((const u8[]){28},1);
    }
}
int main(void) {
    newerDonor();
    olderDonor();
    rejectedSenderReplies();
    acceptedConnection();
    setup();joined();
    tick+=500000000;labGameRoomPoll();expectFrames((const u8[]){9},1);
    tick+=500000000;labGameRoomPoll();expectFrames(NULL,0);
    setup();joined();ready();receiving();
    async_ready=true;labGameRoomPoll();labGameRoomPoll();
    assert(s_room.status.phase==LabGameRoom_Finishing && commits==1);
    expectFrames((const u8[]){9},1);
    tick+=500000000;labGameRoomPoll();
    assert(s_room.status.phase==LabGameRoom_Done && requests==1 && commits==1 && !avm_updates && !ns_updates);
    expectFrames((const u8[]){28},1);
    cleanup();setup();comparison=1;joined();ready();
    u8 update[1024];size_t size=joinBody(update,2);handleControl(24,update,size);
    assert(s_room.status.phase==LabGameRoom_WaitingSender && !requests);
    const u8 network[]={9};expectFrames(network,1);
    receiving();labGameRoomPoll();assert(!commits);
    async_ready=true;labGameRoomPoll();
    assert(s_room.status.phase==LabGameRoom_Committing && !commits && received_task && s_room.receiver.async_open);
    labGameRoomPoll();assert(s_room.status.phase==LabGameRoom_Finishing && commits==1 && !cancels);
    expectFrames((const u8[]){9},1);
    tick+=499999999;labGameRoomPoll();
    assert(s_room.status.phase==LabGameRoom_Finishing && commits==1);expectFrames(NULL,0);
    tick++;labGameRoomPoll();assert(s_room.status.phase==LabGameRoom_Done && commits==1);
    const u8 finish[]={28};expectFrames(finish,1);
    assert(advertisement[0x98]==1 && labReadBe32(advertisement+0x9c)==s_room.host_node &&
        labReadBe32(advertisement+0xa4)==s_room.peer_node);
    labGameRoomPoll();assert(commits==1);

    setup();comparison=1;joined();ready();receiving();s_room.donor.protocol=0x10;
    async_ready=true;labGameRoomPoll();labGameRoomPoll();
    assert(s_room.status.phase==LabGameRoom_Finishing);expectFrames((const u8[]){9},1);
    tick+=500000000;labGameRoomPoll();
    u8 legacy[32];size_t legacy_size=drain(legacy,sizeof(legacy));
    assert(legacy_size==8 && legacy[1]==28 && labReadBe32(legacy+4)==0 && commits==1);

    setup();comparison=1;joined();ready();receiving();
    async_ready=true;labGameRoomPoll();labGameRoomPoll();expectFrames((const u8[]){9},1);
    close(client_fd);client_fd=-1;labGameRoomPoll();
    assert(s_room.status.phase==LabGameRoom_Done && s_room.receiver.status.phase==LabReceive_Installed && commits==1);
    assert(strstr(s_room.status.message,"Game installed") && !s_room.status.peer_joined);

    setup();comparison=1;joined();ready();receiving();
    wrong_keys=true;size=joinBody(update,1);handleControl(24,update,size);
    assert(s_room.status.phase==LabGameRoom_Failed && !commits && cancels==1);
    for(unsigned type=12;type<=17;type++) {
        setup();joined();handleControl((u8)type,NULL,0);
        assert(s_room.status.phase==LabGameRoom_Failed && !requests && !commits);
    }
    setup();comparison=1;joined();labGameRoomStart();expectFrames((const u8[]){10},1);
    tick=6000000000;labGameRoomPoll();assert(s_room.status.phase==LabGameRoom_Failed && !requests);
    setup();joined();close(client_fd);client_fd=-1;labGameRoomPoll();
    assert(s_room.status.open && !s_room.status.peer_joined && s_room.status.phase==LabGameRoom_Waiting);
    cleanup();
    puts("PASS: coordinator replay, readiness ordering, native install, older patch launch reset, cancellation, firmware rejection and reconnect");
}
