#ifndef VHBU_PENDING_UPDATE_H
#define VHBU_PENDING_UPDATE_H

#include <stdint.h>

#define VHBU_PENDING_MAGIC 0x56484255u
#define VHBU_PENDING_FORMAT 2u
#define VHBU_PENDING_PATH "ux0:data/VitaHomebrewUpdate/pending.bin"
#define VHBU_HELPER_TITLE_ID "VHBUHELP1"

typedef struct VhbuPendingUpdate {
    uint32_t magic;
    uint32_t format;
    char title_id[12];
    char version[16];
    uint64_t package_size;
    int64_t notification_rowid;
    char package_sha1[41];
    char content_id[64];
    char vpk_path[192];
    char stage_path[192];
} VhbuPendingUpdate;

#endif
