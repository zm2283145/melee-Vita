/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Direct, uncompressed GameCube ISO backend for the Vita port. */
#include "vita_platform.h"
#include "../vita_log.h"

#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MELEE_VITA_DISC_PATH "ux0:data/melee/GALE01.iso"
#define DVD_COMPLETION_CAPACITY 32
#define DVD_REQUEST_CAPACITY DVD_COMPLETION_CAPACITY
#define DVD_THREAD_PRIORITY 0x10000100
#define DVD_THREAD_STACK (64u * 1024u)
/* Giga Bowser color costumes are bundled with the VPK and copied to data.
 * The normal costume still comes from the user's ISO. */
#define VITA_DVD_OVERLAY_BASE 0x80000000u
#define VITA_DVD_OVERLAY_SHIFT 24u
#define VITA_DVD_OVERLAY_OFFSET_MASK 0x00FFFFFFu
#define VITA_DVD_OVERLAY_COUNT 5u
#define VITA_DVD_COSTUME_DIR "ux0:data/melee/costumes/"

typedef struct {
    const char* name;
    FILE* sync_file;
    SceUID async_file;
    u32 length;
} VitaDvdOverlay;

static VitaDvdOverlay s_overlays[VITA_DVD_OVERLAY_COUNT] = {
    { "PlGkRe.dat", NULL, -1, 0 },
    { "PlGkBu.dat", NULL, -1, 0 },
    { "PlGkBk.dat", NULL, -1, 0 },
    { "PlGkYe.dat", NULL, -1, 0 },
    { "PlGkWh.dat", NULL, -1, 0 },
};

typedef struct {
    DVDCommandBlock* block;
    DVDFileInfo* file;
    DVDCBCallback block_callback;
    DVDCallback file_callback;
    s32 result;
    u32 transferred;
} VitaDvdCompletion;

typedef struct {
    DVDCommandBlock* block;
    DVDFileInfo* file;
    DVDCBCallback block_callback;
    DVDCallback file_callback;
    void* output;
    u32 length;
    u32 offset;
    u64 queued_at;
} VitaDvdRequest;

static FILE* s_disc;
static SceUID s_async_disc = -1;
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
static VitaDvdRequest s_requests[DVD_REQUEST_CAPACITY];
static u32 s_request_head;
static u32 s_request_count;
static u32 s_outstanding_count;
static SceUID s_request_sema = -1;
static SceUID s_queue_mutex = -1;
static SceUID s_dvd_thread = -1;
static BOOL s_async_ready;
static BOOL s_async_stopping;
#ifndef MELEE_VITA_RELEASE
static u64 s_async_queue_us;
static u64 s_async_io_us;
static u64 s_async_bytes;
static u32 s_async_reads;
#endif

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

static s32 read_at_file(FILE* disc, void* output, u32 length, u32 offset)
{
    size_t amount;
    if (disc == NULL || output == NULL) return DVD_RESULT_FATAL_ERROR;
    if (fseek(disc, (long) offset, SEEK_SET) != 0)
        return DVD_RESULT_FATAL_ERROR;
    amount = fread(output, 1, length, disc);
    return amount == length ? (s32) amount : DVD_RESULT_FATAL_ERROR;
}

static s32 read_at(void* output, u32 length, u32 offset)
{
    if (offset & VITA_DVD_OVERLAY_BASE) {
        u32 slot = (offset >> VITA_DVD_OVERLAY_SHIFT) & 0x7Fu;
        u32 file_offset = offset & VITA_DVD_OVERLAY_OFFSET_MASK;
        VitaDvdOverlay* overlay;
        u32 available;
        if (slot >= VITA_DVD_OVERLAY_COUNT || output == NULL)
            return DVD_RESULT_FATAL_ERROR;
        overlay = &s_overlays[slot];
        if (overlay->sync_file == NULL ||
            (u64) file_offset + length >
                (u64) overlay->length + DVD_MIN_TRANSFER_SIZE - 1u)
            return DVD_RESULT_FATAL_ERROR;
        available = file_offset < overlay->length
                        ? overlay->length - file_offset
                        : 0;
        if (available > length) available = length;
        if (available != 0 &&
            read_at_file(overlay->sync_file, output, available, file_offset) < 0)
            return DVD_RESULT_FATAL_ERROR;
        memset((u8*) output + available, 0, length - available);
        return (s32) length;
    }
    return read_at_file(s_disc, output, length, offset);
}

