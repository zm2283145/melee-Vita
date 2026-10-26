/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What the game code needs from the platform for Slippi online play.
 *
 * The 3DS Slippi port (melee-3ds slippi-beta-v0.1.0) has the same calls in
 * port/include/slippi_net_bridge.h.  There they cross an endianness bridge;
 * here the game and the platform are one program, so they are plain
 * functions.  The Vita has them in platforms/vita/slippi/slippi_engine_vita.c.
 * The PS5 build compiles the same game code and has stubs in
 * platforms/ps5/gl_game.c that keep online play switched off. */
#ifndef MELEE_SLIPPI_PLATFORM_H
#define MELEE_SLIPPI_PLATFORM_H

/* mp_platform_slippi_net_status */
#define SLIPPI_NET_IDLE 0
#define SLIPPI_NET_MATCHMAKING 1
#define SLIPPI_NET_CONNECTING 2
#define SLIPPI_NET_CONNECTED 3
#define SLIPPI_NET_FAILED 4
#define SLIPPI_NET_DISCONNECTED 5

/* mp_platform_slippi_net_send_inputs */
#define SLIPPI_NET_INPUTS_OK 1
#define SLIPPI_NET_INPUTS_SKIP 2
#define SLIPPI_NET_INPUTS_DISCONNECTED 3

/* The game info block and the 8 bytes after it
 * (mp_platform_slippi_net_match_block). */
#define SLIPPI_NET_MATCH_BLOCK_TOTAL (0x138 + 8)

/* mp_platform_slippi_ui_css flags */
#define SLIPPI_UI_LOCKED 1  /* locked in: no unselect or colour change */
#define SLIPPI_UI_START 2   /* both locked in: take the match block */
#define SLIPPI_UI_SSS 4     /* the loser picks the stage */
#define SLIPPI_UI_SHEIK 8   /* the Zelda icon plays Sheik */
/* Sounds the game should play, in the same flags. */
#define SLIPPI_UI_SFX_FORWARD 0x100
#define SLIPPI_UI_SFX_BACK 0x200
#define SLIPPI_UI_SFX_ERROR 0x400
#define SLIPPI_UI_SFX_MOVE 0x800

/* mp_platform_slippi_ui_event */
#define SLIPPI_UI_EV_CSS_ENTER 1
#define SLIPPI_UI_EV_LEAVE 2   /* backed out of the online CSS to the menus */
#define SLIPPI_UI_EV_STAGE 3   /* arg = stage | alt << 16, or -1 for none */
#define SLIPPI_UI_EV_RESULT 4  /* arg = 1 won, 0 lost */
#define SLIPPI_UI_EV_MATCH 5   /* the match scene starts */

/* A line for slippi.log.  A newline is added. */
void mp_platform_slippi_log(const char* text);
/* Sleeps about one millisecond while the game waits for the opponent. */
void mp_platform_slippi_idle(void);

/* ---- the connection ---- */
int mp_platform_slippi_net_status(void);
/* Services the network on the game thread. */
void mp_platform_slippi_net_poll(void);
/* 1 when both players have locked in.  out gets the console's game info
 * block (0x138 big-endian bytes), then the decider's RNG offset (4 bytes,
 * big-endian), this machine's player index, the input delay, the frozen
 * Stadium flag and a zero byte. */
int mp_platform_slippi_net_match_block(unsigned char* out);
/* Sends our pad for frame + delay and asks for the opponent's pad for
 * frame.  The pads are 12 bytes in the console's PAD report order.
 * Returns SLIPPI_NET_INPUTS_OK with remote_pad filled in, _SKIP when the
 * opponent's pad has not arrived yet, or _DISCONNECTED. */
int mp_platform_slippi_net_send_inputs(int frame, int delay,
                                       int finalized_frame,
                                       unsigned checksum,
                                       const unsigned char* local_pad,
                                       unsigned char* remote_pad);
/* The newest checksum the opponent sent, and its frame. */
int mp_platform_slippi_net_remote_checksum_frame(void);
unsigned mp_platform_slippi_net_remote_checksum_value(void);
int mp_platform_slippi_net_ping_ms(void);
/* Dolphin's CalcTimeOffsetUs: positive when we are ahead of the opponent. */
int mp_platform_slippi_net_time_offset_us(void);

/* ---- the online CSS session (platforms/vita/slippi/slippi_ui.c) ---- */
/* Once per CSS frame.  packed = ready | ckind << 8 | color << 16, and
 * trigger and held are the local port's HSD buttons.  Returns the
 * SLIPPI_UI_* flags. */
int mp_platform_slippi_ui_css(int packed, int trigger, int held);
void mp_platform_slippi_ui_event(int event, int arg);
/* The opponent's fighter | colour << 8 once they picked one, else -1. */
int mp_platform_slippi_ui_remote(void);
/* One line of the CSS text (see slippi_ui.c for the line numbers), as
 * bytes ready for the game's SIS text.  Returns the line's colour. */
int mp_platform_slippi_ui_text(int line, char* out, int len);
/* A player's name for the match overlay, in plain ASCII: ours when local
 * is nonzero, else the opponent's.  Returns 0 when there is no name. */
int mp_platform_slippi_ui_name(int local, char* out, int len);
/* Frozen Pokemon Stadium: 1 toggles, 0 reads.  Returns the state. */
int mp_platform_slippi_ui_alt(int toggle);
/* 1 when Slippi's patched menu files (tools/slippi/slippi_files.py) are on
 * the memory card, so the menus can show Slippi's Online Play art. */
int mp_platform_slippi_menu_files(void);

#endif
