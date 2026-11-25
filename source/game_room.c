#include "game_room.h"
#include "delivery_wire.h"
#include "game_receiver.h"
#include "ldn_identity.h"
#include "service_access.h"
#include "host_profile.h"
#include "donor_version.h"
#include "lab_log.h"
#include "launch_version.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

_Static_assert(sizeof(NsApplicationDeliveryInfo)==LabDeliveryRecordSize,"Native delivery record ABI");

/* Route the room's extra service lookups through HEN. The SDK still owns
   initialization, PID registration and cleanup. No service is registered or
   overridden in another process. See Makefile's --wrap option. */
static Service *s_room_sm;
Result __real_smGetServiceWrapper(Service *out, SmServiceName name);
Result __wrap_smGetServiceWrapper(Service *out, SmServiceName name) {
    const char *const names[] = {"ldn:s", "bsd:s", "avm"};
    if (s_room_sm) {
        for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++)
            if (smServiceNamesAreEqual(name, smEncodeName(names[i])))
                return labGetService(s_room_sm, names[i], out);
    }
    return __real_smGetServiceWrapper(out, name);
}

typedef struct {
    LabGameRoomStatus status;
    NsRequestServerStopper stopper;
    bool stopper_open, ldn_ready, ap_open, network_created, sockets_ready;
    bool older_update;
    Result launch_reset_result;
    int listener, peer;
    u64 application_id, frame_deadline, sender_deadline, join_deadline;
    u64 next_link_check,completion_at;
    u64 roster_at,roster_retry_at;
    int last_ldn_state;
    unsigned last_link_nodes;
    u32 local_ip, peer_ip, host_node, peer_node;
    u8 advertise[0x180];
    size_t advertise_size;
    LabDeliveryPeer donor;
    LabDeliveryPeer canonical;
    NcmContentMetaKey expected_keys[LabDeliveryMaxKeys];
    unsigned expected_key_count;
    NsApplicationDeliveryInfo local_delivery[LabDeliveryMaxRecords];
    s32 local_delivery_count;
    u8 local_summary[LabApplicationSummaryBytes];
    void *control_data;
    size_t control_data_size;
    u8 frame[LabControlMaxBody + 8];
    size_t frame_have, frame_need;
    LabReceiver receiver;
} GameRoom;
static GameRoom s_room = {.listener = -1, .peer = -1};
static void logLinkState(const char *reason);

