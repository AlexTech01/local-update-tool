#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    LabNacpSize = 0x4000,
    LabTitleFormatOffset = 0x3215,
    LabTitleEntrySize = 0x300,
    LabTitleNameSize = 0x200,
    LabTitleCompressedCapacity = 0x2ffe,
};

typedef enum {
    LabTitle_Ok,
    LabTitle_TruncatedNacp,
    LabTitle_UnsupportedFormat,
    LabTitle_BadCompressedSize,
    LabTitle_InvalidDeflate,
    LabTitle_NoMemory,
    LabTitle_Empty,
    LabTitle_BadInput,
} LabTitleResult;

typedef struct {
    unsigned format;
    unsigned compressed_size;
    int language_index;
} LabTitleInfo;

/* preferred_language is a NACP slot, not a SetLanguage value. */
LabTitleResult labReadTitleName(char *out, size_t capacity, const void *nacp,
    size_t size, unsigned preferred_language, LabTitleInfo *info);
const char *labTitleResultName(LabTitleResult result);
