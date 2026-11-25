#include <switch.h>
#include <kaihen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lab_log.h"
#include "game_room.h"
#include "title_name.h"

#ifndef __SWITCH2__
#error This application must be built against libounce for Switch 2.
#endif

#define TITLE_LIMIT 256
#define CONTROL_DATA_SIZE 0x36000u
#define CONTROL_DATA_MAX_SIZE 0x80000u
#define NS_CONTROL_BUFFER_TOO_SMALL 0x5010u
#define LAB_MIN_HEN_PROTOCOL 6u

static NsApplicationRecord s_titles[TITLE_LIMIT];
static s32 s_title_count;
static unsigned s_selected;
static char s_title_name[80];
static char s_message[160] = "Ready.";
static bool s_ns_ready, s_hen_ready, s_log_ready;
static Service s_hen, s_sm_proxy;
static SetSysFirmwareVersion s_firmware;
static bool s_firmware_known;
static unsigned s_title_language;
static void draw(void);

static void report(const char *stage, Result rc, const char *success, const char *failure) {
    labLogResult(stage, rc);
    snprintf(s_message, sizeof(s_message), "%s", R_SUCCEEDED(rc) ? success : failure);
}

static void readTitleName(void) {
    s_title_name[0] = 0;
    if (!s_ns_ready || !s_title_count) return;
    /* The SDK's icon[0x20000] makes sizeof(NsApplicationControlData) too
       small for some Ounce records. LRP itself uses a 0x36000-byte buffer. */
    size_t control_size = CONTROL_DATA_SIZE;
    NsApplicationControlData *control = calloc(1, control_size);
    const u64 app_id = s_titles[s_selected].application_id;
    if (!control) {
        labLog("Title %016llx: allocation failed", (unsigned long long)app_id);
        return;
    }
    _Static_assert(sizeof(NacpStruct) == LabNacpSize, "NACP size");
    _Static_assert(offsetof(NacpStruct, titles_data_format) == LabTitleFormatOffset, "NACP title format");
    _Static_assert(CONTROL_DATA_SIZE > sizeof(NsApplicationControlData), "Use Ounce control-data capacity");
    const struct {
        NsApplicationControlSource source;
        bool modern;
        u8 acd_index;
        const char *name;
    } attempts[] = {
        {NsApplicationControlSource_Storage, true, 0, "modern cache/storage ACD0"},
        {NsApplicationControlSource_StorageOnly, true, 0, "modern storage-only ACD0"},
        {NsApplicationControlSource_Storage, false, 0, "legacy cache/storage ACD0"},
        {NsApplicationControlSource_StorageOnly, false, 0, "legacy storage-only ACD0"},
        {NsApplicationControlSource_Storage, true, 1, "modern cache/storage ACD1"},
        {NsApplicationControlSource_StorageOnly, true, 1, "modern storage-only ACD1"},
    };
    for (unsigned i = 0; i < sizeof(attempts) / sizeof(*attempts); i++) {
        u64 actual = 0;
        Result rc;
        for (;;) {
            memset(control, 0, control_size);
            actual = 0;
            u32 unused = 0;
            rc = attempts[i].modern
                ? nsGetApplicationControlData2(attempts[i].source, app_id, control,
                    control_size, 0, attempts[i].acd_index, &actual, &unused)
                : nsGetApplicationControlData(attempts[i].source, app_id, control,
                    control_size, &actual);
            labLog("Title %016llx %s: rc=%08x capacity=%zu actual=%llu",
                (unsigned long long)app_id, attempts[i].name, rc, control_size,
                (unsigned long long)actual);
            if (rc != NS_CONTROL_BUFFER_TOO_SMALL || control_size >= CONTROL_DATA_MAX_SIZE) break;
            void *larger = realloc(control, CONTROL_DATA_MAX_SIZE);
            if (!larger) {
                labLog("Title control-data buffer growth failed");
                break;
            }
            control = larger;
            control_size = CONTROL_DATA_MAX_SIZE;
        }
        if (R_FAILED(rc)) continue;
        if (actual > control_size) {
            labLog("Title result size exceeds receive-buffer capacity");
            continue;
        }
        LabTitleInfo info;
        LabTitleResult decoded = labReadTitleName(s_title_name, sizeof(s_title_name),
            &control->nacp, actual, s_title_language, &info);
        labLog("Title decode: %s format=%u compressed_size=%u language=%d",
            labTitleResultName(decoded), info.format, info.compressed_size, info.language_index);
        if (decoded == LabTitle_Ok) break;
    }
    free(control);
}

