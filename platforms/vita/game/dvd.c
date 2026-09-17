/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Direct, uncompressed GameCube ISO backend for the Vita port. */
#include "vita_platform.h"
#include "../vita_log.h"

#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MELEE_VITA_DISC_PATH "ux0:data/melee/GALE01.iso"
#define DVD_COMPLETION_CAPACITY 32

typedef struct {
    DVDCommandBlock* block;
    DVDFileInfo* file;
    DVDCBCallback block_callback;
    DVDCallback file_callback;
    s32 result;
    u32 transferred;
} VitaDvdCompletion;

static FILE* s_disc;
static u8* s_fst;
static u32 s_fst_size;
static u32 s_fst_count;
static s32 s_current_dir;
static BOOL s_initialized;
static BOOL s_auto_invalidation;
static BOOL s_auto_fatal;
static DVDDiskID s_disk_id;
static VitaDvdCompletion s_completions[DVD_COMPLETION_CAPACITY];
static u32 s_completion_head;
static u32 s_completion_count;

static u32 read_be32(const void* address)
{
    const u8* p = address;
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) |
           ((u32) p[2] << 8) | p[3];
}

static const u8* fst_entry(u32 index)
{
    return index < s_fst_count ? s_fst + index * 12u : NULL;
}

static BOOL entry_is_dir(u32 index)
{
    const u8* entry = fst_entry(index);
    return entry != NULL && entry[0] != 0;
}

static u32 entry_name_offset(u32 index)
{
    const u8* entry = fst_entry(index);
    return entry == NULL ? 0 : ((u32) entry[1] << 16) |
                               ((u32) entry[2] << 8) | entry[3];
}

static u32 entry_word1(u32 index)
{
    const u8* entry = fst_entry(index);
    return entry == NULL ? 0 : read_be32(entry + 4);
}

static u32 entry_word2(u32 index)
{
    const u8* entry = fst_entry(index);
    return entry == NULL ? 0 : read_be32(entry + 8);
}

static const char* entry_name(u32 index)
{
    u32 table = s_fst_count * 12u;
    u32 offset = entry_name_offset(index);
    if (table > s_fst_size || offset >= s_fst_size - table) return NULL;
    return (const char*) s_fst + table + offset;
}

static BOOL name_equal(const char* name, const char* part, size_t length)
{
    size_t i;
    if (name == NULL || strlen(name) != length) return FALSE;
    for (i = 0; i < length; ++i) {
        if (tolower((unsigned char) name[i]) !=
            tolower((unsigned char) part[i])) return FALSE;
    }
    return TRUE;
}

static s32 find_child(s32 directory, const char* name, size_t length)
{
    u32 i = (u32) directory + 1u;
    u32 end = entry_word2((u32) directory);
    while (i < end && i < s_fst_count) {
        if (name_equal(entry_name(i), name, length)) return (s32) i;
        if (entry_is_dir(i)) {
            u32 next = entry_word2(i);
            i = next > i ? next : i + 1u;
        } else {
            ++i;
        }
    }
    return -1;
}

static s32 read_at(void* output, u32 length, u32 offset)
{
    size_t amount;
    if (s_disc == NULL || output == NULL) return DVD_RESULT_FATAL_ERROR;
    if (fseek(s_disc, (long) offset, SEEK_SET) != 0)
        return DVD_RESULT_FATAL_ERROR;
    amount = fread(output, 1, length, s_disc);
    return amount == length ? (s32) amount : DVD_RESULT_FATAL_ERROR;
}

static void queue_completion(DVDCommandBlock* block, DVDFileInfo* file,
                             DVDCBCallback block_callback,
                             DVDCallback file_callback, s32 result,
                             u32 transferred)
{
    u32 slot;
    if (s_completion_count == DVD_COMPLETION_CAPACITY)
        OSPanic(__FILE__, __LINE__, "DVD completion queue overflow");
    slot = (s_completion_head + s_completion_count++) % DVD_COMPLETION_CAPACITY;
    s_completions[slot].block = block;
    s_completions[slot].file = file;
    s_completions[slot].block_callback = block_callback;
    s_completions[slot].file_callback = file_callback;
    s_completions[slot].result = result;
    s_completions[slot].transferred = transferred;
}

