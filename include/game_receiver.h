#pragma once
#include <switch.h>

typedef enum {
    LabReceive_Idle, LabReceive_Receiving, LabReceive_ReadyToCommit,
    LabReceive_Installed, LabReceive_Failed, LabReceive_Cancelled,
} LabReceivePhase;
typedef struct {
    LabReceivePhase phase;
    Result result;
    const char *stage;
    u64 received, total;
} LabReceiveStatus;
typedef struct {
    LabReceiveStatus status;
    AsyncResult async;
    bool async_open;
    u64 application_id;
} LabReceiver;

Result labReceiverStart(LabReceiver *receiver, u32 donor_ip, u64 application_id,
                        const NcmContentMetaKey *keys, unsigned count);
/* Poll on the UI thread. ReadyToCommit lasts one frame with the native receive
   object retained. Commit is synchronous and is issued once, only after
   successful completion. Release the receive object after the commit returns. */
void labReceiverPoll(LabReceiver *receiver);
/* Cancels and waits before the caller tears down LDN/stopper. Never commits. */
void labReceiverCancel(LabReceiver *receiver);
