/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Boot-time Direct connect test for Slippi Direct play bring-up.  This one
 * talks to a real person on Slippi Dolphin, so unlike the 3DS self-test
 * (slippi_selftest.c) it doesn't expect a test pattern back.  It only checks
 * that every step of a real Direct match works without the game running:
 *
 *   1. log in with user.json and ask matchmaking for the opponent's code
 *   2. connect to the opponent's Dolphin
 *   3. lock in Fox and exchange selections, so their CSS shows us ready
 *   4. wait for their game to start and send us pads
 *   5. play 30 s of lockstep frames with a neutral pad (Fox stands still)
 *   6. leave, so their Dolphin shows the opponent disconnected
 *
 * The opponent's own character should move normally the whole time.  The
 * boot screen stays black while this runs, at most about 8 minutes. */
#include "slippi_directtest_vita.h"

#include "slippi_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Waiting for a person to type our code on their PC, then to pick a
 * character and stage, takes a while, so the limits are generous. */
#define DIRECTTEST_CONNECT_TIMEOUT_MS (3 * 60 * 1000)
#define DIRECTTEST_SELECTION_TIMEOUT_MS (2 * 60 * 1000)
/* "Ready" only needs both characters (slippi_net_match_block), so when we
 * are player 2 the opponent may still be on the stage select screen. */
#define DIRECTTEST_FIRST_PAD_TIMEOUT_MS (2 * 60 * 1000)
#define DIRECTTEST_FRAMES (30 * 60)

/* External character id 2 is Fox, the same as the 3DS self-test default.
 * Stage 0x1F is Battlefield. */
#define DIRECTTEST_CHARACTER 2
#define DIRECTTEST_STAGE 0x1F

/* Reads the first line of directtest.txt as the opponent's connect code.
 * Returns 0 if the file is missing or empty. */
static int read_opponent_code(char* code, int code_size)
{
    char path[256];
    char* text;
    int length = 0;
    int i;

    snprintf(path, sizeof(path), "%s/directtest.txt", sp_data_dir());
    text = sp_read_file(path, NULL);
    if (text == NULL) return 0;

    /* Skip spaces before the code, then copy up to the end of the line.
     * Codes are upper case on the Slippi side, so fix "abcd#123" too. */
    i = 0;
    while (text[i] == ' ' || text[i] == '\t') i++;
    while (text[i] != '\0' && text[i] != '\r' && text[i] != '\n' && text[i] != ' ' &&
           length + 1 < code_size) {
        code[length] = (char) toupper((unsigned char) text[i]);
        length++;
        i++;
    }
    code[length] = '\0';
    free(text);
    return length > 0;
}

static const char* status_name(int status)
{
    if (status == 0) return "idle";
    if (status == 1) return "searching";
    if (status == 2) return "connecting";
    if (status == 3) return "connected";
    if (status == 4) return "failed";
    if (status == 5) return "disconnected";
    return "?";
}

/* Services the network until done() says yes.  Returns 1 if it did, 0 on a
 * timeout or when the connection fails. */
static int wait_until(int (*done)(void), int timeout_ms, int* last_status)
{
    uint64_t end = sp_time_us() + (uint64_t) timeout_ms * 1000u;
    int status;

    while (sp_time_us() < end) {
        slippi_net_poll();
        status = slippi_net_status();
        if (status != *last_status) {
            sp_log("directtest: status %s", status_name(status));
            *last_status = status;
        }
        if (status == 4 || status == 5) return 0;
        if (done()) return 1;
        sp_sleep_us(5000);
    }
    return 0;
}

static int is_connected(void)
{
    return slippi_net_status() == 3;
}

static unsigned char s_match_block[0x140];

static int match_block_ready(void)
{
    return slippi_net_match_block(s_match_block);
}

static int first_remote_pad_arrived(void)
{
    return slippi_latest_remote_frame() > 0;
}

static int pad_is_neutral(const unsigned char pad[8])
{
    int i;
    for (i = 0; i < 8; i++) {
        if (pad[i] != 0) return 0;
    }
    return 1;
}

/* Lockstep for DIRECTTEST_FRAMES frames at 59.94 Hz, like the 3DS
 * self-test loop but with a neutral pad and no checksums.  Returns the
 * number of frames played. */
