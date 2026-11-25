#include "title_name.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static bool copyName(char *out, size_t capacity, const uint8_t *entry) {
    size_t length = 0;
    while (length < LabTitleNameSize && entry[length]) length++;
    if (length >= capacity) {
        length = capacity - 1;
        /* Do not split a UTF-8 sequence at the display-buffer boundary. */
        while (length && (entry[length] & 0xc0) == 0x80) length--;
    }
    bool visible = false;
    for (size_t i = 0; i < length; i++) {
        uint8_t c = entry[i];
        out[i] = c < 0x20 || c == 0x7f ? ' ' : (char)c;
        if ((unsigned char)out[i] > ' ') visible = true;
    }
    out[length] = 0;
    return visible;
}

LabTitleResult labReadTitleName(char *out, size_t capacity, const void *nacp,
    size_t size, unsigned preferred_language, LabTitleInfo *info) {
    LabTitleInfo local_info = {.language_index = -1};
    if (!info) info = &local_info;
    *info = local_info;
    if (!out || !capacity || !nacp) return LabTitle_BadInput;
    out[0] = 0;
    if (size < LabNacpSize) return LabTitle_TruncatedNacp;
    const uint8_t *bytes = nacp;
    info->format = bytes[LabTitleFormatOffset];
    const uint8_t *entries = bytes;
    uint8_t *decoded = NULL;
    unsigned count = 16;
    if (info->format == 1) {
        info->compressed_size = bytes[0] | ((unsigned)bytes[1] << 8);
        if (!info->compressed_size || info->compressed_size > LabTitleCompressedCapacity)
            return LabTitle_BadCompressedSize;
        count = 32;
        const size_t decoded_size = count * LabTitleEntrySize;
        decoded = malloc(decoded_size);
        if (!decoded) return LabTitle_NoMemory;
        z_stream stream = {0};
        stream.next_in = (Bytef *)bytes + 2;
        stream.avail_in = info->compressed_size;
        stream.next_out = decoded;
        stream.avail_out = decoded_size;
        int rc = inflateInit2(&stream, -15);
        bool valid = false;
        if (rc == Z_OK) {
            rc = inflate(&stream, Z_FINISH);
            valid = rc == Z_STREAM_END && stream.total_out == decoded_size && stream.avail_in == 0;
            inflateEnd(&stream);
        }
        if (!valid) {
            free(decoded);
            return LabTitle_InvalidDeflate;
        }
        entries = decoded;
    } else if (info->format != 0) {
        return LabTitle_UnsupportedFormat;
    }

    if (preferred_language < count &&
        copyName(out, capacity, entries + preferred_language * LabTitleEntrySize)) {
        info->language_index = (int)preferred_language;
    } else {
        /* A publisher-only entry must not hide a real title in another slot. */
        for (unsigned i = 0; i < count; i++) {
            if (copyName(out, capacity, entries + i * LabTitleEntrySize)) {
                info->language_index = (int)i;
                break;
            }
        }
    }
    free(decoded);
    if (info->language_index < 0) {
        out[0] = 0;
        return LabTitle_Empty;
    }
    return LabTitle_Ok;
}

const char *labTitleResultName(LabTitleResult result) {
    switch (result) {
        case LabTitle_Ok: return "ok";
        case LabTitle_TruncatedNacp: return "truncated NACP";
        case LabTitle_UnsupportedFormat: return "unsupported title format";
        case LabTitle_BadCompressedSize: return "invalid compressed size";
        case LabTitle_InvalidDeflate: return "invalid compressed title data";
        case LabTitle_NoMemory: return "allocation failed";
        case LabTitle_Empty: return "no nonempty title name";
        default: return "invalid input";
    }
}