static s32 read_at_async(void* output, u32 length, u32 offset)
{
    SceSSize amount;
    if (output == NULL) return DVD_RESULT_FATAL_ERROR;
    if (offset & VITA_DVD_OVERLAY_BASE) {
        u32 slot = (offset >> VITA_DVD_OVERLAY_SHIFT) & 0x7Fu;
        u32 file_offset = offset & VITA_DVD_OVERLAY_OFFSET_MASK;
        VitaDvdOverlay* overlay;
        u32 available;
        if (slot >= VITA_DVD_OVERLAY_COUNT) return DVD_RESULT_FATAL_ERROR;
        overlay = &s_overlays[slot];
        if (overlay->async_file < 0 ||
            (u64) file_offset + length >
                (u64) overlay->length + DVD_MIN_TRANSFER_SIZE - 1u)
            return DVD_RESULT_FATAL_ERROR;
        available = file_offset < overlay->length
                        ? overlay->length - file_offset
                        : 0;
        if (available > length) available = length;
        if (available != 0) {
            amount = sceIoPread(overlay->async_file, output, available,
                                (SceOff) file_offset);
            if (amount != (SceSSize) available)
                return DVD_RESULT_FATAL_ERROR;
        }
        memset((u8*) output + available, 0, length - available);
        return (s32) length;
    }
    if (s_async_disc < 0) return DVD_RESULT_FATAL_ERROR;
    amount = sceIoPread(s_async_disc, output, length, (SceOff) offset);
    return amount == (SceSSize) length ? (s32) amount
                                      : DVD_RESULT_FATAL_ERROR;
}

static void lock_queues(void)
{
    if (s_queue_mutex >= 0) sceKernelLockMutex(s_queue_mutex, 1, NULL);
}

static void unlock_queues(void)
{
    if (s_queue_mutex >= 0) sceKernelUnlockMutex(s_queue_mutex, 1);
}

static void queue_completion_locked(const VitaDvdRequest* request, s32 result)
{
    u32 slot;
    if (s_completion_count == DVD_COMPLETION_CAPACITY)
        OSPanic(__FILE__, __LINE__, "DVD completion queue overflow");
    slot = (s_completion_head + s_completion_count++) % DVD_COMPLETION_CAPACITY;
    s_completions[slot].block = request->block;
    s_completions[slot].file = request->file;
    s_completions[slot].block_callback = request->block_callback;
    s_completions[slot].file_callback = request->file_callback;
    s_completions[slot].result = result;
    s_completions[slot].transferred = result >= 0 ? (u32) result : 0;
}

static int dvd_reader_thread(SceSize args, void* argp)
{
    (void) args;
    (void) argp;
    for (;;) {
        VitaDvdRequest request;
        s32 result;
#ifndef MELEE_VITA_RELEASE
        u64 started;
#endif
        sceKernelWaitSema(s_request_sema, 1, NULL);
        lock_queues();
        if (s_request_count == 0) {
            BOOL stopping = s_async_stopping;
            unlock_queues();
            if (stopping) break;
            continue;
        }
        request = s_requests[s_request_head];
        s_request_head = (s_request_head + 1u) % DVD_REQUEST_CAPACITY;
        --s_request_count;
        unlock_queues();
#ifndef MELEE_VITA_RELEASE
        started = sceKernelGetProcessTimeWide();
#endif
        result = read_at_async(request.output, request.length, request.offset);
        if (result < 0) {
            melee_vita_log_info(
                "[DVD] async read failed off=%u len=%u",
                (unsigned) request.offset, (unsigned) request.length);
        }
        lock_queues();
#ifndef MELEE_VITA_RELEASE
        s_async_queue_us += started - request.queued_at;
        s_async_io_us += sceKernelGetProcessTimeWide() - started;
        s_async_bytes += request.length;
        ++s_async_reads;
#endif
        queue_completion_locked(&request, result);
        unlock_queues();
    }
    return 0;
}

static BOOL queue_request(const VitaDvdRequest* request)
{
    u32 slot;
    if (!s_async_ready || s_async_stopping) return FALSE;
    lock_queues();
    if (s_outstanding_count == DVD_REQUEST_CAPACITY) {
        unlock_queues();
        melee_vita_log_info("[DVD] async request queue full");
        return FALSE;
    }
    slot = (s_request_head + s_request_count++) % DVD_REQUEST_CAPACITY;
    s_requests[slot] = *request;
    ++s_outstanding_count;
    unlock_queues();
    sceKernelSignalSema(s_request_sema, 1);
    return TRUE;
}