static int play_frames(int delay, int* moving_frames)
{
    unsigned char local_pad[12];
    unsigned char remote_pad[12];
    uint64_t next_frame_us = sp_time_us();
    int frame = 1;
    int stalls = 0;
    int logged_pads = 0;

    /* All zero is a neutral GameCube pad: no buttons, sticks centred. */
    memset(local_pad, 0, sizeof(local_pad));
    *moving_frames = 0;

    while (frame <= DIRECTTEST_FRAMES) {
        uint64_t now = sp_time_us();
        int result;

        if (now < next_frame_us) {
            uint64_t wait_us = next_frame_us - now;
            slippi_net_poll();
            if (wait_us > 2000) wait_us = 2000;
            sp_sleep_us((uint32_t) wait_us);
            continue;
        }
        next_frame_us += SLIPPI_FRAME_US;

        /* checksum 0 at frame 0 means "no checksum": Dolphin only checks
         * frames past the start of the match, so it skips ours. */
        result = slippi_net_send_inputs(frame, delay, 0, 0, local_pad, remote_pad);
        if (result == 3) {
            sp_log("directtest: connection lost at frame %d: %s", frame, slippi_net_error());
            return frame - 1;
        }
        if (result == 2) {
            /* Their pad for this frame isn't here yet, try again next tick. */
            stalls++;
            continue;
        }

        if (!pad_is_neutral(remote_pad)) {
            (*moving_frames)++;
            if (logged_pads < 5) {
                sp_log("directtest: frame %d opponent pad %02x %02x %02x %02x %02x %02x %02x %02x", frame,
                       remote_pad[0], remote_pad[1], remote_pad[2], remote_pad[3], remote_pad[4],
                       remote_pad[5], remote_pad[6], remote_pad[7]);
                logged_pads++;
            }
        }

        if (frame % 300 == 0) {
            sp_log("directtest: frame %d, ping %d ms, time offset %d us, stalls %d, ENet rtt %d ms, "
                   "loss %d/65536",
                   frame, slippi_net_ping_ms(), slippi_time_offset_us(), stalls,
                   slippi_stat(SLIPPI_STAT_ENET_RTT_MS), slippi_stat(SLIPPI_STAT_ENET_LOSS));
        }
        frame++;
    }
    return frame - 1;
}

static int run_directtest(const char* opponent_code)
{
    char name[64];
    char code[32];
    unsigned int rng;
    int last_status = -1;
    int game_index;
    int frames_played;
    int moving_frames = 0;
    int delay;
    uint64_t start;

    /* Steps 1 and 2: log in, matchmaking, then the P2P connection. */
    start = sp_time_us();
    if (slippi_net_start(opponent_code) != 0) {
        sp_log("directtest: FAIL at start: %s", slippi_net_error());
        return -1;
    }
    sp_log("directtest: searching for %s.  Enter this Vita's code in Direct on the PC now.",
           opponent_code);
    if (!wait_until(is_connected, DIRECTTEST_CONNECT_TIMEOUT_MS, &last_status)) {
        sp_log("directtest: FAIL, no connection: %s", slippi_net_error());
        return -1;
    }
    slippi_info(SLIPPI_INFO_NAME, name, sizeof(name));
    slippi_info(SLIPPI_INFO_CODE, code, sizeof(code));
    game_index = slippi_net_local_index();
    sp_log("directtest: connected in %u s to '%s' (%s), we are player %d",
           (unsigned int) ((sp_time_us() - start) / 1000000u), name, code, game_index + 1);

    /* Step 3: lock in.  Like the 3DS self-test, player 1 picks the stage and
     * player 2 leaves it to them. */
    slippi_net_set_selections(DIRECTTEST_CHARACTER, 0, DIRECTTEST_STAGE, game_index == 0);
    if (game_index == 0) sp_log("directtest: picked Fox and Battlefield, waiting for their character");
    else sp_log("directtest: picked Fox, waiting for their character and stage");
    if (!wait_until(match_block_ready, DIRECTTEST_SELECTION_TIMEOUT_MS, &last_status)) {
        sp_log("directtest: FAIL, no selections from the opponent: %s", slippi_net_error());
        return -1;
    }
    rng = ((unsigned int) s_match_block[0x138] << 24) | ((unsigned int) s_match_block[0x139] << 16) |
          ((unsigned int) s_match_block[0x13A] << 8) | s_match_block[0x13B];
    /* As player 2 the stage here may still be the Battlefield default. */
    sp_log("directtest: match ready: stage 0x%02x%02x, p1 character %u, p2 character %u, rng 0x%08x",
           s_match_block[0xE], s_match_block[0xF], s_match_block[0x60], s_match_block[0x84], rng);

    /* Step 4: their Dolphin loads the stage now.  Reset the frame state
     * before their first pad arrives, then wait for it.  Starting lockstep
     * right away would hit the 7 s stall limit while they load. */
    slippi_net_new_game();
    if (!wait_until(first_remote_pad_arrived, DIRECTTEST_FIRST_PAD_TIMEOUT_MS, &last_status)) {
        sp_log("directtest: FAIL, the opponent's game never started: %s", slippi_net_error());
        return -1;
    }
    sp_log("directtest: opponent's game started");

    /* Step 5: lockstep. */
    delay = g_slippi.cfg.delay;
    frames_played = play_frames(delay, &moving_frames);
    sp_log("directtest: played %d of %d frames, delay %d, opponent moved on %d frames", frames_played,
           DIRECTTEST_FRAMES, delay, moving_frames);

    if (frames_played < DIRECTTEST_FRAMES) return -1;
    return 0;
}

void melee_vita_slippi_directtest(void)
{
    char opponent_code[32];
    int result;
    uint64_t linger_end;

    if (!read_opponent_code(opponent_code, sizeof(opponent_code))) return;

    sp_log("directtest: started");
    result = run_directtest(opponent_code);

    /* Step 6: keep the connection serviced a moment so our last pads and
     * acks reach them, then leave. */
    linger_end = sp_time_us() + 1500u * 1000u;
    while (sp_time_us() < linger_end) {
        slippi_net_poll();
        sp_sleep_us(5000);
    }
    slippi_net_stop();

    if (result == 0) sp_log("directtest: RESULT PASS");
    else sp_log("directtest: RESULT FAIL");
}
