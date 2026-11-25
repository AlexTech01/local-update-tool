#include "game_receiver.h"
#include "delivery_wire.h"
#include "lab_log.h"

_Static_assert(sizeof(NcmContentMetaKey)==16,"Native content-meta key ABI");

static void releaseAsync(LabReceiver *receiver) {
    if (!receiver->async_open) return;
    /* This also waits for cancellation when a task is still running. */
    asyncResultClose(&receiver->async);
    receiver->async_open = false;
}
static void failed(LabReceiver *receiver, const char *stage, Result rc) {
    receiver->status.phase = LabReceive_Failed;
    receiver->status.stage = stage;
    receiver->status.result = rc;
    labLogResult(stage, rc);
    releaseAsync(receiver);
}
Result labReceiverStart(LabReceiver *receiver, u32 donor_ip, u64 application_id,
                        const NcmContentMetaKey *keys, unsigned count) {
    if (!receiver || receiver->status.phase != LabReceive_Idle || !donor_ip ||
        !labValidatePatchKeys(keys,count,application_id))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    receiver->application_id = application_id;
    receiver->status.stage = "NS RequestReceiveApplication";
    /* LRP's own local receiver selects Any (6). NS/NIM choose the actual
       installation storage and perform the normal verification/import work. */
    Result rc = nsRequestReceiveApplication(&receiver->async, donor_ip, LabContentPort,
        application_id, keys, (s32)count, NcmStorageId_Any);
    labLogResult(receiver->status.stage, rc);
    receiver->async_open = serviceIsActive(&receiver->async.s);
    if (R_FAILED(rc)) failed(receiver, receiver->status.stage, rc);
    else if (!receiver->async_open)
        failed(receiver, "NS receive returned no async object", MAKERESULT(Module_Libnx, LibnxError_BadInput));
    else receiver->status.phase = LabReceive_Receiving;
    return receiver->status.result;
}
void labReceiverPoll(LabReceiver *receiver) {
    if (receiver->status.phase == LabReceive_ReadyToCommit) {
        /* The caller has already processed cancellation/disconnects this frame.
           From this call until its return, commit cannot be cancelled. */
        receiver->status.stage = "NS CommitReceiveApplication";
        labLog("NS commit starting with receive async retained=%u",receiver->async_open);
        Result rc = nsCommitReceiveApplication(receiver->application_id);
        labLogResult(receiver->status.stage, rc);
        receiver->status.result = rc;
        receiver->status.phase = R_SUCCEEDED(rc) ? LabReceive_Installed : LabReceive_Failed;
        /* The NS receive object's destructor deletes the NIM receive task.
           Stock LRP retains it until after commit; only now release it. */
        releaseAsync(receiver);
        return;
    }
    if (receiver->status.phase != LabReceive_Receiving) return;
    Result rc = asyncResultWait(&receiver->async, 0);
    if (R_SUCCEEDED(rc)) {
        rc = asyncResultGet(&receiver->async);
        labLogResult("NS receive async result", rc);
        labLog("Native receive last reported progress=%llu/%llu bytes (not a network byte counter)",
            (unsigned long long)receiver->status.received,(unsigned long long)receiver->status.total);
        if (R_FAILED(rc)) failed(receiver, "NS receive async result", rc);
        else {
            labLog("Holding completed NS receive object until commit or cancellation");
            receiver->status.phase = LabReceive_ReadyToCommit;
            receiver->status.stage = "Game data verified; waiting to commit";
        }
        return;
    }
    if (rc != KERNELRESULT(TimedOut)) {
        failed(receiver, "Wait for NS receive", rc); return;
    }
    NsReceiveApplicationProgress progress = {0};
    rc = nsGetReceiveApplicationProgress(receiver->application_id, &progress);
    /* AsyncResult is authoritative. Early progress queries can fail while the
       task initializes. Do not convert a progress-only failure into an install. */
    if (R_SUCCEEDED(rc)) {
        receiver->status.received = progress.current_size > 0 ? (u64)progress.current_size : 0;
        receiver->status.total = progress.total_size > 0 ? (u64)progress.total_size : 0;
    }
}
void labReceiverCancel(LabReceiver *receiver) {
    if (receiver->status.phase == LabReceive_Receiving ||
        receiver->status.phase == LabReceive_ReadyToCommit) {
        receiver->status.phase = LabReceive_Cancelled;
        receiver->status.stage = "Game receive cancelled before commit";
        labLog("%s", receiver->status.stage);
    }
    releaseAsync(receiver);
}
