/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Force-included into the shared Vita platform sources on PS5: their file
 * paths use Vita devices ("ux0:data/melee/..."), which these wrappers map to
 * PS5 paths (see melee_ps5_translate_path). */
#ifndef MELEE_PS5_COMPAT_H
#define MELEE_PS5_COMPAT_H

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif
FILE* melee_ps5_fopen(const char* path, const char* mode);
int melee_ps5_remove(const char* path);
int melee_ps5_rename(const char* from, const char* to);
int melee_ps5_mkdir(const char* path, mode_t mode);
DIR* melee_ps5_opendir(const char* path);
int melee_ps5_stat(const char* path, struct stat* st);
#ifdef __cplusplus
}
#endif

#define fopen(p, m) melee_ps5_fopen((p), (m))
#define remove(p) melee_ps5_remove(p)
#define rename(a, b) melee_ps5_rename((a), (b))
#define mkdir(p, m) melee_ps5_mkdir((p), (m))
#define opendir(p) melee_ps5_opendir(p)
#define stat(p, s) melee_ps5_stat((p), (s))

#endif
