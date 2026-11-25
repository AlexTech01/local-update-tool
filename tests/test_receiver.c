#include "game_receiver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const u64 app=UINT64_C(0x01003a400c3da000);
static NcmContentMetaKey key;
static s32 expected_count=1;
static unsigned requests, commits, closes, get_calls;
static Result request_rc, wait_rc, get_rc, commit_rc, progress_rc;
static bool active=true,received_task;
static NsReceiveApplicationProgress progress;
void labLog(const char *fmt, ...) { (void)fmt; }
void labLogResult(const char *stage, Result rc) { (void)stage; (void)rc; }
Result nsRequestReceiveApplication(AsyncResult *a,u32 ip,u16 port,u64 id,
                                  const NcmContentMetaKey *k,s32 count,NcmStorageId storage) {
    requests++;
    assert(ip==0xa9fe0102 && port==55556 && id==app);
    assert(count==expected_count && storage==6 && !memcmp(k,&key,sizeof(key)));
    a->s.active=active; return request_rc;
}
Result nsCommitReceiveApplication(u64 id) {
    assert(id==app);commits++;
    /* NS requires the NIM task in ready state 3. Closing the receive object
       deletes that task, even after its async result reported success. */
    if(!received_task)return 0x6cc10;
    if(!commit_rc)received_task=false;
    return commit_rc;
}
Result nsGetReceiveApplicationProgress(u64 id,NsReceiveApplicationProgress *out) {
    assert(id==app); *out=progress; return progress_rc;
}
Result asyncResultWait(AsyncResult *a,u64 timeout) { assert(a->s.active && timeout==0); return wait_rc; }
Result asyncResultGet(AsyncResult *a) {
    assert(a->s.active && wait_rc==0);get_calls++;if(!get_rc)received_task=true;return get_rc;
}
void asyncResultClose(AsyncResult *a) { assert(a->s.active);closes++;received_task=false;a->s.active=false; }
static void reset(void) {
    requests=commits=closes=get_calls=0;
    request_rc=wait_rc=get_rc=commit_rc=progress_rc=0;
    active=true;received_task=false;expected_count=1;progress=(NsReceiveApplicationProgress){0};
    key=(NcmContentMetaKey){.id=app|0x800,.version=65536,.type=0x81};
}
static void start(LabReceiver *r) { assert(!labReceiverStart(r,0xa9fe0102,app,&key,1)); }
int main(void) {
    reset(); LabReceiver r={0};
    start(&r); assert(r.status.phase==LabReceive_Receiving && requests==1 && commits==0);
    assert(labReceiverStart(&r,0xa9fe0102,app,&key,1)); assert(requests==1);
    wait_rc=KERNELRESULT(TimedOut); progress=(NsReceiveApplicationProgress){100,200};
    labReceiverPoll(&r); assert(r.status.received==100 && r.status.total==200 && !commits && !get_calls);
    progress=(NsReceiveApplicationProgress){-1,-2}; labReceiverPoll(&r);
    assert(!r.status.received && !r.status.total);
    progress_rc=0x1234; labReceiverPoll(&r); assert(r.status.phase==LabReceive_Receiving && !commits);
    wait_rc=0; labReceiverPoll(&r);
    assert(r.status.phase==LabReceive_ReadyToCommit && get_calls==1 && !closes && !commits && received_task && r.async_open);
    labReceiverPoll(&r); assert(r.status.phase==LabReceive_Installed && commits==1 && closes==1 && !r.async_open);
    for(int i=0;i<10;i++) labReceiverPoll(&r);
    labReceiverCancel(&r); assert(commits==1 && closes==1 && r.status.phase==LabReceive_Installed);

    /* Already-present content may complete immediately. Preserve the native
       result/commit sequence without claiming that any payload bytes moved. */
    reset(); r=(LabReceiver){0}; start(&r);
    labReceiverPoll(&r);
    assert(r.status.phase==LabReceive_ReadyToCommit && !r.status.received && !r.status.total && !commits && !closes && received_task);
    labReceiverPoll(&r);
    assert(r.status.phase==LabReceive_Installed && commits==1 && closes==1);

    reset(); r=(LabReceiver){0}; request_rc=0x1234;
    assert(labReceiverStart(&r,0xa9fe0102,app,&key,1)==request_rc);
    assert(r.status.phase==LabReceive_Failed && closes==1); labReceiverPoll(&r); assert(!commits);
    reset(); r=(LabReceiver){0}; active=false;
    assert(labReceiverStart(&r,0xa9fe0102,app,&key,1)); assert(!closes && !commits);

    reset(); r=(LabReceiver){0}; start(&r); get_rc=0x3456;
    labReceiverPoll(&r); assert(r.status.phase==LabReceive_Failed && r.status.result==get_rc);
    labReceiverPoll(&r); assert(!commits && closes==1);
    reset(); r=(LabReceiver){0}; start(&r); wait_rc=0x7890;
    labReceiverPoll(&r); assert(r.status.phase==LabReceive_Failed && !get_calls && !commits && closes==1);

    for(int ready=0;ready<2;ready++) {
        reset(); r=(LabReceiver){0}; start(&r);
        if(ready) labReceiverPoll(&r);
        labReceiverCancel(&r); labReceiverCancel(&r); labReceiverPoll(&r);
        assert(r.status.phase==LabReceive_Cancelled && !commits && closes==1);
    }
    reset(); r=(LabReceiver){0}; start(&r); commit_rc=0x5678;
    labReceiverPoll(&r); labReceiverPoll(&r); labReceiverPoll(&r);
    assert(r.status.phase==LabReceive_Failed && r.status.result==commit_rc && commits==1 && closes==1 && !received_task);
    reset(); r=(LabReceiver){0}; key.type=3;
    assert(labReceiverStart(&r,0xa9fe0102,app,&key,1) && !requests);
    key.type=0x81; key.id++;
    assert(labReceiverStart(&r,0xa9fe0102,app,&key,1) && !requests);
    reset();r=(LabReceiver){0};NcmContentMetaKey multiple[2]={key,key};multiple[1].version++;
    expected_count=2;assert(!labReceiverStart(&r,0xa9fe0102,app,multiple,2));
    labReceiverCancel(&r);assert(requests==1 && !commits);
    for(unsigned i=0;i<2;i++) {
        reset();r=(LabReceiver){0};multiple[0]=multiple[1]=key;multiple[1].version++;
        multiple[i].type=3;
        assert(labReceiverStart(&r,0xa9fe0102,app,multiple,2) && !requests && !commits);
    }
    reset();r=(LabReceiver){0};
    assert(labReceiverStart(&r,0xa9fe0102,app,&key,0) && !requests);
    assert(labReceiverStart(&r,0xa9fe0102,app,&key,19) && !requests);
    puts("PASS: native receive/commit ordering, cancellation, errors, multi-key bounds and firmware-key rejection");
}
