#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t Result;
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int32_t s32;
typedef int64_t s64;
typedef u32 Handle;
#define INVALID_HANDLE 0u
typedef struct { bool active; unsigned kind; } Service;
typedef struct {Handle handle;bool autoclear;} Event;
typedef struct { Service s; Event event; } AsyncResult;
typedef struct { u64 id; u32 version; uint8_t type, install_type; uint16_t pad; } NcmContentMetaKey;
typedef int NcmStorageId;
enum { NcmContentMetaType_Patch=0x81, NcmStorageId_Any=6, Module_Libnx=345, LibnxError_BadInput=11,
    LibnxError_OutOfMemory=12, LibnxError_IoError=13 };
#define MAKERESULT(m,d) ((Result)((m)|((d)<<9)))
#define KERNELRESULT(d) 0xea01u
#define R_SUCCEEDED(r) ((r)==0)
#define R_FAILED(r) ((r)!=0)
static inline bool serviceIsActive(Service *s) { return s->active; }
typedef struct { int64_t current_size, total_size; } NsReceiveApplicationProgress;
typedef struct { uint8_t data[0x100]; } NsApplicationDeliveryInfo;
Result nsCompareApplicationDeliveryInfo(const NsApplicationDeliveryInfo *,s32,
    const NsApplicationDeliveryInfo *,s32,s32 *);
Result nsHasAllContentsToDeliver(const NsApplicationDeliveryInfo *,s32,bool *);
Result nsRequestReceiveApplication(AsyncResult *,u32,u16,u64,const NcmContentMetaKey *,s32,NcmStorageId);
Result nsCommitReceiveApplication(u64);
Result nsGetReceiveApplicationProgress(u64,NsReceiveApplicationProgress *);
Result asyncResultWait(AsyncResult *,u64);
Result asyncResultGet(AsyncResult *);
void asyncResultClose(AsyncResult *);

typedef struct { u16 security_mode,passphrase_size;u8 passphrase[0x40]; } LdnSecurityConfig;
typedef struct { char user_name[0x21]; } LdnUserConfig;
typedef struct { struct {u64 local_communication_id;u16 scene_id;} intent_id;
    int node_count_max,local_communication_version; } LdnNetworkConfig;
Result ldnCreateNetwork(const LdnSecurityConfig *, const LdnUserConfig *, const LdnNetworkConfig *);
Result ldnDestroyNetwork(void);
Result svcGetProcessId(u64 *,unsigned);
#define CUR_PROCESS_HANDLE 0xffff8001u
typedef struct { unsigned out_num_objects; Service *out_objects;
    unsigned buffer_attrs[8];struct {const void *ptr;size_t size;} buffers[8];
    unsigned out_handle_attrs[8];Handle *out_handles;
} SfDispatchParams;
enum {SfBufferAttr_In=1,SfBufferAttr_HipcMapAlias=4,SfOutHandleAttr_HipcCopy=1};
Result nsGetApplicationManagerInterface(Service *);
Result avmInitialize(void);
void avmExit(void);
Result avmPushLaunchVersion(u64, u32);
void eventLoadRemote(Event *,Handle,bool);
Result serviceDispatchImpl(Service *,unsigned,const void *,size_t,void *,size_t,SfDispatchParams);
void serviceClose(Service *);
#define serviceDispatch(s,id,...) serviceDispatchImpl(s,id,NULL,0,NULL,0,(SfDispatchParams){__VA_ARGS__})
#define serviceDispatchIn(s,id,in,...) serviceDispatchImpl(s,id,&(in),sizeof(in),NULL,0,(SfDispatchParams){__VA_ARGS__})
#define serviceDispatchInOut(s,id,in,out,...) serviceDispatchImpl(s,id,&(in),sizeof(in),&(out),sizeof(out),(SfDispatchParams){__VA_ARGS__})

/* Host coordinator integration tests mock IPC and use real local TCP streams. */
typedef u64 SmServiceName;
SmServiceName smEncodeName(const char *);
bool smServiceNamesAreEqual(SmServiceName,SmServiceName);
typedef struct {u8 data[0x100];} NsSystemDeliveryInfo;
typedef struct {Service s;} NsRequestServerStopper;
typedef struct {u8 data[32];} NsApplicationContentMetaStatus;
typedef int NsApplicationControlSource;
enum {NsApplicationControlSource_Storage=0};
Result nsGetApplicationDeliveryInfo(NsApplicationDeliveryInfo *,s32,u64,u32,s32 *);
Result nsGetApplicationControlData2(NsApplicationControlSource,u64,void *,size_t,u32,u32,u64 *,u32 *);
Result nsListContentMetaKeyToDeliverApplication(NcmContentMetaKey *,s32,s32,const NsApplicationDeliveryInfo *,s32,s32 *);
Result nsEstimateRequiredSize(const NcmContentMetaKey *,s32,s64 *);
Result nsVerifyDeliveryProtocolVersion(const NsSystemDeliveryInfo *);
Result nsGetRequestServerStopper(NsRequestServerStopper *);
void nsRequestServerStopperClose(NsRequestServerStopper *);
Result nsListApplicationContentMetaStatus(u64,s32,NsApplicationContentMetaStatus *,s32,s32 *);
typedef int LdnState;
typedef int LdnProtocol;
enum {LdnState_None=0,LdnState_AccessPointCreated=3,LdnServiceType_System=1,
    LdnOperationMode_HighSpeed=1,LdnSecurityMode_Product=1,LdnAcceptPolicy_AlwaysReject=1};
typedef struct {u32 addr;} LdnIpv4Address,LdnSubnetMask;
typedef struct {bool is_connected;LdnIpv4Address ip_addr;} LdnNodeInfo;
typedef struct {LdnNodeInfo nodes[8];} LdnNetworkInfo;
Result ldnGetState(LdnState *);
Result ldnGetNetworkInfo(LdnNetworkInfo *);
Result ldnInitialize(int);
void ldnExit(void);
Result ldnSetProtocol(LdnProtocol);
Result ldnSetOperationMode(int);
Result ldnOpenAccessPoint(void);
Result ldnCloseAccessPoint(void);
Result ldnGetIpv4Address(LdnIpv4Address *,LdnSubnetMask *);
Result ldnSetAdvertiseData(const void *,size_t);
Result ldnSetStationAcceptPolicy(int);
typedef struct {int bsd_service_type;} SocketInitConfig;
enum {BsdServiceType_System=2};
const SocketInitConfig *socketGetDefaultInitConfig(void);
Result socketInitialize(const SocketInitConfig *);
void socketExit(void);
Result socketGetLastResult(void);
u64 armGetSystemTick(void);
u64 armNsToTicks(u64);
void randomGet(void *,size_t);
