#include "service_access.h"

Result labGetService(Service *sm_proxy, const char *name, Service *out) {
    const SmServiceName encoded=smEncodeName(name);
    Handle handle=INVALID_HANDLE;
    Result rc=serviceDispatchIn(sm_proxy,1,encoded,
        .out_handle_attrs={SfOutHandleAttr_HipcMove},.out_handles=&handle);
    if(R_SUCCEEDED(rc))serviceCreate(out,handle);
    return rc;
}