static Result badInput(void) { return MAKERESULT(Module_Libnx, LibnxError_BadInput); }
static void message(const char *text) {
    snprintf(s_room.status.message, sizeof(s_room.status.message), "%s", text);
}
static void fail(const char *text) {
    /* A donor/network error after successful native commit cannot undo that
       install. Keep the receiver outcome separate from session reporting. */
    bool installed=s_room.receiver.status.phase==LabReceive_Installed;
    if (!installed) message(text);
    else if (R_FAILED(s_room.launch_reset_result))
        message("Game update installed. Launch version reset failed. See log.");
    else message("Game installed. Donor session ended; see log.");
    s_room.status.phase = installed?LabGameRoom_Done:LabGameRoom_Failed;
    labLog("Game room: %s", text);
    if(installed) {
        s_room.status.peer_joined=false;
        labLog("Native install already committed; failure affects donor session reporting only");
    }
    logLinkState("room failure");
    labReceiverCancel(&s_room.receiver);
    if(s_room.peer>=0){shutdown(s_room.peer,SHUT_RDWR);close(s_room.peer);s_room.peer=-1;}
}
static u64 afterSeconds(unsigned seconds) {
    return armGetSystemTick() + armNsToTicks((u64)seconds * 1000000000);
}
static bool nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
static bool configureSocket(int fd, bool listener) {
    /* Both native paths use 0x80000001: listener +0xADD30, accepted
       +0x9EC1C (mov w2, #-0x7fffffff). The vendor option's semantics
       are not inferred from ordinary SO_LINGER. */
    const u32 vendor_value[2]={1,1};
    const u32 vendor_option=UINT32_C(0x80000001);
    const char *role=listener?"listener":"accepted";
    int reuse=1;
    if(setsockopt(fd,SOL_SOCKET,(int)vendor_option,vendor_value,sizeof(vendor_value))<0) {
        labLog("Management socket vendor option failed role=%s fd=%d vendor_option=%08x errno=%d bsd_result=%08x",
            role,fd,vendor_option,errno,socketGetLastResult());return false;
    }
    if(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse))<0) {
        labLog("Management socket SO_REUSEADDR failed role=%s fd=%d errno=%d bsd_result=%08x",
            role,fd,errno,socketGetLastResult());return false;
    }
    if(!nonblocking(fd)) {
        labLog("Management socket nonblocking setup failed role=%s fd=%d errno=%d",role,fd,errno);
        return false;
    }
    labLog("Stock management socket options applied role=%s fd=%d vendor_option=%08x",role,fd,vendor_option);
    return true;
}
static void logLinkState(const char *reason) {
    if(!s_room.ldn_ready)return;
    LdnState state=LdnState_None;
    Result rc=ldnGetState(&state);
    labLog("LDN diagnostic %s state=%u result=%08x",reason,(unsigned)state,rc);
    LdnNetworkInfo info={0};rc=ldnGetNetworkInfo(&info);
    labLogResult("LDN diagnostic network info",rc);
    if(R_SUCCEEDED(rc))labLogBytes("LDN diagnostic full network info",&info,sizeof(info));
    if(s_room.peer>=0) {
        int error=0;socklen_t length=sizeof(error);
        int result=getsockopt(s_room.peer,SOL_SOCKET,SO_ERROR,&error,&length);
        labLog("TCP diagnostic fd=%d SO_ERROR=%d query=%d errno=%d buffered_frame=%zu/%zu",
            s_room.peer,error,result,result<0?errno:0,s_room.frame_have,s_room.frame_need);
    }
}
static void monitorLink(void) {
    if(armGetSystemTick()<s_room.next_link_check)return;
    s_room.next_link_check=afterSeconds(1);
    LdnState state=LdnState_None;Result rc=ldnGetState(&state);
    if(R_FAILED(rc)){labLogResult("Monitor own LDN state",rc);return;}
    if((int)state!=s_room.last_ldn_state) {
        labLog("LDN state changed %d -> %u",s_room.last_ldn_state,(unsigned)state);
        s_room.last_ldn_state=(int)state;
    }
    if(state!=LdnState_AccessPointCreated) {
        if(s_room.network_created && s_room.status.phase!=LabGameRoom_Failed && s_room.status.phase!=LabGameRoom_Done)
            fail("Local wireless room disconnected. See log.");
        return;
    }
    LdnNetworkInfo info={0};rc=ldnGetNetworkInfo(&info);
    if(R_FAILED(rc)){labLogResult("Monitor own LDN participant links",rc);return;}
    unsigned nodes=0;
    for(unsigned i=0;i<8;i++)if(info.nodes[i].is_connected)nodes|=1u<<i;
    if(nodes!=s_room.last_link_nodes) {
        labLog("LDN connected node mask changed %02x -> %02x",s_room.last_link_nodes,nodes);
        labLogBytes("LDN membership transition",&info,sizeof(info));
        s_room.last_link_nodes=nodes;
    }
}
static bool waitSocket(int fd, short events, u64 deadline) {
    for (;;) {
        if (armGetSystemTick() >= deadline) { errno = ETIMEDOUT; return false; }
        struct pollfd p = {.fd = fd, .events = events};
        int n = poll(&p, 1, 100);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        if (n && (p.revents & events)) return true;
        if (n && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) { errno = ECONNRESET; return false; }
    }
    errno = ECANCELED; return false;
}
static bool sendExact(int fd, const void *data, size_t size) {
    const u8 *p = data;
    u64 deadline = afterSeconds(5);
    while (size) {
        ssize_t n = send(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!waitSocket(fd, POLLOUT, deadline)) return false;
            continue;
        }
        if (n <= 0) return false;
        p += n; size -= (size_t)n;
    }
    return true;
}
static bool control(u8 type, const void *body, size_t size) {
    if (s_room.peer < 0 || size > LabControlMaxBody) return false;
    u8 *frame=malloc(size+8);
    if(!frame)return false;
    labControlHeader(frame, type, (u32)size);
    if(size)memcpy(frame+8,body,size);
    labLog("Game room control TX type=%u bytes=%zu", type, size);
    bool sent=sendExact(s_room.peer,frame,size+8);
    labLog("Game room TX complete type=%u success=%u wire_bytes=%zu errno=%d",
        type,sent,size+8,sent?0:errno);
    if(sent && (type==7 || type==9 || type==10 || type==20 || type==28))
        labLogBytes("Management frame TX",frame,size+8);
    free(frame);
    return sent;
}
static bool controlDataReply(u8 type, const u8 *body, size_t size) {
    if (size != 8 || !s_room.control_data) return false;
    u64 application = (u64)labReadBe32(body) << 32 | labReadBe32(body + 4);
    if (application != s_room.application_id) return false;
    const bool size_only = type == 1;
    u8 prefix[20];
    labControlHeader(prefix, size_only ? 2 : 4,
        12 + (size_only ? 0 : (u32)s_room.control_data_size));
    memcpy(prefix+8, body, 8); labWriteBe32(prefix + 16, (u32)s_room.control_data_size);
    labLog("Game room control-data response type=%u size=%zu", size_only ? 2 : 4, s_room.control_data_size);
    return sendExact(s_room.peer, prefix, sizeof(prefix)) &&
        (size_only || sendExact(s_room.peer, s_room.control_data, s_room.control_data_size));
}
static bool networkInfo(void) {
    u8 body[8 + LabApplicationPrefixSize + LabDeliveryMaxRecords * LabDeliveryRecordSize + 2 * 0x8c];
    size_t size = labNetworkBody(body, sizeof(body), &s_room.canonical,
        s_room.host_node, s_room.peer_node, s_room.status.local_name,s_room.donor.nickname);
    return size && control(9, body, size);
}
static Result publishAdvertisement(const char *stage) {
    Result rc=ldnSetAdvertiseData(s_room.advertise,s_room.advertise_size);
    labLogResult(stage,rc);
    return rc;
}
static bool selectCanonical(void) {
    s32 comparison=1;
    if(s_room.local_delivery_count && s_room.donor.delivery_count) {
        NsApplicationDeliveryInfo records[LabDeliveryMaxRecords];
        memcpy(records,s_room.donor.delivery,s_room.donor.delivery_count*sizeof(*records));
        Result rc=nsCompareApplicationDeliveryInfo(records,(s32)s_room.donor.delivery_count,
            s_room.local_delivery,s_room.local_delivery_count,&comparison);
        labLogResult("NS select canonical room game version",rc);
        if(R_FAILED(rc))return false;
    }
    bool use_donor=(comparison>0 || s_room.older_update) && s_room.donor.delivery_count;
    if(use_donor)s_room.canonical=s_room.donor;
    else {
        memset(&s_room.canonical,0,sizeof(s_room.canonical));
        memcpy(s_room.canonical.summary,s_room.local_summary,sizeof(s_room.local_summary));
        s_room.canonical.delivery_count=(unsigned)s_room.local_delivery_count;
        memcpy(s_room.canonical.delivery,s_room.local_delivery,
            s_room.local_delivery_count*sizeof(*s_room.local_delivery));
    }
    labUpdateHostApplication(s_room.advertise,s_room.canonical.summary);
    labLog("Canonical room patch source=%s records=%u",use_donor?"donor":"receiver",
        s_room.canonical.delivery_count);
    return true;
}

