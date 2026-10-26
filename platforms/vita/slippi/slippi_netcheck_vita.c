/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Boot-time network check for Slippi Direct play bring-up.  It walks through
 * each layer the online client needs, one step at a time, and logs which
 * step worked: SceNet, Wi-Fi, user.json, DNS, then an ENet handshake with
 * Slippi's matchmaking server.  It never asks the server for a match.
 *
 * To run it, create an empty ux0:data/melee/slippi/netcheck.txt and start
 * the game.  The boot screen stays black for up to ~15 s while it runs, then
 * the game starts as normal.  The result is in ux0:data/melee/slippi/slippi.log. */
#include "slippi_netcheck_vita.h"

#include "slippi_internal.h"

#include <stdio.h>
#include <string.h>

/* Dolphin waits 10 s for the matchmaking server (SLIPPI_MM_CONNECT_TIMEOUT_MS). */
#define NETCHECK_CONNECT_TIMEOUT_MS SLIPPI_MM_CONNECT_TIMEOUT_MS

static int netcheck_requested(void)
{
    char path[256];
    FILE* file;

    snprintf(path, sizeof(path), "%s/netcheck.txt", sp_data_dir());
    file = fopen(path, "rb");
    if (file == NULL) return 0;
    fclose(file);
    return 1;
}

/* user.json is not needed to talk to the server, but the next phase needs
 * it, so check it here too and say what is wrong with it. */
static void check_user_json(void)
{
    slippi_user user;
    char path[256];
    char error[160];

    snprintf(path, sizeof(path), "%s/user.json", sp_data_dir());
    if (slippi_user_load(&user, path, error, sizeof(error)) != 0) {
        sp_log("netcheck: user.json: %s", error);
        return;
    }
    /* Only the start of the uid, and never the play key: people share this
     * log in bug reports. */
    sp_log("netcheck: user.json ok: %s (%s), uid %.6s..., play key %u chars", user.display_name,
           user.connect_code, user.uid, (unsigned int) strlen(user.play_key));
}

/* Returns 1 once ENet says the server answered our connect, 0 on timeout. */
static int wait_for_connect(ENetHost* host, ENetPeer* peer, int timeout_ms)
{
    uint64_t start = sp_time_us();
    uint64_t end = start + (uint64_t) timeout_ms * 1000u;
    ENetEvent event;

    while (sp_time_us() < end) {
        /* 50 ms per call so the loop notices the timeout. */
        int result = enet_host_service(host, &event, 50);
        if (result < 0) {
            sp_log("netcheck: enet_host_service returned an error");
            return 0;
        }
        if (result == 0) continue;

        if (event.type == ENET_EVENT_TYPE_CONNECT && event.peer == peer) {
            sp_log("netcheck: handshake done after %u ms",
                   (unsigned int) ((sp_time_us() - start) / 1000u));
            return 1;
        }
        if (event.type == ENET_EVENT_TYPE_DISCONNECT && event.peer == peer) {
            sp_log("netcheck: server closed the connection during the handshake");
            return 0;
        }
        if (event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
    }
    sp_log("netcheck: no answer within %d ms", timeout_ms);
    return 0;
}

/* Sends a clean disconnect so the server doesn't wait for us to time out. */
static void disconnect_politely(ENetHost* host, ENetPeer* peer)
{
    uint64_t end = sp_time_us() + 1000u * 1000u;
    ENetEvent event;

    enet_peer_disconnect(peer, 0);
    while (sp_time_us() < end) {
        int result = enet_host_service(host, &event, 50);
        if (result <= 0) continue;
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(event.packet);
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            sp_log("netcheck: disconnected cleanly");
            return;
        }
    }
    /* No answer.  mm.slippi.gg didn't confirm in the first hardware test
     * either, which is harmless: drop it on our side. */
    sp_log("netcheck: server didn't confirm the disconnect, dropping the connection");
    enet_peer_reset(peer);
}

static int run_netcheck(void)
{
    char lan_ip[20];
    char server_ip[20];
    ENetAddress server;
    ENetAddress local;
    ENetHost* host;
    ENetPeer* peer;
    int connected;

    /* Step 1: SceNet. */
    if (sp_net_init() != 0) {
        sp_log("netcheck: FAIL at step 1, SceNet did not start");
        return -1;
    }
    sp_log("netcheck: step 1 ok, SceNet started");

    /* Step 2: Wi-Fi.  Keep going if it says no, the next step shows it. */
    slippi_format_ip(sp_lan_ip_fallback(), lan_ip, sizeof(lan_ip));
    sp_log("netcheck: step 2, Wi-Fi status %d (1 = connected), LAN address %s",
           sp_wifi_status(), lan_ip);

    check_user_json();

    /* Step 3: ENet. */
    if (enet_initialize() != 0) {
        sp_log("netcheck: FAIL at step 3, enet_initialize");
        sp_net_exit();
        return -1;
    }
    sp_log("netcheck: step 3 ok, ENet %u.%u.%u", ENET_VERSION_MAJOR, ENET_VERSION_MINOR,
           ENET_VERSION_PATCH);

    /* Step 4: DNS. */
    memset(&server, 0, sizeof(server));
    if (enet_address_set_host(&server, SLIPPI_MM_HOST_PROD) != 0) {
        sp_log("netcheck: FAIL at step 4, cannot resolve %s", SLIPPI_MM_HOST_PROD);
        enet_deinitialize();
        sp_net_exit();
        return -1;
    }
    server.port = SLIPPI_MM_PORT_DEFAULT;
    slippi_format_ip(server.host, server_ip, sizeof(server_ip));
    sp_log("netcheck: step 4 ok, %s is %s", SLIPPI_MM_HOST_PROD, server_ip);

    /* Step 5: a UDP socket.  Same port range and peer/channel counts as
     * slippi_create_host, so this tests what the real client will open. */
    memset(&local, 0, sizeof(local));
    local.host = ENET_HOST_ANY;
    local.port = (enet_uint16) (41000 + (sp_random() % 10000));
    host = enet_host_create(&local, 10, 3, 0, 0);
    if (host == NULL) {
        sp_log("netcheck: FAIL at step 5, cannot open UDP port %u", local.port);
        enet_deinitialize();
        sp_net_exit();
        return -1;
    }
    sp_log("netcheck: step 5 ok, UDP port %u open", local.port);

    /* Step 6: the ENet handshake with the matchmaking server. */
    peer = enet_host_connect(host, &server, 3, 0);
    if (peer == NULL) {
        sp_log("netcheck: FAIL at step 6, enet_host_connect");
        enet_host_destroy(host);
        enet_deinitialize();
        sp_net_exit();
        return -1;
    }
    enet_host_flush(host);
    connected = wait_for_connect(host, peer, NETCHECK_CONNECT_TIMEOUT_MS);
    if (connected) {
        /* peer->roundTripTime is still ENet's 500 ms starting guess here,
         * so the handshake time logged above is the better number. */
        sp_log("netcheck: step 6 ok, matchmaking server answered");
        disconnect_politely(host, peer);
    } else {
        sp_log("netcheck: FAIL at step 6, no ENet handshake with %s:%d", server_ip,
               SLIPPI_MM_PORT_DEFAULT);
        enet_peer_reset(peer);
    }

    enet_host_destroy(host);
    enet_deinitialize();
    sp_net_exit();
    return connected ? 0 : -1;
}

void melee_vita_slippi_netcheck(void)
{
    if (!netcheck_requested()) return;

    sp_log("netcheck: started");
    if (run_netcheck() == 0) sp_log("netcheck: RESULT PASS");
    else sp_log("netcheck: RESULT FAIL");
}
