/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The subset of the Vita system API that the shared Vita platform layer
 * (platforms/vita/game) uses outside its renderer, implemented for PS5 on
 * POSIX threads, files and the PS5 pad/audio services.  Signatures and
 * constants follow vitasdk so those sources build unchanged.
 */
#ifndef MELEE_PS5_PSP2_SHIM_H
#define MELEE_PS5_PSP2_SHIM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Every Vita entry point is renamed: several (sceKernelWaitSema,
 * sceAudioOutOutput, ...) also exist in PS5 system libraries with other
 * signatures, and the system ones must stay reachable. */
#define sceAudioOutOpenPort melee_psp2_sceAudioOutOpenPort
#define sceAudioOutOutput melee_psp2_sceAudioOutOutput
#define sceAudioOutReleasePort melee_psp2_sceAudioOutReleasePort
#define sceCtrlPeekBufferPositive melee_psp2_sceCtrlPeekBufferPositive
#define sceCtrlPeekBufferPositive2 melee_psp2_sceCtrlPeekBufferPositive2
#define sceCtrlPeekBufferPositiveExt2 melee_psp2_sceCtrlPeekBufferPositiveExt2
#define sceCtrlReadBufferPositive melee_psp2_sceCtrlReadBufferPositive
#define sceCtrlSetSamplingMode melee_psp2_sceCtrlSetSamplingMode
#define sceCtrlSetSamplingModeExt melee_psp2_sceCtrlSetSamplingModeExt
#define sceDisplayWaitVblankStart melee_psp2_sceDisplayWaitVblankStart
#define sceIoClose melee_psp2_sceIoClose
#define sceIoGetstat melee_psp2_sceIoGetstat
#define sceIoLseek melee_psp2_sceIoLseek
#define sceIoMkdir melee_psp2_sceIoMkdir
#define sceIoOpen melee_psp2_sceIoOpen
#define sceIoPread melee_psp2_sceIoPread
#define sceIoPwrite melee_psp2_sceIoPwrite
#define sceIoRead melee_psp2_sceIoRead
#define sceIoRemove melee_psp2_sceIoRemove
#define sceIoRename melee_psp2_sceIoRename
#define sceIoRmdir melee_psp2_sceIoRmdir
#define sceIoWrite melee_psp2_sceIoWrite
#define sceKernelAllocMemBlock melee_psp2_sceKernelAllocMemBlock
#define sceKernelChangeThreadCpuAffinityMask melee_psp2_sceKernelChangeThreadCpuAffinityMask
#define sceKernelChangeThreadPriority melee_psp2_sceKernelChangeThreadPriority
#define sceKernelCreateLwMutex melee_psp2_sceKernelCreateLwMutex
#define sceKernelCreateMutex melee_psp2_sceKernelCreateMutex
#define sceKernelCreateSema melee_psp2_sceKernelCreateSema
#define sceKernelCreateThread melee_psp2_sceKernelCreateThread
#define sceKernelDelayThread melee_psp2_sceKernelDelayThread
#define sceKernelDelayThreadCB melee_psp2_sceKernelDelayThreadCB
#define sceKernelDeleteLwMutex melee_psp2_sceKernelDeleteLwMutex
#define sceKernelDeleteMutex melee_psp2_sceKernelDeleteMutex
#define sceKernelDeleteSema melee_psp2_sceKernelDeleteSema
#define sceKernelDeleteThread melee_psp2_sceKernelDeleteThread
#define sceKernelExitDeleteThread melee_psp2_sceKernelExitDeleteThread
#define sceKernelExitProcess melee_psp2_sceKernelExitProcess
#define sceKernelExitThread melee_psp2_sceKernelExitThread
#define sceKernelFreeMemBlock melee_psp2_sceKernelFreeMemBlock
#define sceKernelGetFreeMemorySize melee_psp2_sceKernelGetFreeMemorySize
#define sceKernelGetMemBlockBase melee_psp2_sceKernelGetMemBlockBase
#define sceKernelGetProcessId melee_psp2_sceKernelGetProcessId
#define sceKernelGetProcessTimeWide melee_psp2_sceKernelGetProcessTimeWide
#define sceKernelGetSystemTimeWide melee_psp2_sceKernelGetSystemTimeWide
#define sceKernelGetThreadId melee_psp2_sceKernelGetThreadId
#define sceKernelIsPSVitaTV melee_psp2_sceKernelIsPSVitaTV
#define sceKernelLockLwMutex melee_psp2_sceKernelLockLwMutex
#define sceKernelLockMutex melee_psp2_sceKernelLockMutex
#define sceKernelPollSema melee_psp2_sceKernelPollSema
#define sceKernelSignalSema melee_psp2_sceKernelSignalSema
#define sceKernelStartThread melee_psp2_sceKernelStartThread
#define sceKernelTryLockLwMutex melee_psp2_sceKernelTryLockLwMutex
#define sceKernelTryLockMutex melee_psp2_sceKernelTryLockMutex
#define sceKernelUnlockLwMutex melee_psp2_sceKernelUnlockLwMutex
#define sceKernelUnlockMutex melee_psp2_sceKernelUnlockMutex
#define sceKernelWaitSema melee_psp2_sceKernelWaitSema
#define sceKernelWaitThreadEnd melee_psp2_sceKernelWaitThreadEnd
#define sceSysmoduleLoadModule melee_psp2_sceSysmoduleLoadModule
#define sceSysmoduleUnloadModule melee_psp2_sceSysmoduleUnloadModule
#define sceTouchGetPanelInfo melee_psp2_sceTouchGetPanelInfo
#define sceTouchPeek melee_psp2_sceTouchPeek
#define sceTouchSetSamplingState melee_psp2_sceTouchSetSamplingState