static void loadTitles(void) {
    s_title_count = 0;
    s_selected = 0;
    if (!s_ns_ready) return;
    Result rc = nsListApplicationRecord(s_titles, TITLE_LIMIT, 0, &s_title_count);
    report("List installed games", rc, "Ready.", "Couldn't load games. See log.");
    if (R_FAILED(rc) || s_title_count < 0 || s_title_count > TITLE_LIMIT) s_title_count = 0;
    labLog("application records returned=%d (capacity=%d)", s_title_count, TITLE_LIMIT);
    readTitleName();
}

static void initializeServices(void) {
    Service ro = {0};
    Result rc = kaihenGetRoSession(&ro);
    labLogResult("Get hbloader RO override", rc);
    if (R_SUCCEEDED(rc)) rc = kaihenOpenHomebrewSession(&ro, &s_hen);
    labLogResult("Open HEN homebrew session", rc);
    HenStatus status = {0};
    if (R_SUCCEEDED(rc)) rc = henGetStatus(&s_hen, &status);
    labLogResult("HEN GetStatus", rc);
    if (R_SUCCEEDED(rc)) {
        labLog("HEN protocol=%u initialization_result=%08x", status.protocol_version,
            status.initialization_result);
        rc = status.initialization_result;
        if (R_SUCCEEDED(rc) && status.protocol_version < LAB_MIN_HEN_PROTOCOL)
            rc = MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    }
    if (R_SUCCEEDED(rc)) {
        Handle handle = INVALID_HANDLE;
        rc = henGetServiceManager(&s_hen, &handle);
        labLogResult("HEN GetServiceManager", rc);
        if (R_SUCCEEDED(rc)) {
            serviceCreate(&s_sm_proxy, handle);
            s_hen_ready = true;
        }
    }
    if (serviceIsActive(&s_hen)) {
        Handle handle = INVALID_HANDLE;
        Result frc = henGetSettingsSession(&s_hen, &handle);
        labLogResult("HEN GetSettingsSession (diagnostic only)", frc);
        if (R_SUCCEEDED(frc)) {
            Service settings;
            serviceCreate(&settings, handle);
            /* Use the modern query and an ordinary heap receive buffer. */
            SetSysFirmwareVersion *version = calloc(1, sizeof(*version));
            if (version) {
                frc = serviceDispatch(&settings, 4,
                    .buffer_attrs = {SfBufferAttr_Out | SfBufferAttr_HipcPointer | SfBufferAttr_FixedSize},
                    .buffers = {{version, sizeof(*version)}});
                if (R_SUCCEEDED(frc)) s_firmware = *version;
                free(version);
            } else frc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
            serviceClose(&settings);
        }
        labLogResult("Read actual firmware from set:sys (diagnostic only)", frc);
        s_firmware_known = R_SUCCEEDED(frc);
        if (s_firmware_known)
            labLog("actual firmware=%u.%u.%u", s_firmware.major, s_firmware.minor, s_firmware.micro);
    }
    labLog("Firmware version gate removed; version queries do not block room creation");
    const Result hen_rc = rc;

    Result lrc = setInitialize();
    if (R_SUCCEEDED(lrc)) {
        u64 code = 0;
        SetLanguage language;
        lrc = setGetSystemLanguage(&code);
        if (R_SUCCEEDED(lrc)) lrc = setMakeLanguage(code, &language);
        /* SetLanguage ordering differs from the NACP language slot ordering. */
        static const u8 slots[] = {2, 0, 3, 4, 7, 6, 14, 12, 8, 10, 11, 13, 1, 9, 5, 14, 13, 15};
        if (R_SUCCEEDED(lrc) && (unsigned)language < sizeof(slots))
            s_title_language = slots[language];
        setExit();
    }
    labLogResult("Read title language (English fallback)", lrc);
    labLog("Preferred NACP language slot=%u", s_title_language);

    rc = nsInitialize();
    labLogResult("nsInitialize", rc);
    s_ns_ready = R_SUCCEEDED(rc);
    if (s_ns_ready) loadTitles();
    else report("NS service setup failed", rc, "", "Couldn't load games. See log.");
    if (R_FAILED(hen_rc))
        report("kaiHEN service setup failed", hen_rc, "", "Room service unavailable. See log.");
}

static void readConsoleName(char name[0x81]) {
    snprintf(name, 0x81, "Switch");
    Result rc = setInitialize();
    labLogResult("Initialize settings for console nickname", rc);
    if (R_SUCCEEDED(rc)) {
        SetSysDeviceNickName nickname = {0};
        rc = setGetDeviceNickname(&nickname);
        labLogResult("Read console nickname", rc);
        setExit();
        if (R_SUCCEEDED(rc)) {
            bool visible = false;
            for (size_t i = 0; i < sizeof(nickname.nickname) && nickname.nickname[i]; i++) {
                unsigned char c = (unsigned char)nickname.nickname[i];
                if (c > ' ' && c != 0x7f) visible = true;
            }
            if (visible)
                snprintf(name, 0x81, "%.*s", (int)sizeof(nickname.nickname), nickname.nickname);
            else labLog("Console nickname empty; using fallback");
        }
    }
    labLog("Room/player nickname: %s", name);
}

