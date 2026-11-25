/* Correct the SDK's command-2010 argument packing for this NRO only.
   The native service still receives, verifies and installs the game data. */
#define NX_SERVICE_ASSUME_NON_DOMAIN
#include <switch.h>
#include <stddef.h>
#include <string.h>
#include "delivery_wire.h"
#include "lab_log.h"

typedef struct {
    u8 storage_id;
    u8 reserved;
    u16 port;
    u32 address;
    u64 application_id;
} ReceiveApplicationInput;
_Static_assert(sizeof(ReceiveApplicationInput)==16,"NS receive input size");
_Static_assert(offsetof(ReceiveApplicationInput,port)==2,"NS receive port offset");
_Static_assert(offsetof(ReceiveApplicationInput,address)==4,"NS receive address offset");
_Static_assert(offsetof(ReceiveApplicationInput,application_id)==8,"NS receive application offset");

Result __wrap_nsRequestReceiveApplication(AsyncResult *a,u32 address,u16 port,u64 application_id,
    const NcmContentMetaKey *meta,s32 count,NcmStorageId storage_id) {
    if(!a || !address || !application_id || !meta || count<1 || count>LabDeliveryMaxKeys)
        return MAKERESULT(Module_Libnx,LibnxError_BadInput);
    memset(a,0,sizeof(*a));
    /* LRP +0x7D2B0 writes storage/port/address/application at +0/+2/+4/+8.
       libounce 20261117.1 has an extra u16 field and sends 24 bytes instead. */
    const ReceiveApplicationInput in={(u8)storage_id,0,port,address,application_id};
    labLog("NS receive IPC cmd=2010 input_bytes=%zu donor=%08x port=%u application=%016llx keys=%d storage=%u",
        sizeof(in),address,(unsigned)port,(unsigned long long)application_id,(int)count,(unsigned)storage_id);
    labLogBytes("NS receive IPC input",&in,sizeof(in));
    Service service={0};
    Result rc=nsGetApplicationManagerInterface(&service);
    Handle event=INVALID_HANDLE;
    if(R_SUCCEEDED(rc))rc=serviceDispatchIn(&service,2010,in,
        .buffer_attrs={SfBufferAttr_HipcMapAlias|SfBufferAttr_In},
        .buffers={{meta,(size_t)count*sizeof(*meta)}},
        .out_num_objects=1,.out_objects=&a->s,
        .out_handle_attrs={SfOutHandleAttr_HipcCopy},.out_handles=&event);
    if(R_SUCCEEDED(rc))eventLoadRemote(&a->event,event,false);
    serviceClose(&service);
    return rc;
}