void melee_vita_dvd_shutdown(void)
{
    if (!s_initialized) return;
    if (s_async_ready) {
        lock_queues();
        s_async_stopping = TRUE;
        unlock_queues();
        for (;;) {
            u32 outstanding;
            melee_vita_dvd_poll();
            lock_queues();
            outstanding = s_outstanding_count;
            unlock_queues();
            if (outstanding == 0) break;
            sceKernelDelayThread(1000);
        }
        sceKernelSignalSema(s_request_sema, 1);
        sceKernelWaitThreadEnd(s_dvd_thread, NULL, NULL);
    }
    if (s_dvd_thread >= 0) sceKernelDeleteThread(s_dvd_thread);
    if (s_queue_mutex >= 0) sceKernelDeleteMutex(s_queue_mutex);
    if (s_request_sema >= 0) sceKernelDeleteSema(s_request_sema);
    if (s_async_disc >= 0) sceIoClose(s_async_disc);
    if (s_disc != NULL) fclose(s_disc);
    {
        u32 i;
        for (i = 0; i < VITA_DVD_OVERLAY_COUNT; i++) {
            if (s_overlays[i].sync_file != NULL) fclose(s_overlays[i].sync_file);
            if (s_overlays[i].async_file >= 0)
                sceIoClose(s_overlays[i].async_file);
            s_overlays[i].sync_file = NULL;
            s_overlays[i].async_file = -1;
            s_overlays[i].length = 0;
        }
    }
    free(s_fst);
    s_dvd_thread = s_queue_mutex = s_request_sema = -1;
    s_async_disc = -1;
    s_disc = NULL;
    s_fst = NULL;
    s_fst_size = s_fst_count = 0;
    s_async_ready = FALSE;
    s_initialized = FALSE;
}