/* Construct the native host profile directly. No lcs service, native lobby,
   transfer-memory capture, or LRP coordinator is opened. */
static Result prepareProfile(u64 application_id, const char *nickname, LdnNetworkConfig *config, u32 *protocol) {
    Result rc=nsGetApplicationDeliveryInfo(s_room.local_delivery,LabDeliveryMaxRecords,
        application_id,2,&s_room.local_delivery_count);
    labLogResult("NS read local application delivery info",rc);
    if(R_FAILED(rc))return rc;
    if(s_room.local_delivery_count<0 || s_room.local_delivery_count>LabDeliveryMaxRecords)return badInput();
    *protocol=labDeliveryProtocol(s_room.local_delivery,(unsigned)s_room.local_delivery_count);
    if(!*protocol)return badInput();
    size_t capacity=0x36000;
    s_room.control_data=calloc(1,capacity);
    if(!s_room.control_data)return MAKERESULT(Module_Libnx,LibnxError_OutOfMemory);
    u64 actual=0;u32 unused=0;
    rc=nsGetApplicationControlData2(NsApplicationControlSource_Storage,application_id,
        s_room.control_data,capacity,0,0,&actual,&unused);
    if(rc==0x5010){
        void *larger=realloc(s_room.control_data,0x80000);
        if(!larger)return MAKERESULT(Module_Libnx,LibnxError_OutOfMemory);
        s_room.control_data=larger;capacity=0x80000;
        rc=nsGetApplicationControlData2(NsApplicationControlSource_Storage,application_id,
            s_room.control_data,capacity,0,0,&actual,&unused);
    }
    labLogResult("NS read local control data",rc);
    if(R_FAILED(rc))return rc;
    if(actual<0x4000 || actual>capacity)return badInput();
    s_room.control_data_size=(size_t)actual;
    s64 estimated=0;NcmContentMetaKey keys[LabDeliveryMaxKeys];s32 count=0;
    Result estimate_rc=nsListContentMetaKeyToDeliverApplication(keys,LabDeliveryMaxKeys,0,
        s_room.local_delivery,s_room.local_delivery_count,&count);
    if(R_SUCCEEDED(estimate_rc) && count>0 && count<=LabDeliveryMaxKeys)
        estimate_rc=nsEstimateRequiredSize(keys,count,&estimated);
    labLogResult("NS estimate local delivery size (informational)",estimate_rc);
    if(R_FAILED(estimate_rc) || estimated<0)estimated=0;
    if(!labBuildApplicationSummary(s_room.local_summary,application_id,s_room.control_data,
        s_room.control_data_size,(u64)estimated))return badInput();
    u32 nonce=0;randomGet(&nonce,sizeof(nonce));
    if(!labBuildHostAdvertisement(s_room.advertise,s_room.local_summary,nonce,2,nickname))return badInput();
    s_room.advertise_size=LabAdvertisementSize;
    memcpy(s_room.canonical.summary,s_room.local_summary,sizeof(s_room.local_summary));
    s_room.canonical.delivery_count=(unsigned)s_room.local_delivery_count;
    memcpy(s_room.canonical.delivery,s_room.local_delivery,
        s_room.local_delivery_count*sizeof(*s_room.local_delivery));
    memset(config,0,sizeof(*config));
    config->intent_id.local_communication_id=LAB_COMMUNICATION_ID;
    config->intent_id.scene_id=LAB_SCENE_ID;
    config->node_count_max=2;
    config->local_communication_version=LabRoomProtocol>>4;
    labLog("Host profile comm=%016llx scene=%u room_protocol=%02x LDN=%u",
        (unsigned long long)LAB_COMMUNICATION_ID,LAB_SCENE_ID,LabRoomProtocol,*protocol);
    labLogBytes("Host advertisement",s_room.advertise,s_room.advertise_size);
    return 0;
}

