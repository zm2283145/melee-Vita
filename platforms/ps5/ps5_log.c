/* SPDX-License-Identifier: GPL-3.0-or-later */
/* File log for PS5 bring-up, also serving the Vita layer's log API. */
#include "ps5_log.h"
#include "../vita/vita_log.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Titles run sandboxed: /download0 is their writable data directory. */
#define MELEE_PS5_LOG_PATH "/download0/melee-ps5.log"
/* Bring-up builds also stream the log to a listener on the development PC
 * (platforms/ps5/tools/log-listener.ps1); nothing happens if none listens. */
#ifndef MELEE_PS5_LOG_HOST
#define MELEE_PS5_LOG_HOST "10.1.1.146"
#endif
#ifndef MELEE_PS5_LOG_PORT
#define MELEE_PS5_LOG_PORT 18200
#endif

/* Off unless built with MELEE_PS5_NET_LOG: a firewalled host makes the
 * connect stall the first log call. */
#ifdef MELEE_PS5_NET_LOG
static int s_socket = -2;
#else
static int s_socket = -1;
#endif

static void connect_log_socket(void)
{
    struct sockaddr_in address;
    struct pollfd pfd;
    int flags;
    s_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (s_socket < 0) return;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(MELEE_PS5_LOG_PORT);
    inet_pton(AF_INET, MELEE_PS5_LOG_HOST, &address.sin_addr);
    flags = fcntl(s_socket, F_GETFL, 0);
    fcntl(s_socket, F_SETFL, flags | O_NONBLOCK);
    if (connect(s_socket, (struct sockaddr*) &address, sizeof(address)) != 0) {
        pfd.fd = s_socket;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        if (poll(&pfd, 1, 500) != 1 || !(pfd.revents & POLLOUT) || (pfd.revents & (POLLERR | POLLHUP))) {
            close(s_socket);
            s_socket = -1;
            return;
        }
    }
    fcntl(s_socket, F_SETFL, flags);
}

static void send_line(const char* text, size_t length)
{
    if (s_socket == -2) connect_log_socket();
    if (s_socket < 0) return;
    while (length > 0) {
        const ssize_t sent = send(s_socket, text, length, 0);
        if (sent <= 0) {
            close(s_socket);
            s_socket = -1;
            return;
        }
        text += sent;
        length -= (size_t) sent;
    }
}

static FILE* s_log;
static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec s_start;

static void vlog(const char* format, va_list args)
{
    struct timespec now;
    va_list copy;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (s_start.tv_sec == 0 && s_start.tv_nsec == 0) s_start = now;
    const double t = (double) (now.tv_sec - s_start.tv_sec) +
                     (double) (now.tv_nsec - s_start.tv_nsec) * 1e-9;
    char line[2048];
    int length = snprintf(line, sizeof(line), "[%9.3f] ", t);
    va_copy(copy, args);
    length += vsnprintf(line + length, sizeof(line) - (size_t) length - 1u, format, copy);
    va_end(copy);
    if (length > (int) sizeof(line) - 2) length = (int) sizeof(line) - 2;
    line[length++] = '\n';
    line[length] = '\0';
    pthread_mutex_lock(&s_log_lock);
    if (s_log == NULL) s_log = fopen(MELEE_PS5_LOG_PATH, "w");
    if (s_log != NULL) {
        fputs(line, s_log);
        fflush(s_log);
    }
    send_line(line, (size_t) length);
    pthread_mutex_unlock(&s_log_lock);
}

void melee_ps5_log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vlog(format, args);
    va_end(args);
}

void melee_ps5_log_flush(void)
{
    pthread_mutex_lock(&s_log_lock);
    if (s_log != NULL) fflush(s_log);
    pthread_mutex_unlock(&s_log_lock);
}

int melee_vita_log_start(void)
{
    melee_ps5_log("Melee PS5 log started");
    return 0;
}

void melee_vita_log_info(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vlog(format, args);
    va_end(args);
}

int melee_vita_debugger_wait(void) { return -1; }

void melee_vita_log_stop(void)
{
    pthread_mutex_lock(&s_log_lock);
    if (s_log != NULL) fclose(s_log);
    s_log = NULL;
    pthread_mutex_unlock(&s_log_lock);
}
