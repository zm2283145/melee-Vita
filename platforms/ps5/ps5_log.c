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
 * (platforms/ps5/tools/log-listener.ps1; build with MELEE_PS5_LOG_HOST=<pc ip>); nothing happens if none listens. */
#ifndef MELEE_PS5_LOG_HOST
#define MELEE_PS5_LOG_HOST "127.0.0.1"
#endif
#ifndef MELEE_PS5_LOG_PORT
#define MELEE_PS5_LOG_PORT 18200
#endif

/* The network stream is sent by its own thread from a ring buffer, so a
 * missing or firewalled listener can never stall the game. */
#define NET_RING_SIZE (1u << 20)
static char s_net_ring[NET_RING_SIZE];
static volatile u_int s_net_head, s_net_tail; /* bytes written / sent */
static pthread_mutex_t s_net_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_net_cond = PTHREAD_COND_INITIALIZER;
static int s_net_started;

static int connect_log_socket(void)
{
    struct sockaddr_in address;
    struct timeval timeout = { 2, 0 };
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(MELEE_PS5_LOG_PORT);
    inet_pton(AF_INET, MELEE_PS5_LOG_HOST, &address.sin_addr);
    if (connect(fd, (struct sockaddr*) &address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void* net_thread(void* arg)
{
    int fd = -1;
    unsigned failures = 0;
    (void) arg;
    for (;;) {
        u_int head, tail;
        pthread_mutex_lock(&s_net_lock);
        while (s_net_head == s_net_tail) pthread_cond_wait(&s_net_cond, &s_net_lock);
        head = s_net_head;
        tail = s_net_tail;
        pthread_mutex_unlock(&s_net_lock);
        if (fd < 0) {
            if (failures >= 3u) {
                /* Nobody listens: drop what is queued and stop trying. */
                s_net_tail = head;
                continue;
            }
            fd = connect_log_socket();
            if (fd < 0) {
                ++failures;
                sleep(1);
                continue;
            }
        }
        while (tail != head) {
            const u_int offset = tail % NET_RING_SIZE;
            u_int chunk = head - tail;
            ssize_t sent;
            if (chunk > NET_RING_SIZE - offset) chunk = NET_RING_SIZE - offset;
            sent = send(fd, s_net_ring + offset, chunk, 0);
            if (sent <= 0) {
                close(fd);
                fd = -1;
                ++failures;
                break;
            }
            tail += (u_int) sent;
            s_net_tail = tail;
        }
    }
    return NULL;
}

static void send_line(const char* text, size_t length)
{
#ifdef MELEE_PS5_NET_LOG
    pthread_mutex_lock(&s_net_lock);
    if (!s_net_started) {
        pthread_t thread;
        s_net_started = 1;
        if (pthread_create(&thread, NULL, net_thread, NULL) == 0) pthread_detach(thread);
    }
    if (s_net_head - s_net_tail + length <= NET_RING_SIZE) {
        for (size_t i = 0; i < length; ++i) s_net_ring[(s_net_head + i) % NET_RING_SIZE] = text[i];
        s_net_head += (u_int) length;
        pthread_cond_signal(&s_net_cond);
    }
    pthread_mutex_unlock(&s_net_lock);
#else
    (void) text;
    (void) length;
#endif
}

/* Lets the crash handler give the network thread a moment to send. */
void melee_ps5_log_drain(void)
{
    for (int i = 0; i < 50 && s_net_head != s_net_tail; ++i) usleep(20000);
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