void melee_vita_dvd_poll(void)
{
    for (;;) {
        VitaDvdCompletion completion;
        if (s_async_ready) lock_queues();
        if (s_completion_count == 0) {
            if (s_async_ready) unlock_queues();
            break;
        }
        completion = s_completions[s_completion_head];
        s_completion_head = (s_completion_head + 1u) % DVD_COMPLETION_CAPACITY;
        --s_completion_count;
        if (s_async_ready) {
            --s_outstanding_count;
            unlock_queues();
        }
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

void melee_vita_dvd_log_stats(const char* phase)
{
#ifndef MELEE_VITA_RELEASE
    u64 queue_us;
    u64 io_us;
    u64 bytes;
    u32 reads;
    if (!s_async_ready) return;
    lock_queues();
    queue_us = s_async_queue_us;
    io_us = s_async_io_us;
    bytes = s_async_bytes;
    reads = s_async_reads;
    s_async_queue_us = 0;
    s_async_io_us = 0;
    s_async_bytes = 0;
    s_async_reads = 0;
    unlock_queues();
    melee_vita_log_info(
        "[DVDPERF] %s reads=%u bytes=%llu queue=%lluus io=%lluus",
        phase, reads, (unsigned long long) bytes,
        (unsigned long long) queue_us, (unsigned long long) io_us);
#else
    (void) phase;
#endif
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
    s_async_stopping = FALSE;
    s_async_disc = sceIoOpen(MELEE_VITA_DISC_PATH, SCE_O_RDONLY, 0);
    s_request_sema = sceKernelCreateSema(
        "melee_dvd_work", 0, 0, DVD_REQUEST_CAPACITY, NULL);
    s_queue_mutex = sceKernelCreateMutex("melee_dvd_queue", 0, 1, NULL);
    s_dvd_thread = sceKernelCreateThread(
        "melee_dvd", dvd_reader_thread, DVD_THREAD_PRIORITY,
        DVD_THREAD_STACK, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (s_async_disc >= 0 && s_request_sema >= 0 && s_queue_mutex >= 0 &&
        s_dvd_thread >= 0 && sceKernelStartThread(s_dvd_thread, 0, NULL) >= 0)
    {
        s_async_ready = TRUE;
        melee_vita_log_info("[DVD] asynchronous reader started");
    } else {
        melee_vita_log_info(
            "[DVD] asynchronous reader unavailable; disc=%d work=%d "
            "mutex=%d thread=%d",
            s_async_disc, s_request_sema, s_queue_mutex, s_dvd_thread);
        if (s_dvd_thread >= 0) sceKernelDeleteThread(s_dvd_thread);
        if (s_queue_mutex >= 0) sceKernelDeleteMutex(s_queue_mutex);
        if (s_request_sema >= 0) sceKernelDeleteSema(s_request_sema);
        if (s_async_disc >= 0) sceIoClose(s_async_disc);
        s_dvd_thread = s_queue_mutex = s_request_sema = -1;
        s_async_disc = -1;
    }
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

static void install_costume_if_missing(const char* name)
{
    char source_path[96];
    char target_path[96];
    char temp_path[100];
    FILE* source;
    FILE* target;
    u8 buffer[8192];
    size_t count;
    BOOL ok = TRUE;

    snprintf(target_path, sizeof(target_path), "%s%s",
             VITA_DVD_COSTUME_DIR, name);
    target = fopen(target_path, "rb");
    if (target != NULL) {
        fclose(target);
        return;
    }
    snprintf(source_path, sizeof(source_path), "app0:/costumes/%s", name);
    source = fopen(source_path, "rb");
    if (source == NULL) return;
    sceIoMkdir("ux0:data/melee", 0777);
    sceIoMkdir("ux0:data/melee/costumes", 0777);
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", target_path);
    target = fopen(temp_path, "wb");
    if (target == NULL) {
        fclose(source);
        return;
    }
    while ((count = fread(buffer, 1, sizeof(buffer), source)) != 0) {
        if (fwrite(buffer, 1, count, target) != count) {
            ok = FALSE;
            break;
        }
    }
    if (ferror(source)) ok = FALSE;
    if (fclose(target) != 0) ok = FALSE;
    fclose(source);
    if (ok) {
        if (sceIoRename(temp_path, target_path) >= 0) return;
    }
    sceIoRemove(temp_path);
}

static s32 find_costume_overlay(const char* path)
{
    const char* basename = strrchr(path, '/');
    u32 i;
    if (basename != NULL) {
        basename++;
    } else {
        basename = path;
    }
    for (i = 0; i < VITA_DVD_OVERLAY_COUNT; i++) {
        VitaDvdOverlay* overlay = &s_overlays[i];
        char full_path[96];
        long size;
        if (!name_equal(overlay->name, basename, strlen(basename))) continue;
        if (overlay->sync_file != NULL) return (s32) (s_fst_count + i);
        snprintf(full_path, sizeof(full_path), "%s%s",
                 VITA_DVD_COSTUME_DIR, overlay->name);
        install_costume_if_missing(overlay->name);
        overlay->sync_file = fopen(full_path, "rb");
        if (overlay->sync_file == NULL) return -1;
        if (fseek(overlay->sync_file, 0, SEEK_END) != 0 ||
            (size = ftell(overlay->sync_file)) <= 0 ||
            size > (long) VITA_DVD_OVERLAY_OFFSET_MASK)
        {
            fclose(overlay->sync_file);
            overlay->sync_file = NULL;
            return -1;
        }
        rewind(overlay->sync_file);
        overlay->async_file = sceIoOpen(full_path, SCE_O_RDONLY, 0);
        if (overlay->async_file < 0) {
            fclose(overlay->sync_file);
            overlay->sync_file = NULL;
            return -1;
        }
        overlay->length = (u32) size;
        return (s32) (s_fst_count + i);
    }
    return -1;
}

s32 DVDConvertPathToEntrynum(const char* path)
{
    s32 current;
    const char* cursor;
    if (!s_initialized || path == NULL) return -1;
    {
        s32 overlay = find_costume_overlay(path);
        if (overlay >= 0) return overlay;
    }
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
    if (!s_initialized || info == NULL || entry < 0) return FALSE;
    if ((u32) entry >= s_fst_count &&
        (u32) entry < s_fst_count + VITA_DVD_OVERLAY_COUNT)
    {
        u32 slot = (u32) entry - s_fst_count;
        if (s_overlays[slot].sync_file == NULL) return FALSE;
        memset(info, 0, sizeof(*info));
        info->startAddr = VITA_DVD_OVERLAY_BASE |
                          (slot << VITA_DVD_OVERLAY_SHIFT);
        info->length = s_overlays[slot].length;
        info->cb.state = DVD_STATE_END;
        return TRUE;
    }
    if ((u32) entry >= s_fst_count || entry_is_dir((u32) entry)) return FALSE;
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
    VitaDvdRequest request;
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
    request.block = &info->cb;
    request.file = info;
    request.block_callback = NULL;
    request.file_callback = callback;
    request.output = output;
    request.length = (u32) length;
    request.offset = info->startAddr + (u32) offset;
    request.queued_at = sceKernelGetProcessTimeWide();
    if (queue_request(&request)) return TRUE;
    if (!s_async_ready) {
        result = read_at(output, (u32) length, request.offset);
        queue_completion_locked(&request, result);
        return TRUE;
    }
    info->cb.state = DVD_STATE_FATAL_ERROR;
    return FALSE;
}

int DVDReadAbsAsyncPrio(DVDCommandBlock* block, void* output, s32 length,
                        s32 offset, DVDCBCallback callback, s32 priority)
{
    VitaDvdRequest request;
    s32 result;
    (void) priority;
    if (block == NULL || length < 0 || offset < 0) return FALSE;
    block->command = DVD_COMMAND_READ;
    block->state = DVD_STATE_BUSY;
    block->addr = output;
    block->length = (u32) length;
    block->offset = (u32) offset;
    block->callback = callback;
    request.block = block;
    request.file = NULL;
    request.block_callback = callback;
    request.file_callback = NULL;
    request.output = output;
    request.length = (u32) length;
    request.offset = (u32) offset;
    request.queued_at = sceKernelGetProcessTimeWide();
    if (queue_request(&request)) return TRUE;
    if (!s_async_ready) {
        result = read_at(output, (u32) length, request.offset);
        queue_completion_locked(&request, result);
        return TRUE;
    }
    block->state = DVD_STATE_FATAL_ERROR;
    return FALSE;
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
