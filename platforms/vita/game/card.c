/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Minimal persistent GameCube memory-card backend for the Vita port. */
#include "vita_platform.h"

#include "../vita_log.h"

#include <dolphin/card.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define VITA_CARD_BYTES (8u * 1024u * 1024u)
#define VITA_CARD_SECTOR 8192
#define VITA_CARD_INDEX_VERSION 1u
#define VITA_CARD_CALLBACKS 16

typedef struct VitaCardIndex {
    char magic[8];
    u32 version;
    u8 active[CARD_MAX_FILE];
    CARDStat stat[CARD_MAX_FILE];
} VitaCardIndex;

typedef struct VitaCardCompletion {
    CARDCallback callback;
    s32 channel;
    s32 result;
} VitaCardCompletion;

static VitaCardIndex s_index;
static VitaCardCompletion s_completions[VITA_CARD_CALLBACKS];
static u32 s_completion_read;
static u32 s_completion_write;
static s32 s_result[2] = { CARD_RESULT_READY, CARD_RESULT_NOCARD };
static s32 s_xferred[2];
static u8 s_dirty[CARD_MAX_FILE];
static BOOL s_initialized;
static BOOL s_mounted;
static CARDCallback s_detach_callback;
static char s_game[4] = { 'G', 'A', 'L', 'E' };
static char s_maker[2] = { '0', '1' };

static const char* s_save_dir = "ux0:data/melee/save";
static const char* s_index_path = "ux0:data/melee/save/card.index";
static const char* s_import_dir = "ux0:data/melee/save/import";
static const char* s_export_dir = "ux0:data/melee/save/export";

static void data_path(char* path, size_t size, s32 file_no)
{
    snprintf(path, size, "%s/slot-%03ld.dat", s_save_dir, (long) file_no);
}

static s32 channel_result(s32 channel)
{
    if (channel == 0) return CARD_RESULT_READY;
    if (channel == 1) return CARD_RESULT_NOCARD;
    return CARD_RESULT_FATAL_ERROR;
}

static void set_result(s32 channel, s32 result)
{
    if (channel >= 0 && channel < 2) s_result[channel] = result;
}

static void queue_completion(CARDCallback callback, s32 channel, s32 result)
{
    u32 next;
    if (callback == NULL) return;
    next = (s_completion_write + 1u) % VITA_CARD_CALLBACKS;
    if (next == s_completion_read) {
        /* A full queue is preferable to an inline callback: replace the oldest
         * completion while preserving the GameCube's asynchronous ordering. */
        s_completion_read = (s_completion_read + 1u) % VITA_CARD_CALLBACKS;
    }
    s_completions[s_completion_write].callback = callback;
    s_completions[s_completion_write].channel = channel;
    s_completions[s_completion_write].result = result;
    s_completion_write = next;
}

void melee_vita_card_poll(void)
{
    while (s_completion_read != s_completion_write) {
        VitaCardCompletion completion = s_completions[s_completion_read];
        s_completion_read = (s_completion_read + 1u) % VITA_CARD_CALLBACKS;
        completion.callback(completion.channel, completion.result);
    }
}

static void reset_index(void)
{
    memset(&s_index, 0, sizeof(s_index));
    memcpy(s_index.magic, "MVMCARD", 7);
    s_index.version = VITA_CARD_INDEX_VERSION;
}

static BOOL save_index(void)
{
    FILE* file = fopen(s_index_path, "wb");
    BOOL ok;
    if (file == NULL) return FALSE;
    ok = fwrite(&s_index, 1, sizeof(s_index), file) == sizeof(s_index);
    if (fclose(file) != 0) ok = FALSE;
    return ok;
}

static void load_index(void)
{
    FILE* file;
    mkdir("ux0:data/melee", 0777);
    mkdir(s_save_dir, 0777);
    file = fopen(s_index_path, "rb");
    if (file == NULL || fread(&s_index, 1, sizeof(s_index), file) != sizeof(s_index) ||
        memcmp(s_index.magic, "MVMCARD", 7) != 0 ||
        s_index.version != VITA_CARD_INDEX_VERSION) {
        if (file != NULL) fclose(file);
        reset_index();
        save_index();
        return;
    }
    fclose(file);
}

