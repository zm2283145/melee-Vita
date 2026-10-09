/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libc functions the PS5 exports only from libScePosixForWebKit, which a
 * folder title does not get: the import resolves to garbage.  The GL
 * driver's shader cache calls mkstemp; Mesa may call isatty. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

int mkstemp(char* template)
{
    static uint32_t counter;
    const size_t length = template != NULL ? strlen(template) : 0;
    if (length < 6 || memcmp(template + length - 6, "XXXXXX", 6) != 0) {
        errno = EINVAL;
        return -1;
    }
    for (int attempt = 0; attempt < 64; ++attempt) {
        static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        uint32_t v = (uint32_t) getpid() * 2654435761u + ++counter * 40503u;
        for (int i = 0; i < 6; ++i, v /= 36u) template[length - 6 + i] = digits[v % 36u];
        {
            const int fd = open(template, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (fd >= 0 || errno != EEXIST) return fd;
        }
    }
    errno = EEXIST;
    return -1;
}

int isatty(int fd)
{
    (void) fd;
    errno = ENOTTY;
    return 0;
}