void melee_vita_dvd_poll(void)
{
    while (s_completion_count != 0) {
        VitaDvdCompletion completion = s_completions[s_completion_head];
        s_completion_head = (s_completion_head + 1u) % DVD_COMPLETION_CAPACITY;
        --s_completion_count;
        if (completion.block != NULL) {
            completion.block->transferredSize = completion.transferred;
            completion.block->state = completion.result >= 0
                                          ? DVD_STATE_END
                                          : DVD_STATE_FATAL_ERROR;
        }
        if (completion.file_callback != NULL)
            completion.file_callback(completion.result, completion.file);
        if (completion.block_callback != NULL)
            completion.block_callback(completion.result, completion.block);
    }
}

/* The decomp keeps the two HSD font atlases (debug font and the sislib glyphs
 * used for menu description text) out of the repo; the PC port lifts them
 * from main.dol at boot (src/pc/discfont.c).  Do the same from the ISO. */
/* Raw byte views of the atlases; the real headers clash with dolphin/types. */
extern u8 HSD_DebugFontAtlas[];  /* 128 x DebugFontGlyph (56 bytes) */
extern u8 HSD_SisLib_FontAtlas[]; /* 287 x TextGlyphTexture (512 bytes) */
#define MELEE_VITA_DEBUG_FONT_BYTES (56 * 128)
#define MELEE_VITA_SIS_GLYPH_BYTES 512
#define MELEE_VITA_SIS_GLYPH_COUNT 287

static long vita_find_bytes(const u8* hay, long hay_len, const u8* needle,
                            long needle_len)
{
    long i;
    for (i = 0; i + needle_len <= hay_len; i++) {
        if (hay[i] == needle[0] &&
            memcmp(hay + i, needle, (size_t) needle_len) == 0)
            return i;
    }
    return -1;
}

static void melee_vita_load_disc_fonts(const u8* header)
{
    static const u32 marks[4] = { 0x10808000u, 0x46808000u, 0x7C808000u,
                                  0xB3808000u };
    static const u8 kern_sig[] = { 0x09, 0x08, 0x09, 0x0C,
                                   0x09, 0x08, 0x08, 0x08 };
    static const u8 keys_sig[] = {
        0, 0, 0, 0x26, 0, 0, 0, 0xFF, 0, 0, 0, 0xE8, 0, 0, 0, 0xEF,
        0, 0, 0, 0x42, 0, 0, 0, 0xD6, 0, 0, 0, 0x01, 0, 0, 0, 0x54,
        0, 0, 0, 0x14, 0, 0, 0, 0xA3, 0, 0, 0, 0x80, 0, 0, 0, 0xFD,
        0, 0, 0, 0x6E,
    };
    u32 dol_off = read_be32(header + 0x420);
    u32 fst_off = read_be32(header + 0x424);
    long len, i, debug_off = -1, kern, keys, start, end, glyphs;
    u8* dol;

    if (fst_off <= dol_off || fst_off - dol_off > (8u << 20)) {
        OSReport("[FONT] implausible DOL extent %u..%u\n", dol_off, fst_off);
        return;
    }
    len = (long) (fst_off - dol_off);
    dol = malloc((size_t) len);
    if (dol == NULL || read_at(dol, (u32) len, dol_off) < 0) {
        OSReport("[FONT] cannot read main.dol\n");
        free(dol);
        return;
    }

    for (i = 0; i + 0x20 + MELEE_VITA_DEBUG_FONT_BYTES <= len; i += 4) {
        if (read_be32(dol + i) == marks[0] && read_be32(dol + i + 8) == marks[1] &&
            read_be32(dol + i + 16) == marks[2] &&
            read_be32(dol + i + 24) == marks[3])
        {
            debug_off = i + 0x20;
            break;
        }
    }
    if (debug_off >= 0)
        memcpy(HSD_DebugFontAtlas, dol + debug_off,
               (size_t) MELEE_VITA_DEBUG_FONT_BYTES);
    else
        OSReport("[FONT] debug font not found\n");

    kern = vita_find_bytes(dol, len, kern_sig, (long) sizeof kern_sig);
    keys = vita_find_bytes(dol, len, keys_sig, (long) sizeof keys_sig);
    if (kern < 0 || keys < 0) {
        OSReport("[FONT] sislib font boundaries not found\n");
        free(dol);
        return;
    }
    start = (kern + 0x240 + 31) & ~31L; /* GALE01 kerning table length */
    end = (keys - 0x8C) & ~31L;
    glyphs = (end - start) / MELEE_VITA_SIS_GLYPH_BYTES;
    if (glyphs <= 0 || glyphs > MELEE_VITA_SIS_GLYPH_COUNT ||
        start + glyphs * MELEE_VITA_SIS_GLYPH_BYTES > len)
    {
        OSReport("[FONT] implausible sislib glyph count %ld\n", glyphs);
    } else {
        memcpy(HSD_SisLib_FontAtlas, dol + start,
               (size_t) (glyphs * MELEE_VITA_SIS_GLYPH_BYTES));
        OSReport("[FONT] loaded %ld sislib glyphs\n", glyphs);
    }
    free(dol);
}