static s32 find_name(const char* name)
{
    s32 i;
    if (name == NULL) return -1;
    for (i = 0; i < CARD_MAX_FILE; ++i) {
        if (s_index.active[i] &&
            strncmp(s_index.stat[i].fileName, name, CARD_FILENAME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

static s32 find_free(void)
{
    s32 i;
    for (i = 0; i < CARD_MAX_FILE; ++i) {
        if (!s_index.active[i]) return i;
    }
    return -1;
}

static s32 used_bytes(void)
{
    s32 i;
    u32 total = 0;
    for (i = 0; i < CARD_MAX_FILE; ++i) {
        if (s_index.active[i]) total += s_index.stat[i].length;
    }
    return total > VITA_CARD_BYTES ? (s32) VITA_CARD_BYTES : (s32) total;
}

/* ---- Dolphin .gci interchange -------------------------------------------
 *
 * A .gci file is the card's 64-byte directory entry for one save followed by
 * its data blocks, which is exactly what this backend keeps per file.  Files
 * dropped in save/import are copied onto the card at boot; every save the
 * game closes is written back out to save/export, so a Vita save can be taken
 * to Dolphin and back.  Header fields are big endian, as on the GameCube. */

#define GCI_HEADER_SIZE 0x40u
#define GCI_BLOCK_SIZE 8192u

static u16 read_be16(const u8* p) { return (u16) ((u16) p[0] << 8 | p[1]); }
static u32 read_be32(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}
static void write_be16(u8* p, u16 v) { p[0] = (u8) (v >> 8); p[1] = (u8) v; }
static void write_be32(u8* p, u32 v)
{
    p[0] = (u8) (v >> 24); p[1] = (u8) (v >> 16); p[2] = (u8) (v >> 8); p[3] = (u8) v;
}

static BOOL copy_stream(FILE* in, FILE* out, u32 bytes)
{
    static u8 buffer[4096];
    while (bytes != 0) {
        const u32 chunk = bytes < sizeof(buffer) ? bytes : (u32) sizeof(buffer);
        if (fread(buffer, 1, chunk, in) != chunk) return FALSE;
        if (fwrite(buffer, 1, chunk, out) != chunk) return FALSE;
        bytes -= chunk;
    }
    return TRUE;
}

static void export_gci(s32 file_no)
{
    const CARDStat* stat;
    char source[64], target[128];
    u8 header[GCI_HEADER_SIZE];
    FILE* in;
    FILE* out;
    BOOL ok;
    if (file_no < 0 || file_no >= CARD_MAX_FILE || !s_index.active[file_no]) return;
    stat = &s_index.stat[file_no];
    if (stat->length == 0 || (stat->length % GCI_BLOCK_SIZE) != 0) return;
    mkdir(s_export_dir, 0777);
    snprintf(target, sizeof(target), "%s/%.32s.gci", s_export_dir, stat->fileName);
    data_path(source, sizeof(source), file_no);
    in = fopen(source, "rb");
    if (in == NULL) return;
    out = fopen(target, "wb");
    if (out == NULL) { fclose(in); return; }
    memset(header, 0, sizeof(header));
    memcpy(header + 0x00, stat->gameName, 4);
    memcpy(header + 0x04, stat->company, 2);
    header[0x06] = 0xff;
    header[0x07] = stat->bannerFormat;
    memcpy(header + 0x08, stat->fileName, CARD_FILENAME_MAX);
    write_be32(header + 0x28, stat->time);
    write_be32(header + 0x2c, stat->iconAddr);
    write_be16(header + 0x30, stat->iconFormat);
    write_be16(header + 0x32, stat->iconSpeed);
    header[0x34] = CARD_ATTR_PUBLIC;
    header[0x35] = 0xff; /* copy count: unrestricted, as Dolphin writes */
    write_be16(header + 0x36, 0xffff);
    write_be16(header + 0x38, (u16) (stat->length / GCI_BLOCK_SIZE));
    write_be16(header + 0x3a, 0xffff);
    write_be32(header + 0x3c, stat->commentAddr);
    ok = fwrite(header, 1, sizeof(header), out) == sizeof(header) &&
         copy_stream(in, out, stat->length);
    fclose(in);
    if (fclose(out) != 0) ok = FALSE;
    if (!ok) {
        remove(target);
        melee_vita_log_info("[CARD] export failed for %.32s", stat->fileName);
    } else {
        melee_vita_log_info("[CARD] exported %.32s (%u bytes)", stat->fileName,
                            (unsigned) stat->length);
    }
}

static void import_gci(const char* directory, const char* name)
{
    char source[256], target[64], done[288];
    u8 header[GCI_HEADER_SIZE];
    char file_name[CARD_FILENAME_MAX + 1];
    CARDStat* stat;
    FILE* in;
    FILE* out;
    u32 length;
    s32 file_no;
    snprintf(source, sizeof(source), "%s/%s", directory, name);
    in = fopen(source, "rb");
    if (in == NULL) return;
    if (fread(header, 1, sizeof(header), in) != sizeof(header)) {
        fclose(in);
        melee_vita_log_info("[CARD] %s is too small to be a .gci", name);
        return;
    }
    memcpy(file_name, header + 0x08, CARD_FILENAME_MAX);
    file_name[CARD_FILENAME_MAX] = '\0';
    length = (u32) read_be16(header + 0x38) * GCI_BLOCK_SIZE;
    if (length == 0 || length > VITA_CARD_BYTES) {
        fclose(in);
        melee_vita_log_info("[CARD] %s has an impossible size", name);
        return;
    }
    if (find_name(file_name) >= 0) {
        fclose(in);
        melee_vita_log_info("[CARD] %s already on the card, left alone", file_name);
        return;
    }
    file_no = find_free();
    if (file_no < 0 || length > (u32) ((s32) VITA_CARD_BYTES - used_bytes())) {
        fclose(in);
        melee_vita_log_info("[CARD] no room to import %s", file_name);
        return;
    }
    data_path(target, sizeof(target), file_no);
    out = fopen(target, "wb");
    if (out == NULL) { fclose(in); return; }
    if (!copy_stream(in, out, length)) {
        fclose(in);
        fclose(out);
        remove(target);
        melee_vita_log_info("[CARD] %s is shorter than its header claims", name);
        return;
    }
    fclose(in);
    if (fclose(out) != 0) {
        remove(target);
        return;
    }
    stat = &s_index.stat[file_no];
    memset(stat, 0, sizeof(*stat));
    memcpy(stat->fileName, header + 0x08, CARD_FILENAME_MAX);
    stat->length = length;
    stat->time = read_be32(header + 0x28);
    memcpy(stat->gameName, header + 0x00, 4);
    memcpy(stat->company, header + 0x04, 2);
    stat->bannerFormat = header[0x07];
    stat->iconAddr = read_be32(header + 0x2c);
    stat->iconFormat = read_be16(header + 0x30);
    stat->iconSpeed = read_be16(header + 0x32);
    stat->commentAddr = read_be32(header + 0x3c);
    s_index.active[file_no] = TRUE;
    if (!save_index()) {
        s_index.active[file_no] = FALSE;
        remove(target);
        return;
    }
    /* Renamed rather than deleted: the import is repeatable if something in
     * the save turns out to be wrong, and nothing of the user's is thrown
     * away. */
    snprintf(done, sizeof(done), "%s.imported", source);
    remove(done);
    rename(source, done);
    melee_vita_log_info("[CARD] imported %s (%u bytes)", file_name, (unsigned) length);
}

static BOOL has_gci_suffix(const char* name)
{
    const size_t length = strlen(name);
    return length > 4u && strcasecmp(name + length - 4u, ".gci") == 0;
}

static void import_gci_directory(const char* directory)
{
    DIR* dir = opendir(directory);
    struct dirent* entry;
    if (dir == NULL) return;
    while ((entry = readdir(dir)) != NULL)
        if (has_gci_suffix(entry->d_name)) import_gci(directory, entry->d_name);
    closedir(dir);
}

static void import_gci_files(void)
{
    mkdir(s_import_dir, 0777);
    import_gci_directory(s_import_dir);
    import_gci_directory(s_save_dir);
}

void CARDInit(const char* game, const char* maker)
{
    if (game != NULL) memcpy(s_game, game, sizeof(s_game));
    if (maker != NULL) memcpy(s_maker, maker, sizeof(s_maker));
    if (!s_initialized) {
        load_index();
        import_gci_files();
    }
    s_initialized = TRUE;
}

void CARDSetGameAndMaker(s32 channel, const char* game, const char* maker)
{
    if (channel != 0) return;
    if (game != NULL) memcpy(s_game, game, sizeof(s_game));
    if (maker != NULL) memcpy(s_maker, maker, sizeof(s_maker));
}

s32 CARDGetResultCode(s32 channel)
{
    return channel >= 0 && channel < 2 ? s_result[channel] : CARD_RESULT_FATAL_ERROR;
}

int CARDProbe(s32 channel)
{
    return channel == 0 ? TRUE : FALSE;
}

s32 CARDProbeEx(s32 channel, s32* mem_size, s32* sector_size)
{
    s32 result = channel_result(channel);
    if (result == CARD_RESULT_READY) {
        if (mem_size != NULL) *mem_size = (s32) (VITA_CARD_BYTES / (1024u * 1024u));
        if (sector_size != NULL) *sector_size = VITA_CARD_SECTOR;
    }
    set_result(channel, result);
    return result;
}

s32 CARDMount(s32 channel, void* work_area, CARDCallback detach_callback)
{
    s32 result = channel_result(channel);
    (void) work_area;
    if (result == CARD_RESULT_READY) {
        s_mounted = TRUE;
        s_detach_callback = detach_callback;
    }
    set_result(channel, result);
    return result;
}

s32 CARDMountAsync(s32 channel, void* work_area, CARDCallback detach_callback,
                   CARDCallback attach_callback)
{
    s32 result = CARDMount(channel, work_area, detach_callback);
    queue_completion(attach_callback, channel, result);
    return result;
}

s32 CARDUnmount(s32 channel)
{
    s32 result = channel_result(channel);
    if (result == CARD_RESULT_READY) {
        s_mounted = FALSE;
        s_detach_callback = NULL;
    }
    set_result(channel, result);
    return result;
}

s32 CARDCheck(s32 channel)
{
    s32 result = channel_result(channel);
    set_result(channel, result);
    return result;
}

s32 CARDCheckAsync(s32 channel, CARDCallback callback)
{
    s32 result = CARDCheck(channel);
    queue_completion(callback, channel, result);
    return result;
}

s32 CARDFreeBlocks(s32 channel, s32* bytes_unused, s32* files_unused)
{
    s32 i;
    s32 files = 0;
    s32 result = channel_result(channel);
    if (result != CARD_RESULT_READY) return result;
    for (i = 0; i < CARD_MAX_FILE; ++i) files += !s_index.active[i];
    if (bytes_unused != NULL) *bytes_unused = (s32) VITA_CARD_BYTES - used_bytes();
    if (files_unused != NULL) *files_unused = files;
    return CARD_RESULT_READY;
}

s32 CARDCreate(s32 channel, const char* name, u32 size, CARDFileInfo* info)
{
    s32 file_no;
    char path[64];
    FILE* file;
    s32 result = channel_result(channel);
    if (result != CARD_RESULT_READY) return result;
    if (name == NULL || info == NULL || strnlen(name, CARD_FILENAME_MAX + 1u) > CARD_FILENAME_MAX)
        return CARD_RESULT_NAMETOOLONG;
    if (find_name(name) >= 0) return CARD_RESULT_EXIST;
    if (size > (u32) ((s32) VITA_CARD_BYTES - used_bytes())) return CARD_RESULT_INSSPACE;
    file_no = find_free();
    if (file_no < 0) return CARD_RESULT_LIMIT;
    data_path(path, sizeof(path), file_no);
    file = fopen(path, "wb");
    if (file == NULL) return CARD_RESULT_IOERROR;
    if (size != 0 && (fseek(file, (long) size - 1L, SEEK_SET) != 0 || fputc(0, file) == EOF)) {
        fclose(file);
        remove(path);
        return CARD_RESULT_IOERROR;
    }
    if (fclose(file) != 0) {
        remove(path);
        return CARD_RESULT_IOERROR;
    }
    memset(&s_index.stat[file_no], 0, sizeof(CARDStat));
    strncpy(s_index.stat[file_no].fileName, name, CARD_FILENAME_MAX);
    s_index.stat[file_no].length = size;
    memcpy(s_index.stat[file_no].gameName, s_game, sizeof(s_game));
    memcpy(s_index.stat[file_no].company, s_maker, sizeof(s_maker));
    s_index.stat[file_no].iconAddr = 0xFFFFFFFFu;
    s_index.stat[file_no].commentAddr = 0xFFFFFFFFu;
    s_index.active[file_no] = TRUE;
    if (!save_index()) {
        s_index.active[file_no] = FALSE;
        remove(path);
        return CARD_RESULT_IOERROR;
    }
    info->chan = channel;
    info->fileNo = file_no;
    info->offset = 0;
    info->length = (s32) size;
    info->iBlock = 0;
    return CARD_RESULT_READY;
}

s32 CARDCreateAsync(s32 channel, const char* name, u32 size, CARDFileInfo* info,
                    CARDCallback callback)
{
    s32 result = CARDCreate(channel, name, size, info);
    set_result(channel, result);
    queue_completion(callback, channel, result);
    return result;
}

s32 CARDFastOpen(s32 channel, s32 file_no, CARDFileInfo* info)
{
    s32 result = channel_result(channel);
    if (result != CARD_RESULT_READY) return result;
    if (info == NULL || file_no < 0 || file_no >= CARD_MAX_FILE || !s_index.active[file_no])
        return CARD_RESULT_NOFILE;
    info->chan = channel;
    info->fileNo = file_no;
    info->offset = 0;
    info->length = (s32) s_index.stat[file_no].length;
    info->iBlock = 0;
    return CARD_RESULT_READY;
}

s32 CARDOpen(s32 channel, const char* name, CARDFileInfo* info)
{
    s32 file_no = find_name(name);
    if (channel_result(channel) != CARD_RESULT_READY) return channel_result(channel);
    return file_no < 0 ? CARD_RESULT_NOFILE : CARDFastOpen(channel, file_no, info);
}

s32 CARDClose(CARDFileInfo* info)
{
    if (info == NULL) return CARD_RESULT_FATAL_ERROR;
    /* A save the game has finished writing is mirrored out as a .gci. */
    if (info->fileNo >= 0 && info->fileNo < CARD_MAX_FILE && s_dirty[info->fileNo]) {
        s_dirty[info->fileNo] = 0u;
        export_gci(info->fileNo);
    }
    info->chan = -1;
    return CARD_RESULT_READY;
}

static s32 transfer(const CARDFileInfo* info, void* buffer, s32 length, s32 offset,
                    BOOL writing)
{
    char path[64];
    FILE* file;
    size_t transferred;
    if (info == NULL || buffer == NULL || info->chan != 0 || info->fileNo < 0 ||
        info->fileNo >= CARD_MAX_FILE || !s_index.active[info->fileNo]) return CARD_RESULT_NOFILE;
    if (length < 0 || offset < 0 || (u32) offset + (u32) length > s_index.stat[info->fileNo].length)
        return CARD_RESULT_IOERROR;
    data_path(path, sizeof(path), info->fileNo);
    file = fopen(path, writing ? "r+b" : "rb");
    if (file == NULL || fseek(file, offset, SEEK_SET) != 0) {
        if (file != NULL) fclose(file);
        return CARD_RESULT_IOERROR;
    }
    transferred = writing ? fwrite(buffer, 1, (size_t) length, file)
                          : fread(buffer, 1, (size_t) length, file);
    if (fclose(file) != 0 || transferred != (size_t) length) return CARD_RESULT_IOERROR;
    if (writing) s_dirty[info->fileNo] = 1u;
    s_xferred[0] = length;
    return CARD_RESULT_READY;
}

s32 CARDRead(const CARDFileInfo* info, void* address, s32 length, s32 offset)
{
    return transfer(info, address, length, offset, FALSE);
}

s32 CARDReadAsync(const CARDFileInfo* info, void* address, s32 length, s32 offset,
                  CARDCallback callback)
{
    s32 result = CARDRead(info, address, length, offset);
    s32 channel = info != NULL ? info->chan : 0;
    set_result(channel, result);
    queue_completion(callback, channel, result);
    return result;
}

s32 CARDWrite(const CARDFileInfo* info, const void* address, s32 length, s32 offset)
{
    return transfer(info, (void*) address, length, offset, TRUE);
}

s32 CARDWriteAsync(const CARDFileInfo* info, const void* address, s32 length, s32 offset,
                   CARDCallback callback)
{
    s32 result = CARDWrite(info, address, length, offset);
    s32 channel = info != NULL ? info->chan : 0;
    set_result(channel, result);
    queue_completion(callback, channel, result);
    return result;
}

s32 CARDGetXferredBytes(s32 channel)
{
    return channel >= 0 && channel < 2 ? s_xferred[channel] : 0;
}

s32 CARDGetStatus(s32 channel, s32 file_no, CARDStat* stat)
{
    s32 result = channel_result(channel);
    if (result != CARD_RESULT_READY) return result;
    if (stat == NULL || file_no < 0 || file_no >= CARD_MAX_FILE || !s_index.active[file_no])
        return CARD_RESULT_NOFILE;
    *stat = s_index.stat[file_no];
    return CARD_RESULT_READY;
}

s32 CARDSetStatus(s32 channel, s32 file_no, const CARDStat* stat)
{
    char name[CARD_FILENAME_MAX];
    u32 length;
    s32 result = channel_result(channel);
    if (result != CARD_RESULT_READY) return result;
    if (stat == NULL || file_no < 0 || file_no >= CARD_MAX_FILE || !s_index.active[file_no])
        return CARD_RESULT_NOFILE;
    memcpy(name, s_index.stat[file_no].fileName, sizeof(name));
    length = s_index.stat[file_no].length;
    s_index.stat[file_no] = *stat;
    memcpy(s_index.stat[file_no].fileName, name, sizeof(name));
    s_index.stat[file_no].length = length;
    return save_index() ? CARD_RESULT_READY : CARD_RESULT_IOERROR;
}

s32 CARDSetStatusAsync(s32 channel, s32 file_no, const CARDStat* stat,
                       CARDCallback callback)
{
    s32 result = CARDSetStatus(channel, file_no, stat);
    set_result(channel, result);
    queue_completion(callback, channel, result);
    return result;
}

s32 CARDDelete(s32 channel, const char* name)
{
    s32 file_no;
    char path[64];
    s32 result = channel_result(channel);
    if (result != CARD_RESULT_READY) return result;
    file_no = find_name(name);
    if (file_no < 0) return CARD_RESULT_NOFILE;
    data_path(path, sizeof(path), file_no);
    if (remove(path) != 0 && errno != ENOENT) return CARD_RESULT_IOERROR;
    s_index.active[file_no] = FALSE;
    memset(&s_index.stat[file_no], 0, sizeof(CARDStat));
    return save_index() ? CARD_RESULT_READY : CARD_RESULT_IOERROR;
}

s32 CARDDeleteAsync(s32 channel, const char* name, CARDCallback callback)
{
    s32 result = CARDDelete(channel, name);
    set_result(channel, result);
    queue_completion(callback, channel, result);
    return result;
}

s32 CARDRename(s32 channel, const char* old_name, const char* new_name)
{
    s32 file_no;
    s32 result = channel_result(channel);
    if (result != CARD_RESULT_READY) return result;
    if (new_name == NULL || strnlen(new_name, CARD_FILENAME_MAX + 1u) > CARD_FILENAME_MAX)
        return CARD_RESULT_NAMETOOLONG;
    if (find_name(new_name) >= 0) return CARD_RESULT_EXIST;
    file_no = find_name(old_name);
    if (file_no < 0) return CARD_RESULT_NOFILE;
    memset(s_index.stat[file_no].fileName, 0, CARD_FILENAME_MAX);
    strncpy(s_index.stat[file_no].fileName, new_name, CARD_FILENAME_MAX);
    return save_index() ? CARD_RESULT_READY : CARD_RESULT_IOERROR;
}

s32 CARDRenameAsync(s32 channel, const char* old_name, const char* new_name,
                    CARDCallback callback)
{
    s32 result = CARDRename(channel, old_name, new_name);
    set_result(channel, result);
    queue_completion(callback, channel, result);
    return result;
}

s32 CARDFormat(s32 channel)
{
    s32 i;
    char path[64];
    s32 result = channel_result(channel);
    if (result != CARD_RESULT_READY) return result;
    for (i = 0; i < CARD_MAX_FILE; ++i) {
        if (s_index.active[i]) {
            data_path(path, sizeof(path), i);
            remove(path);
        }
    }
    reset_index();
    return save_index() ? CARD_RESULT_READY : CARD_RESULT_IOERROR;
}

s32 CARDFormatAsync(s32 channel, CARDCallback callback)
{
    s32 result = CARDFormat(channel);
    set_result(channel, result);
    queue_completion(callback, channel, result);
    return result;
}