static bool startReceiver(const NcmContentMetaKey *keys, unsigned count) {
    Result rc=labReceiverStart(&s_room.receiver,s_room.peer_ip,s_room.application_id,keys,count);
    if(R_FAILED(rc))return false;
    s_room.status.phase=LabGameRoom_Receiving;
    labUpdateHostProgress(s_room.advertise,s_room.host_node,s_room.peer_node,true,true,false);
    if(R_FAILED(publishAdvertisement("Game room advertise native receive phase")))return false;
    message("Receiving game update...");
    return true;
}
static bool validateDonor(void) {
    LabDeliveryPeer *peer=&s_room.donor;
    if(!peer->delivery_count){message("Donor has no transferable patch.");return false;}
    NsSystemDeliveryInfo system;memcpy(&system,peer->system_info,sizeof(system));
    Result rc=nsVerifyDeliveryProtocolVersion(&system);
    labLogResult("NS verify donor delivery protocol",rc);
    if(R_FAILED(rc)){message("Donor protocol validation failed. See log.");return false;}
    NsApplicationDeliveryInfo records[LabDeliveryMaxRecords];
    memcpy(records,peer->delivery,peer->delivery_count*sizeof(*records));
    LabDonorVersionDecision decision=LabDonorVersion_Unchecked;
    rc=labCheckDonorVersion(records,(s32)peer->delivery_count,s_room.local_delivery,
        s_room.local_delivery_count,&decision);
    if(R_FAILED(rc)){message("Couldn't check donor version. See log.");return false;}
    if(decision!=LabDonorVersion_Receive && decision!=LabDonorVersion_Older)return false;
    unsigned count=0;
    /* NS command 2007 validates the supplied record, then returns the LOCAL
       installed Patch key. It cannot select a remote patch for reception.
       Compare above authenticated the donor's unchanged delivery records. */
    if(!labPatchKeysFromDelivery(s_room.expected_keys,LabDeliveryMaxKeys,&count,
        peer->delivery,peer->delivery_count,s_room.application_id)){
        message("Couldn't validate donor patch keys. See log.");return false;
    }
    s_room.expected_key_count=count;
    s_room.older_update=decision==LabDonorVersion_Older;
    labLog("Selected donor Patch keys from authenticated delivery records; local NS send-key lookup is not used for reception");
    labLogBytes("Expected game patch keys",s_room.expected_keys,count*sizeof(*s_room.expected_keys));
    /* Deliberately omit NeedsSystemUpdateToDeliverApplication and firmware
       selection. Native protocol/content verification remains authoritative. */
    return true;
}
static bool requestSender(void) {
    u8 body[0x18];
    labSendApplicationBody(body,s_room.local_ip,s_room.host_node,s_room.application_id);
    if(!control(20,body,sizeof(body)))return false;
    s_room.status.phase=LabGameRoom_WaitingSender;
    s_room.sender_deadline=afterSeconds(5);
    message("Starting donor sender...");
    return true;
}
static void peerLeftLobby(void) {
    if(s_room.peer>=0){shutdown(s_room.peer,SHUT_RDWR);close(s_room.peer);s_room.peer=-1;}
    s_room.status.peer_joined=false;s_room.status.peer_name[0]=0;
    s_room.status.phase=LabGameRoom_Waiting;
    s_room.roster_at=s_room.roster_retry_at=0;
    s_room.frame_have=0;s_room.expected_key_count=0;
    s_room.older_update=false;
    memset(&s_room.donor,0,sizeof(s_room.donor));
    s_room.peer_node++;
    if(!s_room.peer_node || s_room.peer_node==s_room.host_node)s_room.peer_node=s_room.host_node==1?2:1;
    selectCanonical();
    s_room.advertise[1]=1;s_room.advertise[2]=1;
    labUpdateHostProgress(s_room.advertise,s_room.host_node,s_room.peer_node,false,false,false);
    publishAdvertisement("Game room remove departed participant");
    message("Donor left. Waiting for one stock donor...");
}
static void logPatchKeys(const char *label, const void *data, unsigned count) {
    const u8 *keys=data;
    labLogBytes(label,data,(size_t)count*16);
    for(unsigned i=0;i<count;i++) {
        const u8 *key=keys+(size_t)i*16;
        labLog("%s[%u] id=%016llx version=%08x type=%02x install_type=%02x padding=%02x%02x",
            label,i,(unsigned long long)labReadLe64(key),labReadLe32(key+8),
            key[12],key[13],key[14],key[15]);
    }
}
static void handleControl(u8 type, const u8 *body, size_t size) {
    labLog("Game room control RX type=%u bytes=%zu", type, size);
    if (type == 1 || type == 3) {
        if (!controlDataReply(type, body, size)) fail("Couldn't answer title information request.");
    } else if (type == 5 || type == 24) {
        bool first = !s_room.status.peer_joined;
        if((first && type!=5) || (type==5 && s_room.status.phase!=LabGameRoom_Waiting && s_room.status.phase!=LabGameRoom_Joining &&
            s_room.status.phase!=LabGameRoom_Ready)) {
            fail("Unexpected initial room join.");return;
        }
        LabDeliveryPeer updated;
        if (!labParseJoin(&updated, body, size, s_room.advertise[0], s_room.application_id)) {
            fail("Unexpected join data. See log."); return;
        }
        NsSystemDeliveryInfo system;memcpy(&system,updated.system_info,sizeof(system));
        Result rc=nsVerifyDeliveryProtocolVersion(&system);
        labLogResult("NS verify room participant delivery protocol",rc);
        if(R_FAILED(rc)) {
            const u8 reason[4]={1,0,0,0};control(8,reason,sizeof(reason));
            fail("Donor delivery protocol is incompatible. See log.");return;
        }
        /* A status update after StartCommunication is expected. It must not
           change the patch transaction once the sender request has been sent. */
        if(s_room.status.phase==LabGameRoom_WaitingSender || s_room.status.phase==LabGameRoom_Receiving ||
           s_room.status.phase==LabGameRoom_Committing || s_room.status.phase==LabGameRoom_Finishing) {
            NsApplicationDeliveryInfo records[LabDeliveryMaxRecords];
            NcmContentMetaKey keys[LabDeliveryMaxKeys];unsigned count=0;s32 comparison=0;
            memcpy(records,updated.delivery,updated.delivery_count*sizeof(*records));
            rc=nsCompareApplicationDeliveryInfo(records,(s32)updated.delivery_count,
                records,(s32)updated.delivery_count,&comparison);
            labLogResult("NS authenticate updated participant delivery records",rc);
            if(R_FAILED(rc) || comparison ||
               !labPatchKeysFromDelivery(keys,LabDeliveryMaxKeys,&count,updated.delivery,
                   updated.delivery_count,s_room.application_id) ||
               !labPatchKeySetsEqual(keys,count,s_room.expected_keys,s_room.expected_key_count)) {
                fail("Donor changed the selected patch transaction.");return;
            }
        }
        s_room.donor=updated;
        LabDeliveryPeer *peer = &s_room.donor;
        labLog("Room participant protocol=%02x application_stage=%u first_join=%u",peer->protocol,peer->stage,first);
        labLogBytes("Game room donor system delivery info", peer->system_info, sizeof(peer->system_info));
        labLogBytes("Game room donor application delivery info", peer->delivery,
            peer->delivery_count * LabDeliveryRecordSize);
        snprintf(s_room.status.peer_name, sizeof(s_room.status.peer_name), "%s", peer->nickname);
        if(type==5) {
            u8 node[4]; labWriteBe32(node, s_room.peer_node);
            if (!control(7, node, sizeof(node))) { fail("Couldn't acknowledge join."); return; }
        }
        if (first) {
            s_room.status.peer_joined = true;
            s_room.status.phase = LabGameRoom_Joining;
            /* Native clients consume type 7 synchronously, then start their
               communication thread and reset their initial readiness wait
               (+0x612AC/+0x612CC/+0x61370). Avoid publishing the initial roster
               in that setup window in our much smaller homebrew host. */
            s_room.roster_at=armGetSystemTick()+armNsToTicks(100000000);
            message("Finishing stock donor join...");
        }
        bool preparing = s_room.status.phase == LabGameRoom_Preparing;
        if (preparing) {
            labLog("Stock donor answered StartCommunication with updated delivery records");
            if (!validateDonor()) {fail("Couldn't prepare donor sender. See log.");return;}
        }
        if(!selectCanonical()){fail("Couldn't select the room game version.");return;}
        s_room.advertise[2]=2;
        if(R_FAILED(publishAdvertisement("Game room update canonical participant advertisement"))) {
            fail("Couldn't update room advertisement.");return;
        }
        if(s_room.status.phase==LabGameRoom_Joining)return;
        if(!networkInfo()){fail("Couldn't send room information.");return;}
        if(preparing) {
            if(!requestSender())fail("Couldn't prepare donor sender. See log.");
        } else if(s_room.status.phase==LabGameRoom_Ready)
            message("Donor joined. Press A to receive/install.");
    } else if (type == 21 && s_room.status.phase == LabGameRoom_WaitingSender) {
        NcmContentMetaKey keys[LabDeliveryMaxKeys];unsigned count=0;
        /* Retain evidence before validation: earlier builds discarded the
           rejected reply, so their log could not distinguish malformed framing
           from different Patch keys. Never relax the transaction checks based
           on a failed reply, and bound logging even for oversized peer input. */
        const size_t limit=8+LabDeliveryMaxKeys*sizeof(*keys);
        labLogBytes("Game room sender response body",body,size<limit?size:limit);
        labLog("Sender reply bytes=%zu phase=%u advertised_keys=%u (required phase=2, keys=1..%u, bytes=8+16*keys)",
            size,size?body[0]:0,size>1?body[1]:0,LabDeliveryMaxKeys);
        if(size>limit)labLog("Sender reply log truncated at %zu bytes",limit);
        if(size>=8) {
            unsigned available=(unsigned)((size-8)/16);
            if(available>LabDeliveryMaxKeys)available=LabDeliveryMaxKeys;
            logPatchKeys("Sender offered Patch keys",body+8,available);
        }
        logPatchKeys("Sender expected Patch keys",s_room.expected_keys,s_room.expected_key_count);
        if (!labParseSenderResponse(keys, LabDeliveryMaxKeys, &count, body, size, s_room.application_id)) {
            labLog("Sender reply rejected: invalid phase/count/size or invalid/duplicate selected-game Patch keys; native receive not requested");
            fail("Invalid sender reply. See log."); return;
        }
        if (!labPatchKeySetsEqual(keys,count,s_room.expected_keys,s_room.expected_key_count)) {
            labLog("Sender reply rejected: offered Patch key set differs from authenticated donor selection; native receive not requested");
            fail("Sender patch differs from selection. See log."); return;
        }
        labLogBytes("Game room donor sender patch keys",keys,count*sizeof(*keys));
        if (!startReceiver(keys,count)) fail("Couldn't start game receiver. See log.");
    } else if (type == 6 || type == 8) {
        labLogBytes("Game room peer leave/error", body, size);
        if(s_room.status.phase==LabGameRoom_Ready || s_room.status.phase==LabGameRoom_Joining || s_room.status.phase==LabGameRoom_Waiting)
            peerLeftLobby();
        else if (s_room.status.phase != LabGameRoom_Done) fail("Donor left or reported an error.");
    } else if (type >= 12 && type <= 17) {
        fail("Unexpected firmware transfer request.");
    } else {
        /* Only local A starts a receive, and only for the validated game patch.
           Peers cannot schedule a receive or commit on this console. */
        labLogBytes("Game room unhandled control body", body, size > 256 ? 256 : size);
    }
}
static void readControl(void) {
    for (unsigned frames = 0; frames < 8; frames++) {
        if (!s_room.frame_have) { s_room.frame_need = 8; s_room.frame_deadline = afterSeconds(10); }
        ssize_t n = recv(s_room.peer, s_room.frame + s_room.frame_have,
            s_room.frame_need - s_room.frame_have, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (s_room.frame_have && armGetSystemTick() >= s_room.frame_deadline)
                fail("Room packet timed out.");
            return;
        }
        if (n <= 0) {
            labLog("Management recv ended fd=%d result=%d errno=%d buffered=%zu/%zu",
                s_room.peer,(int)n,n<0?errno:0,s_room.frame_have,s_room.frame_need);
            logLinkState("management recv ended");
            if (!s_room.status.peer_joined && s_room.status.phase == LabGameRoom_Waiting) {
                /* Discovery/control-data queries may use a short-lived TCP
                   connection before the actual room join. */
                close(s_room.peer); s_room.peer = -1; s_room.frame_have = 0;
                message("Waiting for one stock donor...");
            } else if(s_room.status.phase==LabGameRoom_Ready || s_room.status.phase==LabGameRoom_Joining)peerLeftLobby();
            else if (s_room.status.phase != LabGameRoom_Done) fail("Donor disconnected.");
            return;
        }
        s_room.frame_have += (size_t)n;
        if (s_room.frame_have != s_room.frame_need) continue;
        u8 type; size_t size;
        if (!labParseControlHeader(s_room.frame, &type, &size)) { fail("Invalid room packet."); return; }
        if (s_room.frame_need == 8 && size) { s_room.frame_need += size; continue; }
        handleControl(type, s_room.frame + 8, size);
        s_room.frame_have = 0;
        if (s_room.status.phase == LabGameRoom_Failed || s_room.peer<0) return;
    }
}
static bool isLdnPeer(u32 address) {
    LdnNetworkInfo info = {0};
    Result rc = ldnGetNetworkInfo(&info);
    labLogResult("Game room verify connected LDN donor", rc);
    if (R_FAILED(rc)) return false;
    for (unsigned i = 1; i < 8; i++)
        if (info.nodes[i].is_connected && info.nodes[i].ip_addr.addr == address) return true;
    return false;
}