#ifdef __cplusplus
extern "C" {
#endif

typedef int SceUID;
typedef unsigned int SceSize;
typedef int SceSSize;
typedef unsigned int SceUInt;
typedef int SceInt;
typedef uint32_t SceUInt32;
typedef int32_t SceInt32;
typedef uint64_t SceUInt64;
typedef int64_t SceInt64;
typedef int64_t SceOff;
typedef unsigned int SceMode;
typedef int SceBool;
typedef void* ScePVoid;

/* ---- kernel: process, time ---------------------------------------------- */
SceUInt64 sceKernelGetProcessTimeWide(void);
SceUInt64 sceKernelGetSystemTimeWide(void);
int sceKernelExitProcess(int result);
int sceKernelIsPSVitaTV(void);
int sceKernelGetProcessId(void);

/* ---- kernel: threads and synchronisation -------------------------------- */
#define SCE_KERNEL_CPU_MASK_USER_0 (0x01 << 16)
#define SCE_KERNEL_CPU_MASK_USER_1 (0x01 << 17)
#define SCE_KERNEL_CPU_MASK_USER_2 (0x01 << 18)
#define SCE_KERNEL_CPU_MASK_USER_ALL \
    (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2)
#define SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT 0
#define SCE_KERNEL_MUTEX_ATTR_RECURSIVE 0x02
#define SCE_KERNEL_LOWEST_PRIORITY_USER 191
#define SCE_KERNEL_HIGHEST_PRIORITY_USER 64
#define SCE_KERNEL_DEFAULT_PRIORITY_USER 0x10000100

typedef int (*SceKernelThreadEntry)(SceSize args, void* argp);

typedef struct SceKernelLwMutexWork {
    int64_t data[4];
} SceKernelLwMutexWork;

SceUID sceKernelCreateThread(const char* name, SceKernelThreadEntry entry,
                             int priority, SceSize stack_size, SceUInt attr,
                             int cpu_affinity_mask, const void* option);
int sceKernelStartThread(SceUID thid, SceSize arglen, void* argp);
int sceKernelWaitThreadEnd(SceUID thid, int* stat, SceUInt* timeout);
int sceKernelDeleteThread(SceUID thid);
int sceKernelExitDeleteThread(int status);
int sceKernelExitThread(int status);
int sceKernelGetThreadId(void);
int sceKernelDelayThread(SceUInt delay_us);
int sceKernelDelayThreadCB(SceUInt delay_us);
int sceKernelChangeThreadCpuAffinityMask(SceUID thid, int mask);
int sceKernelChangeThreadPriority(SceUID thid, int priority);

SceUID sceKernelCreateSema(const char* name, SceUInt attr, int init_val,
                           int max_val, const void* option);
int sceKernelDeleteSema(SceUID semaid);
int sceKernelSignalSema(SceUID semaid, int signal);
int sceKernelWaitSema(SceUID semaid, int signal, SceUInt* timeout);
int sceKernelPollSema(SceUID semaid, int signal);

SceUID sceKernelCreateMutex(const char* name, SceUInt attr, int init_count,
                            const void* option);
int sceKernelDeleteMutex(SceUID mutexid);
int sceKernelLockMutex(SceUID mutexid, int lock_count, SceUInt* timeout);
int sceKernelTryLockMutex(SceUID mutexid, int lock_count);
int sceKernelUnlockMutex(SceUID mutexid, int unlock_count);

int sceKernelCreateLwMutex(SceKernelLwMutexWork* work, const char* name,
                           unsigned int attr, int init_count, const void* option);
int sceKernelDeleteLwMutex(SceKernelLwMutexWork* work);
int sceKernelLockLwMutex(SceKernelLwMutexWork* work, int lock_count,
                         unsigned int* timeout);
int sceKernelTryLockLwMutex(SceKernelLwMutexWork* work, int lock_count);
int sceKernelUnlockLwMutex(SceKernelLwMutexWork* work, int unlock_count);

#define SCE_KERNEL_ERROR_WAIT_TIMEOUT ((int) 0x80028005)

/* ---- kernel: memory ----------------------------------------------------- */
typedef enum SceKernelMemBlockType {
    SCE_KERNEL_MEMBLOCK_TYPE_USER_RW = 0x0c20d060,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW = 0x09408060,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE = 0x0c208060,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW = 0x0c80d060,
} SceKernelMemBlockType;
typedef struct SceKernelFreeMemorySizeInfo {
    int size;
    int size_user;
    int size_cdram;
    int size_phycont;
} SceKernelFreeMemorySizeInfo;
SceUID sceKernelAllocMemBlock(const char* name, SceKernelMemBlockType type,
                              SceSize size, const void* opt);
int sceKernelFreeMemBlock(SceUID uid);
int sceKernelGetMemBlockBase(SceUID uid, void** base);
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo* info);

