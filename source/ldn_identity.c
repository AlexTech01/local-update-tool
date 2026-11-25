#include "ldn_identity.h"
#include "service_access.h"
#include "lab_log.h"
#include <string.h>

static Result badInput(void) { return MAKERESULT(Module_Libnx, LibnxError_BadInput); }
static Result processUpdate(Service *writer, u64 instance, u64 pid, const u8 property[16], bool remove) {
    Service updater = {0};
    Result rc = serviceDispatchIn(writer, 2, instance,
        .out_num_objects = 1, .out_objects = &updater);
    labLogResult(remove ? "ARP acquire own process detach updater" : "ARP acquire own process restore updater", rc);
    if (R_SUCCEEDED(rc)) {
        struct { u64 pid; u8 property[16]; } in = {pid, {0}};
        /* Set first, even for delete: Issue requires an initialized property. */
        memcpy(in.property, property, sizeof(in.property));
        rc = serviceDispatchIn(&updater, 1, in);
        labLogResult("ARP stage own original process property", rc);
    }
    if (R_SUCCEEDED(rc) && remove) {
        rc = serviceDispatch(&updater, 2);
        labLogResult("ARP stage own process detach", rc);
    }
    if (R_SUCCEEDED(rc)) {
        rc = serviceDispatch(&updater, 0);
        labLogResult(remove ? "ARP issue own process detach" : "ARP issue own process restoration", rc);
    }
    /* An unissued updater's destructor restores its previous state. */
    serviceClose(&updater);
    return rc;
}
Result labCreateGameNetwork(Service *sm_proxy, const LdnSecurityConfig *security,
    const LdnUserConfig *user, const LdnNetworkConfig *config) {
    Result rc = ldnCreateNetwork(security, user, config);
    labLogResult("LDN create game network with ordinary identity", rc);
    if (rc != 0xc2cb) return rc;
    /* LDN +0x8ED10 accepts an explicit communication ID when ARP cannot map
       the caller PID. Glue process updater leaves the application instance,
       its NACP, certificate and launch property intact. No code is patched. */
    Service reader = {0}, writer = {0};
    u64 pid = 0, instance = 0;
    u8 property[16] = {0};
    rc = svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
    if (R_SUCCEEDED(rc)) rc = labGetService(sm_proxy, "arp:r", &reader);
    labLogResult("ARP open reader for own LDN identity", rc);
    if (R_SUCCEEDED(rc)) rc = serviceDispatchInOut(&reader, 3, pid, instance);
    labLogResult("ARP read own application instance", rc);
    if (R_SUCCEEDED(rc) && (!pid || !instance)) rc = badInput();
    if (R_SUCCEEDED(rc)) {
        /* GetApplicationProcessProperty returns a 0x10-byte inline record. */
        rc = serviceDispatchInOut(&reader, 2, instance, property);
        labLogResult("ARP save own process property", rc);
    }
    if (R_SUCCEEDED(rc)) rc = labGetService(sm_proxy, "arp:w", &writer);
    labLogResult("ARP open writer for scoped LDN identity", rc);
    if (R_SUCCEEDED(rc)) {
        labLog("ARP scoped detach pid=%llu instance=%llu; no instance unregister or NACP write",
            (unsigned long long)pid, (unsigned long long)instance);
        rc = processUpdate(&writer, instance, pid, property, true);
        /* Even a failed Issue could have reached the server before a transport
           error. Attempt restoration after every detach attempt. */
        Result create_rc = rc;
        if (R_SUCCEEDED(rc)) {
            u64 found = 0;
            Result lookup = serviceDispatchInOut(&reader, 3, pid, found);
            labLogResult("ARP verify own mapping detached", lookup);
            if (lookup == 0xcc9d) {
                create_rc = ldnCreateNetwork(security, user, config);
                labLogResult("LDN create game network during scoped detach", create_rc);
            } else create_rc = R_FAILED(lookup) ? lookup : badInput();
        }
        Result restore_rc = badInput();
        for (unsigned attempt = 0; attempt < 3 && R_FAILED(restore_rc); attempt++)
            restore_rc = processUpdate(&writer, instance, pid, property, false);
        if (R_SUCCEEDED(restore_rc)) {
            u64 found = 0; u8 restored[16] = {0};
            restore_rc = serviceDispatchInOut(&reader, 3, pid, found);
            if (R_SUCCEEDED(restore_rc) && found != instance) restore_rc = badInput();
            if (R_SUCCEEDED(restore_rc)) restore_rc = serviceDispatchInOut(&reader, 2, instance, restored);
            if (R_SUCCEEDED(restore_rc) && memcmp(restored, property, sizeof(property))) restore_rc = badInput();
        }
        labLogResult("ARP verify original process mapping restored", restore_rc);
        if (R_FAILED(restore_rc)) {
            labLog("ARP restoration failed: close the homebrew host game completely before relaunching");
            if (R_SUCCEEDED(create_rc)) labLogResult("LDN destroy network after restoration failure", ldnDestroyNetwork());
            rc = restore_rc;
        } else rc = create_rc;
    }
    serviceClose(&writer); serviceClose(&reader);
    return rc;
}