void DVDInit(void)
{
    u8 header[0x42C];
    u32 fst_offset;
    if (s_initialized) return;
    s_disc = fopen(MELEE_VITA_DISC_PATH, "rb");
    if (s_disc == NULL || fread(header, 1, sizeof(header), s_disc) != sizeof(header))
        goto failure;
    if (memcmp(header, "GALE01", 6) != 0 || read_be32(header + 0x1C) != 0xC2339F3Du)
        goto failure;

    memcpy(&s_disk_id, header, sizeof(s_disk_id));
    fst_offset = read_be32(header + 0x424);
    s_fst_size = read_be32(header + 0x428);
    if (s_fst_size < 12 || s_fst_size > 16u * 1024u * 1024u) goto failure;
    s_fst = malloc(s_fst_size);
    if (s_fst == NULL || read_at(s_fst, s_fst_size, fst_offset) < 0) goto failure;
    s_fst_count = read_be32(s_fst + 8);
    if (s_fst[0] == 0 || s_fst_count == 0 ||
        s_fst_count > s_fst_size / 12u) goto failure;
    s_current_dir = 0;
    s_initialized = TRUE;
    melee_vita_load_disc_fonts(header);
    OSReport("Melee Vita DVD: mounted %s (%u FST entries)\n",
             MELEE_VITA_DISC_PATH, s_fst_count);
    return;

failure:
    OSReport("Melee Vita DVD: failed to mount %s\n", MELEE_VITA_DISC_PATH);
    free(s_fst);
    s_fst = NULL;
    if (s_disc != NULL) fclose(s_disc);
    s_disc = NULL;
    s_fst_size = s_fst_count = 0;
}

s32 DVDConvertPathToEntrynum(const char* path)
{
    s32 current;
    const char* cursor;
    if (!s_initialized || path == NULL) return -1;
    current = path[0] == '/' ? 0 : s_current_dir;
    cursor = path + (path[0] == '/');
    while (*cursor != '\0') {
        const char* end;
        size_t length;
        while (*cursor == '/') ++cursor;
        if (*cursor == '\0') break;
        end = cursor;
        while (*end != '\0' && *end != '/') ++end;
        length = (size_t) (end - cursor);
        if (length == 1 && cursor[0] == '.') {
            /* Keep current directory. */
        } else if (length == 2 && cursor[0] == '.' && cursor[1] == '.') {
            current = current == 0 ? 0 : (s32) entry_word1((u32) current);
        } else {
            current = find_child(current, cursor, length);
            if (current < 0) return -1;
        }
        cursor = end;
    }
    return current;
}

BOOL DVDFastOpen(s32 entry, DVDFileInfo* info)
{
    if (!s_initialized || info == NULL || entry < 0 ||
        (u32) entry >= s_fst_count || entry_is_dir((u32) entry)) return FALSE;
    memset(info, 0, sizeof(*info));
    info->startAddr = entry_word1((u32) entry);
    info->length = entry_word2((u32) entry);
    info->cb.state = DVD_STATE_END;
    return TRUE;
}

BOOL DVDOpen(const char* path, DVDFileInfo* info)
{
    return DVDFastOpen(DVDConvertPathToEntrynum(path), info);
}

BOOL DVDClose(DVDFileInfo* info)
{
    if (info == NULL) return FALSE;
    info->cb.state = DVD_STATE_END;
    return TRUE;
}

