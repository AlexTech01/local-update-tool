#include <stddef.h>
#include "lab_log.h"
#include "launch_version.h"

typedef struct {
    u32 version;
    u32 reserved_before_id;
    u64 application_id;
    u64 reserved_after_id;
} NsLaunchVersionInput;

_Static_assert(sizeof(NsLaunchVersionInput) == 24, "NS launch version input size");
_Static_assert(offsetof(NsLaunchVersionInput, application_id) == 8, "NS launch version application offset");

Result labResetLaunchVersion(u64 application_id) {
    if (!application_id) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Result rc = avmInitialize();
    labLogResult("Open AVM for launch version reset", rc);
    if (R_FAILED(rc)) return rc;

    Service manager = {0};
    rc = nsGetApplicationManagerInterface(&manager);
    labLogResult("Open NS application manager for launch version reset", rc);
    if (R_SUCCEEDED(rc)) {
        labLog("Resetting recorded launch version to 0 for application %016llx", (unsigned long long)application_id);
        rc = avmPushLaunchVersion(application_id, 0);
        labLogResult("AVM PushLaunchVersion", rc);
        if (R_SUCCEEDED(rc)) {
            const NsLaunchVersionInput input = {.application_id = application_id};
            rc = serviceDispatchIn(&manager, 36, input);
            labLogResult("NS PushLaunchVersion", rc);
        }
    }
    serviceClose(&manager);
    avmExit();
    return rc;
}
