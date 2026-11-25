#include "donor_version.h"
#include <assert.h>
#include <stdio.h>

static NsApplicationDeliveryInfo donor[2], local[2];
static unsigned compare_calls, content_calls;
static Result compare_rc, content_rc;
static s32 self_comparison;
static s32 comparison, expected_count;
static bool complete;

void labLog(const char *format, ...) { (void)format; }
void labLogResult(const char *stage, Result rc) { (void)stage; (void)rc; }
Result nsCompareApplicationDeliveryInfo(const NsApplicationDeliveryInfo *a,s32 ac,
    const NsApplicationDeliveryInfo *b,s32 bc,s32 *out) {
    assert(a==donor && (b==local || b==donor) && ac==expected_count && bc==expected_count);
    compare_calls++; *out=b==donor?self_comparison:comparison; return compare_rc;
}
Result nsHasAllContentsToDeliver(const NsApplicationDeliveryInfo *records,s32 count,bool *out) {
    assert(records==donor && count==expected_count);
    content_calls++; *out=complete; return content_rc;
}
static void reset(void) {
    compare_calls=content_calls=0;
    compare_rc=content_rc=0;
    comparison=self_comparison=0;expected_count=1;complete=true;
}
static LabDonorVersionDecision check(void) {
    LabDonorVersionDecision decision=LabDonorVersion_Unchecked;
    assert(!labCheckDonorVersion(donor,expected_count,local,expected_count,&decision));
    return decision;
}
int main(void) {
    reset();comparison=-1;
    assert(check()==LabDonorVersion_Older && compare_calls==1 && !content_calls);
    reset();comparison=1;
    assert(check()==LabDonorVersion_Receive && compare_calls==1 && !content_calls);
    reset();
    assert(check()==LabDonorVersion_Receive);
    assert(compare_calls==1 && content_calls==1);
    reset();complete=false;
    assert(check()==LabDonorVersion_Receive);
    assert(compare_calls==1 && content_calls==1);

    reset();compare_rc=0x1234;
    LabDonorVersionDecision d=LabDonorVersion_Receive;
    assert(labCheckDonorVersion(donor,1,local,1,&d)==compare_rc);
    assert(d==LabDonorVersion_Unchecked && compare_calls==1 && !content_calls);
    reset();content_rc=0x5678;complete=false;
    assert(labCheckDonorVersion(donor,1,local,1,&d)==content_rc);
    assert(d==LabDonorVersion_Unchecked && compare_calls==1 && content_calls==1);
    reset();
    assert(!labCheckDonorVersion(donor,1,NULL,0,&d));
    assert(d==LabDonorVersion_Receive && compare_calls==1 && !content_calls);
    reset();compare_rc=0x74410;
    assert(labCheckDonorVersion(donor,1,NULL,0,&d)==compare_rc);
    assert(d==LabDonorVersion_Unchecked && compare_calls==1 && !content_calls);
    reset();self_comparison=1;
    assert(labCheckDonorVersion(donor,1,NULL,0,&d));
    assert(d==LabDonorVersion_Unchecked && compare_calls==1 && !content_calls);
    reset();expected_count=2;
    assert(check()==LabDonorVersion_Receive && content_calls==1);
    reset();d=LabDonorVersion_Receive;
    assert(labCheckDonorVersion(donor,0,local,1,&d));
    assert(labCheckDonorVersion(donor,17,local,1,&d));
    assert(labCheckDonorVersion(NULL,1,local,1,&d));
    assert(labCheckDonorVersion(donor,1,NULL,1,&d));
    assert(labCheckDonorVersion(donor,1,local,-1,&d));
    assert(labCheckDonorVersion(donor,1,local,17,&d));
    assert(labCheckDonorVersion(donor,1,local,1,NULL));
    assert(d==LabDonorVersion_Unchecked && !compare_calls && !content_calls);
    puts("PASS: older, equal and newer donor classification, incomplete repairs and native validation errors");
}
