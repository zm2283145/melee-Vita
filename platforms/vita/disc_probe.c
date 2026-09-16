#include "disc_probe.h"

#include <stdio.h>
#include <string.h>

enum melee_vita_disc_result melee_vita_probe_disc(
    const char* path, struct melee_vita_disc_info* info)
{
    static const unsigned char expected_magic[4] = {0xc2, 0x33, 0x9f, 0x3d};
    unsigned char header[0x20];
    memset(info, 0, sizeof(*info));

    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return MELEE_VITA_DISC_OPEN_FAILED;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return MELEE_VITA_DISC_READ_FAILED;
    }
    const long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return MELEE_VITA_DISC_READ_FAILED;
    }
    info->size = (uint64_t) size;
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return MELEE_VITA_DISC_READ_FAILED;
    }
    fclose(file);

    memcpy(info->game_id, header, 6);
    info->game_id[6] = '\0';
    info->disc_number = header[6];
    info->revision = header[7];

    if (memcmp(header + 0x1c, expected_magic, sizeof(expected_magic)) != 0) {
        return MELEE_VITA_DISC_BAD_MAGIC;
    }
    if (memcmp(header, "GALE01", 6) != 0) {
        return MELEE_VITA_DISC_WRONG_GAME;
    }
    if (info->disc_number != 0 || info->revision != 2) {
        return MELEE_VITA_DISC_WRONG_REVISION;
    }
    if (info->size != MELEE_VITA_EXPECTED_DISC_SIZE) {
        return MELEE_VITA_DISC_WRONG_SIZE;
    }
    return MELEE_VITA_DISC_OK;
}

const char* melee_vita_disc_result_text(enum melee_vita_disc_result result)
{
    switch (result) {
    case MELEE_VITA_DISC_OK:
        return "PASS: Melee NTSC-U revision 1.02 disc detected.";
    case MELEE_VITA_DISC_OPEN_FAILED:
        return "FAIL: Could not open the external disc image.";
    case MELEE_VITA_DISC_READ_FAILED:
        return "FAIL: Could not read the external disc image.";
    case MELEE_VITA_DISC_BAD_MAGIC:
        return "FAIL: The file is not a valid GameCube disc image.";
    case MELEE_VITA_DISC_WRONG_GAME:
        return "FAIL: The disc is not Melee NTSC-U (GALE01).";
    case MELEE_VITA_DISC_WRONG_REVISION:
        return "FAIL: Melee revision 1.02 (disc revision 2) is required.";
    case MELEE_VITA_DISC_WRONG_SIZE:
        return "FAIL: Disc size differs from the expected uncompressed image.";
    default:
        return "FAIL: Unknown disc validation result.";
    }
}