s32 DVDReadPrio(DVDFileInfo* info, void* output, s32 length, s32 offset,
                s32 priority)
{
    s32 result;
    (void) priority;
    if (info == NULL || length < 0 || offset < 0 ||
        (uint64_t) (u32) offset + (u32) length >
            (uint64_t) info->length + DVD_MIN_TRANSFER_SIZE - 1u)
        return DVD_RESULT_FATAL_ERROR;
    info->cb.state = DVD_STATE_BUSY;
    result = read_at(output, (u32) length, info->startAddr + (u32) offset);
    info->cb.transferredSize = result >= 0 ? (u32) result : 0;
    info->cb.state = result >= 0 ? DVD_STATE_END : DVD_STATE_FATAL_ERROR;
    return result;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* info, void* output, s32 length, s32 offset,
                      DVDCallback callback, s32 priority)
{
    s32 result;
    (void) priority;
    if (info == NULL || length < 0 || offset < 0 ||
        (uint64_t) (u32) offset + (u32) length >
            (uint64_t) info->length + DVD_MIN_TRANSFER_SIZE - 1u) {
        melee_vita_log_info("[DVD] async read rejected start=0x%08x file_len=%u off=%d len=%d",
                            info != NULL ? (unsigned) info->startAddr : 0u,
                            info != NULL ? (unsigned) info->length : 0u,
                            (int) offset, (int) length);
        return FALSE;
    }
    info->callback = callback;
    info->cb.state = DVD_STATE_BUSY;
    result = read_at(output, (u32) length, info->startAddr + (u32) offset);
    if (result < 0)
        melee_vita_log_info("[DVD] async read failed start=0x%08x file_len=%u off=%d len=%d",
                            (unsigned) info->startAddr, (unsigned) info->length,
                            (int) offset, (int) length);
    queue_completion(&info->cb, info, NULL, callback, result,
                     result >= 0 ? (u32) result : 0);
    return TRUE;
}

int DVDReadAbsAsyncPrio(DVDCommandBlock* block, void* output, s32 length,
                        s32 offset, DVDCBCallback callback, s32 priority)
{
    s32 result;
    (void) priority;
    if (block == NULL || length < 0 || offset < 0) return FALSE;
    block->command = DVD_COMMAND_READ;
    block->state = DVD_STATE_BUSY;
    block->addr = output;
    block->length = (u32) length;
    block->offset = (u32) offset;
    block->callback = callback;
    result = read_at(output, (u32) length, (u32) offset);
    queue_completion(block, NULL, callback, NULL, result,
                     result >= 0 ? (u32) result : 0);
    return TRUE;
}

s32 DVDGetFileInfoStatus(const DVDFileInfo* info)
{
    return info == NULL ? DVD_STATE_END : info->cb.state;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block)
{
    return block == NULL ? DVD_STATE_END : block->state;
}

s32 DVDGetTransferredSize(DVDFileInfo* info)
{
    return info == NULL ? 0 : (s32) info->cb.transferredSize;
}

s32 DVDGetDriveStatus(void)
{
    return s_initialized ? DVD_STATE_END : DVD_STATE_NO_DISK;
}

BOOL DVDCheckDisk(void) { return s_initialized; }
DVDDiskID* DVDGetCurrentDiskID(void) { return &s_disk_id; }
void* DVDGetFSTLocation(void) { return s_fst; }

BOOL DVDChangeDir(const char* path)
{
    s32 entry = DVDConvertPathToEntrynum(path);
    if (entry < 0 || !entry_is_dir((u32) entry)) return FALSE;
    s_current_dir = entry;
    return TRUE;
}

BOOL DVDSetAutoInvalidation(BOOL enabled)
{
    BOOL previous = s_auto_invalidation;
    s_auto_invalidation = enabled;
    return previous;
}

int DVDSetAutoFatalMessaging(BOOL enabled)
{
    BOOL previous = s_auto_fatal;
    s_auto_fatal = enabled;
    return previous;
}

void DVDPause(void) {}
void DVDResume(void) {}
void DVDReset(void) {}
int DVDResetRequired(void) { return FALSE; }

int DVDCompareDiskID(const DVDDiskID* a, const DVDDiskID* b)
{
    if (a == NULL || b == NULL) return FALSE;
    return memcmp(a, b, sizeof(*a)) == 0;
}

DVDDiskID* DVDGenerateDiskID(DVDDiskID* id, const char* game,
                             const char* company, u8 disc, u8 version)
{
    if (id == NULL) return NULL;
    memset(id, 0, sizeof(*id));
    if (game != NULL) memcpy(id->gameName, game, 4);
    if (company != NULL) memcpy(id->company, company, 2);
    id->diskNumber = disc;
    id->gameVersion = version;
    return id;
}
