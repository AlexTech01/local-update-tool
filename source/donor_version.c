#include "donor_version.h"
#include "delivery_wire.h"
#include "lab_log.h"

Result labCheckDonorVersion(const NsApplicationDeliveryInfo *donor, s32 donor_count,
    const NsApplicationDeliveryInfo *local, s32 local_count,
    LabDonorVersionDecision *decision) {
    if (decision) *decision = LabDonorVersion_Unchecked;
    if (!decision || !donor || donor_count < 1 || donor_count > LabDeliveryMaxRecords ||
        local_count < 0 || local_count > LabDeliveryMaxRecords || (!local && local_count))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!local_count) {
        /* Compare authenticates delivery records in NS. With no local record,
           compare the donor with itself rather than skipping authentication. */
        s32 self_comparison=0;
        Result rc=nsCompareApplicationDeliveryInfo(donor,donor_count,donor,donor_count,&self_comparison);
        labLogResult("NS authenticate donor delivery records without local records",rc);
        if(R_FAILED(rc))return rc;
        if(self_comparison)return MAKERESULT(Module_Libnx,LibnxError_BadInput);
        *decision = LabDonorVersion_Receive;
        return 0;
    }
    s32 comparison = 0;
    Result rc = nsCompareApplicationDeliveryInfo(donor, donor_count, local, local_count, &comparison);
    labLogResult("NS compare donor and local game versions", rc);
    if (R_FAILED(rc)) return rc;
    labLog("Donor game version comparison=%d (donor vs receiver)", comparison);
    if (comparison < 0) {
        labLog("Older donor patch selected for native receive");
        *decision = LabDonorVersion_Older;
        return 0;
    }
    if (comparison > 0) {
        *decision = LabDonorVersion_Receive;
        return 0;
    }
    bool have_all = false;
    rc = nsHasAllContentsToDeliver(donor, donor_count, &have_all);
    labLogResult("NS check existing game patch contents", rc);
    if (R_FAILED(rc)) return rc;
    labLog("Equal game version: all_contents_present=%u",have_all);
    *decision = LabDonorVersion_Receive;
    return 0;
}
