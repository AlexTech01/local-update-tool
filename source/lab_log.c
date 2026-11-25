#include "lab_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

static FILE *s_log;

bool labLogOpen(void) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/local-update-tool", 0777);
    s_log = fopen("sdmc:/switch/local-update-tool/diagnostics.log", "w");
    labLog("=== Local Update Tool 1.0.0: custom coordinator / native game receive-install ===");
    labLog("System tick frequency=%llu Hz",(unsigned long long)armGetSystemTickFreq());
    return s_log != NULL;
}

void labLogClose(void) {
    if (s_log) fclose(s_log);
    s_log = NULL;
}

void labLog(const char *format, ...) {
    if (!s_log) return;
    fprintf(s_log, "[%llu] ", (unsigned long long)armGetSystemTick());
    va_list args;
    va_start(args, format);
    vfprintf(s_log, format, args);
    va_end(args);
    fputc('\n', s_log);
    fflush(s_log);
}

void labLogResult(const char *operation, Result rc) {
    labLog("%s: 0x%08x (module=%u description=%u)", operation,
           rc, R_MODULE(rc), R_DESCRIPTION(rc));
}

void labLogBytes(const char *name, const void *buffer, size_t size) {
    const u8 *bytes = buffer;
    labLog("%s (%zu bytes):", name, size);
    if (!s_log) return;
    for (size_t i = 0; i < size; i++) {
        fprintf(s_log, "%02x%s", bytes[i], (i % 16 == 15) ? "\n" : " ");
    }
    if (size % 16) fputc('\n', s_log);
    fflush(s_log);
}

