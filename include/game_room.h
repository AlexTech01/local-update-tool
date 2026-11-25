#pragma once
#include <switch.h>

typedef enum {
    LabGameRoom_Waiting, LabGameRoom_Joining, LabGameRoom_Ready, LabGameRoom_Preparing, LabGameRoom_WaitingSender,
    LabGameRoom_Receiving, LabGameRoom_Committing, LabGameRoom_Finishing, LabGameRoom_Done, LabGameRoom_Failed,
} LabGameRoomPhase;
typedef struct {
    bool open, peer_joined;
    LabGameRoomPhase phase;
    char local_name[0x81], peer_name[0x81];
    char message[128];
    u64 received, total;
} LabGameRoomStatus;

Result labGameRoomOpen(Service *sm_proxy, u64 application_id, const char *nickname);
void labGameRoomPoll(void);
void labGameRoomStart(void);
void labGameRoomClose(void);
const LabGameRoomStatus *labGameRoomGetStatus(void);