static void openRoom(void) {
    if(!s_hen_ready || !s_ns_ready || !s_title_count) {
        snprintf(s_message,sizeof(s_message),"Can't create room. Check the game list and log.");return;
    }
    snprintf(s_message,sizeof(s_message),"Opening room...");draw();consoleUpdate(NULL);
    char nickname[0x81];readConsoleName(nickname);
    const u64 application=s_titles[s_selected].application_id;
    labLog("selected application=%016llx",(unsigned long long)application);
    report("Create custom game-update room",labGameRoomOpen(&s_sm_proxy,application,nickname),
        "","Couldn't create room. See log.");
}
static void drawPlayerName(const char *text) {
    char name[80];size_t length=0;
    while(length<0x80 && text[length])length++;
    if(length>=sizeof(name)) {
        length=sizeof(name)-1;
        while(length && ((unsigned char)text[length]&0xc0)==0x80)length--;
    }
    bool visible=false;
    for(size_t i=0;i<length;i++) {
        unsigned char c=(unsigned char)text[i];name[i]=c<0x20 || c==0x7f?' ':(char)c;
        if(c>' ' && c!=0x7f)visible=true;
    }
    name[length]=0;printf("%s",visible?name:"Switch");
}
static void draw(void) {
    printf("\x1b[2J\x1b[HLocal Update Tool 1.0.0\n\n");
    if(s_title_count) {
        printf("Game %u / %d\n%s (%016llX)\n\n",s_selected+1,s_title_count,
            s_title_name[0]?s_title_name:"Unknown title",(unsigned long long)s_titles[s_selected].application_id);
    } else printf("No games found.\n\n");
    const LabGameRoomStatus *room=labGameRoomGetStatus();
    if(room->open) {
        printf("%s\nPlayers: %u / 2\n  ",room->message,room->peer_joined?2:1);
        drawPlayerName(room->local_name);printf(" (you)\n");
        if(room->peer_joined){printf("  ");drawPlayerName(room->peer_name);printf("\n");}
        if(room->total)printf("\nGame data: %llu / %llu KiB\n",
            (unsigned long long)(room->received/1024),(unsigned long long)(room->total/1024));
        if(room->phase==LabGameRoom_Ready)printf("\nA Receive & install   ");else printf("\n");
        printf("B Close room   + Exit\n");
    } else printf("D-pad Left/Right Select   L/R Skip 10\nA Create room   Y Refresh   + Exit\n");
    if(s_message[0])printf("\n%s\n",s_message);
    if(!s_log_ready)printf("Logging unavailable.\n");
}

int main(int argc,char **argv) {
    (void)argc;(void)argv;consoleInit(NULL);
    padConfigureInput(1,HidNpadStyleSet_NpadStandard);PadState pad;padInitializeDefault(&pad);
    s_log_ready=labLogOpen();printf("Local Update Tool\n\nLoading...\n");consoleUpdate(NULL);
    initializeServices();bool dirty=true;
    while(appletMainLoop()) {
        padUpdate(&pad);u64 down=padGetButtonsDown(&pad);
        if(down&HidNpadButton_Plus)break;
        if(labGameRoomGetStatus()->open) {
            const LabGameRoomStatus previous=*labGameRoomGetStatus();
            if(down&HidNpadButton_B) {
                labGameRoomClose();snprintf(s_message,sizeof(s_message),"Room closed.");dirty=true;
            } else {
                if(down&HidNpadButton_A)labGameRoomStart();
                labGameRoomPoll();
                if(memcmp(&previous,labGameRoomGetStatus(),sizeof(previous)))dirty=true;
            }
        } else {
            unsigned old=s_selected;
            if(s_title_count) {
                if(down&HidNpadButton_Right)s_selected=(s_selected+1)%s_title_count;
                if(down&HidNpadButton_Left)s_selected=(s_selected+s_title_count-1)%s_title_count;
                if(down&HidNpadButton_R)s_selected=(s_selected+10)%s_title_count;
                if(down&HidNpadButton_L)s_selected=(s_selected+s_title_count-(10%s_title_count))%s_title_count;
            }
            if(old!=s_selected){readTitleName();dirty=true;}
            if(down&HidNpadButton_Y){loadTitles();dirty=true;}
            if(down&HidNpadButton_A){openRoom();dirty=true;}
        }
        if(dirty){draw();dirty=false;}consoleUpdate(NULL);
    }
    labGameRoomClose();if(s_ns_ready)nsExit();serviceClose(&s_sm_proxy);serviceClose(&s_hen);
    labLog("app exited");labLogClose();consoleExit(NULL);return 0;
}