Result labGameRoomOpen(Service *sm_proxy, u64 application_id, const char *nickname) {
    if (s_room.status.open || !sm_proxy || !application_id || !nickname) return badInput();
    memset(&s_room, 0, sizeof(s_room));
    s_room.listener = s_room.peer = -1;
    s_room.last_ldn_state=-1;
    s_room.application_id = application_id;
    randomGet(&s_room.host_node,sizeof(s_room.host_node));
    if(!s_room.host_node)s_room.host_node=1;
    s_room.peer_node=s_room.host_node+1;
    if(!s_room.peer_node)s_room.peer_node=1;
    labLog("Room assigned node IDs host=%08x first_peer=%08x",s_room.host_node,s_room.peer_node);
    snprintf(s_room.status.local_name, sizeof(s_room.status.local_name), "%s", nickname);
    s_room_sm = sm_proxy;
    Result rc = nsGetRequestServerStopper(&s_room.stopper);
    labLogResult("Game room acquire NS RequestServerStopper", rc);
    if (R_SUCCEEDED(rc)) s_room.stopper_open = true;
    LdnNetworkConfig config; u32 protocol = 0;
    if (R_SUCCEEDED(rc)) rc = prepareProfile(application_id, nickname, &config, &protocol);
    if (R_SUCCEEDED(rc)) {
        rc = ldnInitialize(LdnServiceType_System); s_room.ldn_ready = R_SUCCEEDED(rc);
        labLogResult("Game room initialize own LDN service", rc);
    }
    if (R_SUCCEEDED(rc)) {
        rc = ldnSetProtocol((LdnProtocol)protocol);
        labLogResult("Game room set native-selected LDN protocol", rc);
    }
    if (R_SUCCEEDED(rc)) {
        rc = ldnSetOperationMode(LdnOperationMode_HighSpeed);
        labLogResult("Game room set LDN operation mode", rc);
    }
    if (R_SUCCEEDED(rc)) {
        rc = ldnOpenAccessPoint(); s_room.ap_open = R_SUCCEEDED(rc);
        labLogResult("Game room open own access point", rc);
    }
    if (R_SUCCEEDED(rc)) {
        const u8 passphrase[16] = {0x2d,0x30,0x88,0x5a,0x65,0x8a,0xb4,0xe4,0xd8,0x50,0x92,0xed,0x52,0x5a,0x99,0x4c};
        LdnSecurityConfig security = {.security_mode = LdnSecurityMode_Product, .passphrase_size = 16};
        memcpy(security.passphrase, passphrase, 16);
        LdnUserConfig user = {0}; snprintf(user.user_name, sizeof(user.user_name), "LcsHost");
        rc = labCreateGameNetwork(sm_proxy, &security, &user, &config); s_room.network_created = R_SUCCEEDED(rc);
        labLogResult("Game room create own LDN network", rc);
    }
    if (R_SUCCEEDED(rc)) {
        LdnIpv4Address address; LdnSubnetMask mask;
        rc = ldnGetIpv4Address(&address, &mask);
        if (R_SUCCEEDED(rc)) s_room.local_ip = address.addr;
        labLogResult("Game room get own LDN address", rc);
        labLog("Game room local IPv4 host-order=%08x", s_room.local_ip);
    }
    if (R_SUCCEEDED(rc)) {
        rc = ldnSetAdvertiseData(s_room.advertise, s_room.advertise_size);
        labLogResult("Game room publish native advertisement", rc);
    }
    if (R_SUCCEEDED(rc)) {
        SocketInitConfig sockets = *socketGetDefaultInitConfig();
        sockets.bsd_service_type = BsdServiceType_System;
        rc = socketInitialize(&sockets); s_room.sockets_ready = R_SUCCEEDED(rc);
        labLogResult("Game room initialize system sockets", rc);
    }
    if (R_SUCCEEDED(rc)) {
        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        s_room.listener = fd;
        struct sockaddr_in address = {0}; address.sin_family = AF_INET;
        address.sin_port = htons(LabControlPort); address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (fd < 0 || !configureSocket(fd,true) ||
            bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 7) < 0) {
            labLog("Game room listen failed errno=%d bsd_result=%08x", errno, socketGetLastResult());
            rc = MAKERESULT(Module_Libnx, LibnxError_IoError);
        }
    }
    if (R_FAILED(rc)) { labGameRoomClose(); return rc; }
    s_room.status.open = true; s_room.status.phase = LabGameRoom_Waiting;
    message("Waiting for one stock donor...");
    labLog("Game room ready; game-only receive/install; no firmware or native sharing planner requested");
    return 0;
}
void labGameRoomStart(void) {
    if (!s_room.status.open || s_room.status.phase != LabGameRoom_Ready) return;
    readControl();
    if(s_room.status.phase!=LabGameRoom_Ready)return;
    /* Authenticate delivery records and select the donor's exact patch. */
    if(!validateDonor())return;
    s_room.roster_retry_at=0;
    Result rc = ldnSetStationAcceptPolicy(LdnAcceptPolicy_AlwaysReject);
    labLogResult("Game room close room to new joins", rc);
    if (R_FAILED(rc)) { fail("Couldn't close room to new joins."); return; }
    s_room.advertise[1] = 0;
    labUpdateHostProgress(s_room.advertise,s_room.host_node,s_room.peer_node,true,false,false);
    rc = ldnSetAdvertiseData(s_room.advertise, s_room.advertise_size);
    labLogResult("Game room mark room communicating", rc);
    if (R_FAILED(rc) || !control(10, NULL, 0)) { fail("Couldn't start donor communication."); return; }
    /* LRP's client type-10 handler refreshes records and sends type 24 before
       sender selection. Do not race a type-20 request ahead of that reply. */
    s_room.status.phase = LabGameRoom_Preparing;
    s_room.sender_deadline = afterSeconds(5);
    message("Waiting for donor readiness...");
}
void labGameRoomPoll(void) {
    if (!s_room.status.open) return;
    monitorLink();
    if (s_room.peer < 0 && s_room.status.phase == LabGameRoom_Waiting) {
        struct sockaddr_in address; socklen_t length = sizeof(address);
        int fd = accept(s_room.listener, (struct sockaddr *)&address, &length);
        if (fd >= 0) {
            u32 ip = ntohl(address.sin_addr.s_addr);
            labLog("Game room control connection peer=%08x", ip);
            if (!isLdnPeer(ip) || !configureSocket(fd,false)) close(fd);
            else {
                s_room.peer = fd; s_room.peer_ip = ip; s_room.join_deadline = afterSeconds(15);
                message("Reading donor information...");
            }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) fail("Couldn't accept donor connection.");
    }
    if (s_room.peer >= 0 && s_room.status.phase != LabGameRoom_Failed && s_room.status.phase != LabGameRoom_Done)
        readControl();
    if(s_room.peer>=0 && s_room.status.phase==LabGameRoom_Joining && armGetSystemTick()>=s_room.roster_at) {
        if(!networkInfo())fail("Couldn't publish the initial room roster.");
        else {
            s_room.status.phase=LabGameRoom_Ready;
            message("Donor joined. Press A to receive/install.");
            s_room.roster_retry_at=armGetSystemTick()+armNsToTicks(500000000);
        }
    } else if(s_room.peer>=0 && s_room.status.phase==LabGameRoom_Ready && s_room.roster_retry_at &&
        armGetSystemTick()>=s_room.roster_retry_at) {
        /* A valid duplicate snapshot also notifies the client's readiness
           waiter. This single replay is a homebrew timing accommodation,
           not a new packet type or a claimed stock retransmit interval. */
        s_room.roster_retry_at=0;
        labLog("Replaying initial roster once for stock-client readiness");
        if(!networkInfo())fail("Couldn't confirm the room roster.");
    }
    if (s_room.peer >= 0 && !s_room.status.peer_joined &&
        s_room.status.phase == LabGameRoom_Waiting && armGetSystemTick() >= s_room.join_deadline) {
        close(s_room.peer); s_room.peer = -1; s_room.frame_have = 0;
        message("Waiting for one stock donor...");
        labLog("Game room pre-join connection expired");
    }
    if ((s_room.status.phase == LabGameRoom_Preparing || s_room.status.phase == LabGameRoom_WaitingSender) &&
        armGetSystemTick() >= s_room.sender_deadline)
        fail("Donor sender timed out. See log.");
    if (s_room.status.phase == LabGameRoom_Receiving ||
        s_room.status.phase == LabGameRoom_Committing) {
        labReceiverPoll(&s_room.receiver);
        const LabReceiveStatus *receive = &s_room.receiver.status;
        s_room.status.received = receive->received;
        s_room.status.total = receive->total;
        if (receive->phase == LabReceive_ReadyToCommit) {
            s_room.status.phase = LabGameRoom_Committing;
            message("Installing game update...");
        } else if (receive->phase == LabReceive_Installed) {
            if (s_room.older_update) {
                s_room.launch_reset_result = labResetLaunchVersion(s_room.application_id);
                labLogResult("Reset selected game's launch version after older update", s_room.launch_reset_result);
            }
            s_room.status.phase = LabGameRoom_Finishing;
            labUpdateHostProgress(s_room.advertise,s_room.host_node,s_room.peer_node,true,true,true);
            /* LRP +0xA98E0 publishes room information after commit. Its host
               loop waits 500 ms at +0x9FFA4 before +0xA7900 sends completion.
               These are recovered stock steps, not guessed retry intervals. */
            if(!networkInfo()) {
                fail("Game committed; couldn't send final room information.");
                return;
            }
            publishAdvertisement("Game room publish native completion results");
            s_room.completion_at=armGetSystemTick()+armNsToTicks(500000000);
            labLog("Game committed; final roster sent; waiting native 500 ms before completion");
            message("Game installed. Finishing donor session...");
            NsApplicationContentMetaStatus entries[32]; s32 count = 0;
            Result rc = nsListApplicationContentMetaStatus(s_room.application_id, 0, entries, 32, &count);
            labLogResult("NS list installed content after commit", rc);
            if (R_SUCCEEDED(rc) && count >= 0 && count <= 32)
                labLogBytes("Installed content meta status", entries, count * sizeof(*entries));
        } else if (receive->phase == LabReceive_Failed) fail("Game receive/install failed. See log.");
    }
    if(s_room.status.phase==LabGameRoom_Finishing && armGetSystemTick()>=s_room.completion_at) {
        /* LRP +0xA7900 -> +0xB3D20: modern sharing completion. An
           already-current donor may legitimately have zero updates. */
        size_t completion_size=(s_room.donor.protocol&0xfe)==0x10?0:0x40;
        if(!control(28,s_room.advertise+0x98,completion_size)) {
            fail("Game committed; donor completion notification failed.");
            return;
        }
        s_room.status.phase=LabGameRoom_Done;
        if (R_FAILED(s_room.launch_reset_result))
            message("Game update installed. Launch version reset failed. See log.");
        else if (s_room.older_update)
            message("Game update installed. Launch version reset.");
        else message("Game update installed. Firmware not requested.");
    }
}
void labGameRoomClose(void) {
    /* Leave the network and stopper alive until NS has stopped receiving. */
    labReceiverCancel(&s_room.receiver);
    if (s_room.peer >= 0) { shutdown(s_room.peer, SHUT_RDWR); close(s_room.peer); }
    if (s_room.listener >= 0) close(s_room.listener);
    s_room.peer = s_room.listener = -1;
    if (s_room.sockets_ready) { socketExit(); s_room.sockets_ready = false; }
    if (s_room.network_created) {
        labLogResult("Game room destroy own LDN network", ldnDestroyNetwork()); s_room.network_created = false;
    }
    if (s_room.ap_open) { labLogResult("Game room close own access point", ldnCloseAccessPoint()); s_room.ap_open = false; }
    if (s_room.ldn_ready) { ldnExit(); s_room.ldn_ready = false; }
    if (s_room.stopper_open) { nsRequestServerStopperClose(&s_room.stopper); s_room.stopper_open = false; }
    free(s_room.control_data); s_room.control_data = NULL; s_room.control_data_size = 0;
    s_room_sm = NULL;
    s_room.status.open = s_room.status.peer_joined = false;
}
const LabGameRoomStatus *labGameRoomGetStatus(void) { return &s_room.status; }
