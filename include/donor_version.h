#pragma once
#include <switch.h>

typedef enum {
    LabDonorVersion_Unchecked,
    LabDonorVersion_Receive,
    LabDonorVersion_Older,
} LabDonorVersionDecision;

/* Authenticate and compare native delivery records. Older identifies a
   receive that needs a launch version reset after commit. */
Result labCheckDonorVersion(const NsApplicationDeliveryInfo *donor, s32 donor_count,
    const NsApplicationDeliveryInfo *local, s32 local_count,
    LabDonorVersionDecision *decision);