/* ---- io ----------------------------------------------------------------- */
#define SCE_O_RDONLY 0x0001
#define SCE_O_WRONLY 0x0002
#define SCE_O_RDWR (SCE_O_RDONLY | SCE_O_WRONLY)
#define SCE_O_NBLOCK 0x0004
#define SCE_O_APPEND 0x0100
#define SCE_O_CREAT 0x0200
#define SCE_O_TRUNC 0x0400
#define SCE_O_EXCL 0x0800
#define SCE_SEEK_SET 0
#define SCE_SEEK_CUR 1
#define SCE_SEEK_END 2
#define SCE_S_IFMT 0xF000
#define SCE_S_IFDIR 0x1000
#define SCE_S_IFREG 0x2000
#define SCE_S_ISDIR(m) (((m) & SCE_S_IFMT) == SCE_S_IFDIR)
#define SCE_S_ISREG(m) (((m) & SCE_S_IFMT) == SCE_S_IFREG)

typedef struct SceDateTime {
    unsigned short year, month, day, hour, minute, second;
    unsigned int microsecond;
} SceDateTime;
typedef struct SceIoStat {
    SceMode st_mode;
    unsigned int st_attr;
    SceOff st_size;
    /* st_ctime & co. are macros in FreeBSD's sys/stat.h. */
    SceDateTime sce_st_ctime;
    SceDateTime sce_st_atime;
    SceDateTime sce_st_mtime;
    unsigned int st_private[6];
} SceIoStat;

/* Vita device paths are translated to PS5 paths:
 *   ux0:data/melee/... -> /data/melee/...   (writable user data)
 *   app0:...            -> /app0/...         (read-only title image)
 *   ux0:/ur0:...        -> /data/...                                   */
const char* melee_ps5_translate_path(const char* path, char* out, size_t size);

SceUID sceIoOpen(const char* file, int flags, SceMode mode);
int sceIoClose(SceUID fd);
SceSSize sceIoRead(SceUID fd, void* data, SceSize size);
SceSSize sceIoWrite(SceUID fd, const void* data, SceSize size);
SceSSize sceIoPread(SceUID fd, void* data, SceSize size, SceOff offset);
SceSSize sceIoPwrite(SceUID fd, const void* data, SceSize size, SceOff offset);
SceOff sceIoLseek(SceUID fd, SceOff offset, int whence);
int sceIoMkdir(const char* dir, SceMode mode);
int sceIoRemove(const char* file);
int sceIoRename(const char* oldname, const char* newname);
int sceIoGetstat(const char* file, SceIoStat* stat);
int sceIoRmdir(const char* path);

