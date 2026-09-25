#include "homebrew_update_client.h"

#include <psp2/net/net.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <string.h>

#include "pending_update.h"

#define HBU_STATUS_PORT 13379
#define HBU_CLIENT_TIMEOUT_US 250000
#define HBU_RESPONSE_CAPACITY 512

static int send_all(int socket, const char *data, int length)
{
    int sent = 0;

    while (sent < length) {
        int result = sceNetSend(socket, data + sent,
                                (unsigned int)(length - sent), 0);
        if (result <= 0)
            return -1;
        sent += result;
    }
    return 0;
}

static int get_status(const char *title_id)
{
    SceNetSockaddrIn address;
    char request[192];
    char response[HBU_RESPONSE_CAPACITY];
    char *body;
    int socket;
    int timeout = HBU_CLIENT_TIMEOUT_US;
    int total = 0;
    int status;
    int request_length;

    if (title_id != NULL && title_id[0] != '\0') {
        request_length = snprintf(request, sizeof(request),
                                  "GET /status/%s HTTP/1.1\r\n"
                                  "Host: 127.0.0.1\r\n"
                                  "Connection: close\r\n\r\n",
                                  title_id);
    } else {
        request_length = snprintf(request, sizeof(request),
                                  "GET /status HTTP/1.1\r\n"
                                  "Host: 127.0.0.1\r\n"
                                  "Connection: close\r\n\r\n");
    }
    if (request_length <= 0 || request_length >= (int)sizeof(request))
        return HOMEBREW_UPDATE_NOT_DETECTED;

    socket = sceNetSocket("vhbu_status_client", SCE_NET_AF_INET,
                          SCE_NET_SOCK_STREAM, 0);
    if (socket < 0)
        return HOMEBREW_UPDATE_NOT_DETECTED;

    (void)sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO,
                           &timeout, sizeof(timeout));
    (void)sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO,
                           &timeout, sizeof(timeout));

    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = SCE_NET_AF_INET;
    address.sin_port = sceNetHtons(HBU_STATUS_PORT);
    address.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_LOOPBACK);

    if (sceNetConnect(socket, (SceNetSockaddr *)&address,
                      sizeof(address)) < 0 ||
        send_all(socket, request, request_length) < 0) {
        sceNetSocketClose(socket);
        return HOMEBREW_UPDATE_NOT_DETECTED;
    }

    while (total < (int)sizeof(response) - 1) {
        int received = sceNetRecv(socket, response + total,
                                  sizeof(response) - 1u - (unsigned int)total,
                                  0);
        if (received <= 0)
            break;
        total += received;
    }
    sceNetSocketClose(socket);
    response[total] = '\0';

    if (strncmp(response, "HTTP/1.1 200 ", 13) != 0 &&
        strncmp(response, "HTTP/1.0 200 ", 13) != 0)
        return HOMEBREW_UPDATE_NOT_DETECTED;
    body = strstr(response, "\r\n\r\n");
    if (body == NULL)
        return HOMEBREW_UPDATE_NOT_DETECTED;
    body += 4;
    if (sscanf(body, "{\"status\":%d}", &status) != 1 ||
        status < HOMEBREW_UPDATE_DISABLED ||
        status > HOMEBREW_UPDATE_READY)
        return HOMEBREW_UPDATE_NOT_DETECTED;
    return status;
}

int HomebrewUpdateClientGetStatus(void)
{
    return get_status(NULL);
}

int HomebrewUpdateClientGetStatusForTitle(const char *title_id)
{
    return get_status(title_id);
}

int HomebrewUpdateClientLaunchPendingInstaller(const char *title_id)
{
    VhbuPendingUpdate pending;
    char uri[64];
    SceUID file;
    int received;
    int length;
    int result;

    if (title_id == NULL || strlen(title_id) != 9u)
        return -1;
    file = sceIoOpen(VHBU_PENDING_PATH, SCE_O_RDONLY, 0);
    if (file < 0)
        return 0;
    memset(&pending, 0, sizeof(pending));
    received = sceIoRead(file, &pending, sizeof(pending));
    sceIoClose(file);
    if (received != (int)sizeof(pending) ||
        pending.magic != VHBU_PENDING_MAGIC ||
        pending.format != VHBU_PENDING_FORMAT ||
        strncmp(pending.title_id, title_id, sizeof(pending.title_id)) != 0)
        return -2;
    length = snprintf(uri, sizeof(uri),
                      "psgm:play?titleid=%s", VHBU_HELPER_TITLE_ID);
    if (length <= 0 || length >= (int)sizeof(uri))
        return -3;
    result = sceAppMgrLaunchAppByUri(0xfffff, uri);
    return result < 0 ? result : 1;
}
