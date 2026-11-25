#include "title_name.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char nacp[LabNacpSize];
static char name[80];
static LabTitleInfo info;

static LabTitleResult readName(unsigned language) {
    return labReadTitleName(name, sizeof(name), nacp, sizeof(nacp), language, &info);
}

static void readFixture(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fread(nacp, 1, sizeof(nacp), file) == sizeof(nacp));
    assert(fgetc(file) == EOF);
    fclose(file);
}

int main(int argc, char **argv) {
    assert(argc == 5);
    /* Legacy records, including a publisher-only slot and a language beyond
       the eight-bit mask used by the SDK's NS name-selection helper. */
    memcpy(nacp + 2 * LabTitleEntrySize, "Japanese title", 15);
    memcpy(nacp + 15 * LabTitleEntrySize, "Portuguese title", 17);
    memcpy(nacp + LabTitleNameSize, "Publisher only", 15);
    assert(readName(15) == LabTitle_Ok);
    assert(strcmp(name, "Portuguese title") == 0 && info.language_index == 15);
    assert(readName(0) == LabTitle_Ok);
    assert(strcmp(name, "Japanese title") == 0 && info.language_index == 2);
    assert(readName(UINT32_MAX) == LabTitle_Ok);

    /* Independently produced Python/raw-zlib fixtures: format-1 title table
       with English, Dutch, and a slot above the original sixteen entries. */
    readFixture(argv[1]);
    assert(readName(0) == LabTitle_Ok && strcmp(name, "Compressed English") == 0);
    assert(info.format == 1 && info.compressed_size > 0);
    assert(readName(8) == LabTitle_Ok && strcmp(name, "Compressed Dutch") == 0);
    assert(readName(17) == LabTitle_Ok && strcmp(name, "Slot seventeen") == 0);
    assert(readName(31) == LabTitle_Ok && strcmp(name, "Compressed English") == 0);

    unsigned size = nacp[0] | (unsigned)nacp[1] << 8;
    nacp[0] = (unsigned char)(size - 1);
    nacp[1] = (unsigned char)((size - 1) >> 8);
    assert(readName(0) == LabTitle_InvalidDeflate && !name[0]);
    nacp[0] = 0xff; nacp[1] = 0xff;
    assert(readName(0) == LabTitle_BadCompressedSize);
    nacp[0] = nacp[1] = 0;
    assert(readName(0) == LabTitle_BadCompressedSize);

    /* A valid stream must have the expected bounded output, not just EOF. */
    readFixture(argv[2]);
    assert(readName(0) == LabTitle_InvalidDeflate);
    readFixture(argv[3]);
    assert(readName(0) == LabTitle_InvalidDeflate);
    readFixture(argv[4]);
    assert(readName(0) == LabTitle_Empty && !name[0]);

    memset(nacp, 0, sizeof(nacp));
    assert(readName(0) == LabTitle_Empty && !name[0]);
    nacp[LabTitleFormatOffset] = 2;
    assert(readName(0) == LabTitle_UnsupportedFormat);
    assert(labReadTitleName(name, sizeof(name), nacp, 16, 0, &info) == LabTitle_TruncatedNacp);
    assert(labReadTitleName(NULL, 0, nacp, sizeof(nacp), 0, &info) == LabTitle_BadInput);

    /* Console controls are sanitized; truncation does not split UTF-8. */
    memset(nacp, 0, sizeof(nacp));
    memcpy(nacp, "Ab\x1b" "c", 5);
    assert(readName(0) == LabTitle_Ok && strcmp(name, "Ab c") == 0);
    memcpy(nacp, "AB\xe2\x82\xac" "C", 7);
    char short_name[5];
    assert(labReadTitleName(short_name, sizeof(short_name), nacp, sizeof(nacp), 0, NULL) == LabTitle_Ok);
    assert(strcmp(short_name, "AB") == 0);
    puts("PASS: legacy/compressed names, language fallback, malformed records, UTF-8 boundaries");
}