/* ---- audio -------------------------------------------------------------- */
typedef enum SceAudioOutPortType {
    SCE_AUDIO_OUT_PORT_TYPE_MAIN = 0,
    SCE_AUDIO_OUT_PORT_TYPE_BGM = 1,
    SCE_AUDIO_OUT_PORT_TYPE_VOICE = 2,
} SceAudioOutPortType;
typedef enum SceAudioOutMode {
    SCE_AUDIO_OUT_MODE_MONO = 0,
    SCE_AUDIO_OUT_MODE_STEREO = 1,
} SceAudioOutMode;
int sceAudioOutOpenPort(SceAudioOutPortType type, int len, int freq,
                        SceAudioOutMode mode);
int sceAudioOutReleasePort(int port);
int sceAudioOutOutput(int port, const void* buf);

/* ---- controller --------------------------------------------------------- */
enum {
    SCE_CTRL_SELECT = 0x00000001,
    SCE_CTRL_L3 = 0x00000002,
    SCE_CTRL_R3 = 0x00000004,
    SCE_CTRL_START = 0x00000008,
    SCE_CTRL_UP = 0x00000010,
    SCE_CTRL_RIGHT = 0x00000020,
    SCE_CTRL_DOWN = 0x00000040,
    SCE_CTRL_LEFT = 0x00000080,
    SCE_CTRL_LTRIGGER = 0x00000100,
    SCE_CTRL_L2 = SCE_CTRL_LTRIGGER,
    SCE_CTRL_RTRIGGER = 0x00000200,
    SCE_CTRL_R2 = SCE_CTRL_RTRIGGER,
    SCE_CTRL_L1 = 0x00000400,
    SCE_CTRL_R1 = 0x00000800,
    SCE_CTRL_TRIANGLE = 0x00001000,
    SCE_CTRL_CIRCLE = 0x00002000,
    SCE_CTRL_CROSS = 0x00004000,
    SCE_CTRL_SQUARE = 0x00008000,
    SCE_CTRL_INTERCEPTED = 0x00010000,
    SCE_CTRL_PSBUTTON = SCE_CTRL_INTERCEPTED,
};
enum {
    SCE_CTRL_MODE_DIGITAL = 0,
    SCE_CTRL_MODE_ANALOG = 1,
    SCE_CTRL_MODE_ANALOG_WIDE = 2,
};
typedef struct SceCtrlData {
    SceUInt64 timeStamp;
    unsigned int buttons;
    unsigned char lx, ly, rx, ry;
    uint8_t up, right, down, left;
    uint8_t lt, rt, l1, r1;
    uint8_t triangle, circle, cross, square;
    uint8_t reserved[4];
} SceCtrlData;
int sceCtrlSetSamplingMode(int mode);
int sceCtrlSetSamplingModeExt(int mode);
int sceCtrlPeekBufferPositive(int port, SceCtrlData* pad_data, int count);
int sceCtrlPeekBufferPositive2(int port, SceCtrlData* pad_data, int count);
int sceCtrlPeekBufferPositiveExt2(int port, SceCtrlData* pad_data, int count);
int sceCtrlReadBufferPositive(int port, SceCtrlData* pad_data, int count);

/* ---- touch (none on PS5: the panel reports no touches) ------------------- */
#define SCE_TOUCH_PORT_FRONT 0
#define SCE_TOUCH_PORT_BACK 1
#define SCE_TOUCH_SAMPLING_STATE_STOP 0
#define SCE_TOUCH_SAMPLING_STATE_START 1
typedef struct SceTouchReport {
    uint8_t id;
    uint8_t force;
    uint16_t x;
    uint16_t y;
    int8_t reserved[8];
    uint16_t info;
} SceTouchReport;
typedef struct SceTouchData {
    SceUInt64 timeStamp;
    SceUInt32 status;
    SceUInt32 reportNum;
    SceTouchReport report[8];
} SceTouchData;
typedef struct SceTouchPanelInfo {
    int16_t minAaX, minAaY, maxAaX, maxAaY;
    int16_t minDispX, minDispY, maxDispX, maxDispY;
    uint8_t minForce, maxForce;
    uint8_t reserved[30];
} SceTouchPanelInfo;
int sceTouchSetSamplingState(int port, int state);
int sceTouchGetPanelInfo(int port, SceTouchPanelInfo* info);
int sceTouchPeek(int port, SceTouchData* data, int count);

/* ---- display ------------------------------------------------------------ */
int sceDisplayWaitVblankStart(void);

/* ---- sysmodule / net (debug logging over the network is Vita-only) ------- */
#define SCE_SYSMODULE_NET 0x0001
int sceSysmoduleLoadModule(int id);
int sceSysmoduleUnloadModule(int id);

#ifdef __cplusplus
}
#endif

#endif
