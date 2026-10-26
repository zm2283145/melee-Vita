/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The online CSS session for Slippi Direct: what Slippi's
 * HandleInputsOnCSS and LoadCSSText codes do, run once per CSS frame by the
 * game (src/melee/slippi/slippi_online_mode.c).
 *
 * Ported from the 3DS Slippi port (melee-3ds slippi-beta-v0.1.0,
 * native-3ds/port/3ds/slippi/slippi_ui.c).  The 3DS draws its connect code
 * keyboard, chat page and settings on the bottom screen.  The Vita has one
 * screen, so the code goes in through the system keyboard (the IME dialog),
 * and the delay and chat settings stay in config.ini for now.
 *
 * How the CSS works, as in Slippi:
 * - START with a fighter chosen opens the keyboard.  Confirming a code
 *   locks in and starts the search.  The first stage is random.
 * - Z cancels a search or clears an error.  Holding Z disconnects.
 * - After a game the winner presses START to lock in, and the loser
 *   presses START to pick the stage.
 * - The D-pad sends quick chat: first a page, then a message. */
#include "slippi_ui.h"
#include "slippi_internal.h"

#include <melee/slippi/slippi_platform.h>

#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vita2d.h>

/* GameCube buttons, as the game's HSD_PAD_* bits. */
#define GC_LEFT 0x0001
#define GC_RIGHT 0x0002
#define GC_DOWN 0x0004
#define GC_UP 0x0008
#define GC_Z 0x0010
#define GC_A 0x0100
#define GC_B 0x0200
#define GC_X 0x0400
#define GC_Y 0x0800
#define GC_START 0x1000

#define CODE_MAX 8
#define HISTORY_MAX 16
/* Frames Z must be held to disconnect (Slippi's 0x30). */
#define HOLD_Z_FRAMES 0x30

enum { PHASE_IDLE, PHASE_SEARCH, PHASE_CONNECTED, PHASE_ERROR };
enum { PAGE_NONE, PAGE_CSS, PAGE_CODE, PAGE_MATCH };
/* Text colours, by index into the game's table (slippi_online_mode.c). */
enum { TEXT_WHITE, TEXT_GRAY, TEXT_RED, TEXT_DONE, TEXT_WAIT, TEXT_CHAT_P1,
       TEXT_CHAT_P2 };

static struct {
    int page;
    int phase;
    int net_status;
    int ready;          /* a fighter is chosen on the CSS */
    int locked;         /* locked in */
    int need_stage;     /* lost the last game: picks the next stage */
    int chose_stage;
    int hold_z;         /* frames Z has been held while connected */
    int spinner;        /* 0 or 1, flips every 15 frames */
    int frozen_stadium; /* Slippi's alt stage mode, Z on the SSS */
    int chat_enabled;
    int chat_page;      /* -1 closed, else 0 Up, 1 Left, 2 Right, 3 Down */
    int chat_count;     /* lines in chat_text, newest last */
    char chat_text[3][64];
    int chat_port[3];   /* whose port colour each chat line gets */
    char user_name[32];
    char user_code[16];
    char target[16];    /* the code being searched for */
    char opponent_name[32];
    char opponent_code[16];
    char error[128];
} ui;

static int started;     /* the network client is running */
static int sent;        /* our selections went out for this game */
static int games;       /* games played on this connection */
static int ckind = -1;
static int color;
static int lock_ckind;
static int lock_color;
static int stage = -1;
static int alt;
static int frame;
static int pending_start;
static int pending_cancel;
static int sfx;
static unsigned stage_used; /* stages drawn from the pool this connection */
static int user_loaded;
static int history_loaded;
static int history_count;
static char history[HISTORY_MAX][CODE_MAX + 1];

/* The system keyboard.  The game thread opens and closes it; the render
 * thread only draws it while ime_open is set. */
static volatile int ime_open;
static SceWChar16 ime_title[64];
static SceWChar16 ime_initial[CODE_MAX + 1];
static SceWChar16 ime_text[CODE_MAX + 1];
/* Buttons still held when the keyboard closed.  They don't reach the game
 * until they are let go, so the button that confirmed the code doesn't
 * also press something on the CSS. */
static u16 swallow_buttons;

static void copy(char* dst, int size, const char* src)
{
    if (src == NULL) src = "";
    snprintf(dst, size, "%s", src);
}

static void path_in_data_dir(char* out, int size, const char* name)
{
    snprintf(out, size, "%s/%s", sp_data_dir(), name);
}

/* ---------------------------------------------------------- user, files */

static void load_settings(void)
{
    slippi_config cfg;
    char path[200];

    slippi_config_defaults(&cfg);
    path_in_data_dir(path, sizeof(path), "config.ini");
    slippi_config_load(&cfg, path);
    ui.chat_enabled = cfg.chat_enabled;
}

static void load_user(void)
{
    slippi_user user;
    char path[200];
    char error[160];

    if (user_loaded) return;
    user_loaded = 1;
    path_in_data_dir(path, sizeof(path), "user.json");
    if (slippi_user_load(&user, path, error, sizeof(error)) == 0) {
        copy(ui.user_name, sizeof(ui.user_name), user.display_name);
        copy(ui.user_code, sizeof(ui.user_code), user.connect_code);
    } else {
        copy(ui.user_name, sizeof(ui.user_name), "No Slippi account");
        copy(ui.user_code, sizeof(ui.user_code), "See user.json");
    }
    /* Keep the play key out of memory we don't need it in. */
    memset(&user, 0, sizeof(user));
    load_settings();
}

/* direct-codes.txt: one code per line, the most recent first. */
static void load_history(void)
{
    char path[200];
    char* text;
    int i = 0;

    if (history_loaded) return;
    history_loaded = 1;
    path_in_data_dir(path, sizeof(path), "direct-codes.txt");
    text = sp_read_file(path, NULL);
    if (text == NULL) return;
    while (text[i] != '\0' && history_count < HISTORY_MAX) {
        char line[CODE_MAX + 1];
        int n = 0;
        while (text[i] != '\0' && text[i] != '\n') {
            if (text[i] != '\r' && n < CODE_MAX) {
                line[n] = text[i];
                n++;
            }
            i++;
        }
        if (text[i] == '\n') i++;
        line[n] = '\0';
        if (n > 0) {
            copy(history[history_count], sizeof(history[0]), line);
            history_count++;
        }
    }
    free(text);
}

/* Puts code first in the history and writes the file again. */
static void save_history(const char* code)
{
    char keep[HISTORY_MAX][CODE_MAX + 1];
    char path[200];
    int count = 1;
    int i;
    FILE* file;

    copy(keep[0], sizeof(keep[0]), code);
    for (i = 0; i < history_count && count < HISTORY_MAX; i++) {
        if (strcmp(history[i], code) != 0) {
            copy(keep[count], sizeof(keep[0]), history[i]);
            count++;
        }
    }
    memcpy(history, keep, sizeof(keep));
    history_count = count;

    path_in_data_dir(path, sizeof(path), "direct-codes.txt");
    file = fopen(path, "w");
    if (file == NULL) return;
    for (i = 0; i < history_count; i++) {
        fprintf(file, "%s\n", history[i]);
    }
    fclose(file);
}

/* --------------------------------------------------------- the session */

/* Dolphin's getRandomStage: one of the server's legal stages, with no
 * repeat until every one was drawn. */
static int random_stage(void)
{
    static const unsigned char fallback[] = { 0x02, 0x03, 0x08, 0x1C, 0x1F, 0x20 };
    unsigned char pool[16];
    int count;
    int left = 0;
    int pick;
    int i;

    sp_lock();
    count = g_slippi.match.stage_count;
    if (count > 16) count = 16;
    for (i = 0; i < count; i++) {
        pool[i] = g_slippi.match.stages[i];
    }
    sp_unlock();
    if (count <= 0) {
        count = (int) sizeof(fallback);
        memcpy(pool, fallback, sizeof(fallback));
    }

    for (i = 0; i < count; i++) {
        if ((stage_used & (1u << i)) == 0) left++;
    }
    if (left == 0) {
        stage_used = 0;
        left = count;
    }
    pick = (int) (sp_random() % (unsigned) left);
    for (i = 0; i < count; i++) {
        if (stage_used & (1u << i)) continue;
        if (pick == 0) {
            stage_used |= 1u << i;
            return pool[i];
        }
        pick--;
    }
    return 0x1F;
}

static void send_selections(void)
{
    int pick = -1;
    int pick_alt = 0;

    if (games == 0) {
        /* The first stage is random. */
        pick = random_stage();
    } else if (ui.chose_stage) {
        pick = stage;
        pick_alt = alt;
    }
    /* Otherwise the winner leaves the stage to the loser. */
    slippi_set_selections(lock_ckind, lock_color, pick, pick_alt << 8);
    sent = 1;
    sp_log("Slippi UI: locked in character %d colour %d, stage %d%s", lock_ckind, lock_color,
           pick, pick_alt ? " (frozen)" : "");
}

static void stop(int phase)
{
    if (started) slippi_net_stop();
    started = 0;
    sent = 0;
    ui.locked = 0;
    ui.need_stage = 0;
    ui.chose_stage = 0;
    ui.hold_z = 0;
    ui.phase = phase;
    ui.opponent_name[0] = '\0';
    ui.opponent_code[0] = '\0';
    ui.chat_page = -1;
    ui.chat_count = 0;
    games = 0;
    stage_used = 0;
}

static void begin_search(const char* code)
{
    copy(ui.target, sizeof(ui.target), code);
    save_history(code);
    lock_ckind = ckind;
    lock_color = color;
    ui.locked = 1;
    ui.need_stage = 0;
    ui.chose_stage = 0;
    ui.error[0] = '\0';
    sent = 0;
    games = 0;
    stage_used = 0;
    sp_log("Slippi UI: searching for %s", code);
    if (slippi_net_start(code) < 0) {
        copy(ui.error, sizeof(ui.error), slippi_net_error());
        ui.phase = PHASE_ERROR;
        ui.locked = 0;
        sfx |= SLIPPI_UI_SFX_ERROR;
    } else {
        started = 1;
        ui.phase = PHASE_SEARCH;
    }
}

static int local_port(void)
{
    int index;
    sp_lock();
    index = g_slippi.match.local_index;
    sp_unlock();
    return index == 1 ? 1 : 0;
}

/* -------------------------------------------------- the system keyboard */

static void ascii_to_wide(SceWChar16* out, int count, const char* in)
{
    int i = 0;
    while (in[i] != '\0' && i + 1 < count) {
        out[i] = (SceWChar16) (unsigned char) in[i];
        i++;
    }
    out[i] = 0;
}

static void open_code_entry(void)
{
    SceImeDialogParam param;
    int result;

    load_history();
    ascii_to_wide(ime_title, 64, "Opponent's connect code");
    /* Start from the last code, like Slippi's suggestion. */
    if (history_count > 0) {
        ascii_to_wide(ime_initial, CODE_MAX + 1, history[0]);
    } else {
        ime_initial[0] = 0;
    }
    memset(ime_text, 0, sizeof(ime_text));

    sceImeDialogParamInit(&param);
    param.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
    param.languagesForced = SCE_TRUE;
    param.type = SCE_IME_TYPE_BASIC_LATIN;
    param.option = SCE_IME_OPTION_NO_ASSISTANCE;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title = ime_title;
    param.maxTextLength = CODE_MAX;
    param.initialText = ime_initial;
    param.inputTextBuffer = ime_text;
    result = sceImeDialogInit(&param);
    if (result < 0) {
        sp_log("Slippi UI: the keyboard did not open (0x%08X)", (unsigned) result);
        snprintf(ui.error, sizeof(ui.error), "The keyboard did not open (0x%08X)",
                 (unsigned) result);
        ui.phase = PHASE_ERROR;
        sfx |= SLIPPI_UI_SFX_ERROR;
        return;
    }
    ime_open = 1;
    ui.page = PAGE_CODE;
    sfx |= SLIPPI_UI_SFX_FORWARD;
}

/* Called every CSS frame while the keyboard is open.  When it has closed,
 * an entered code starts the search. */
static void poll_code_entry(void)
{
    SceImeDialogResult result;
    char code[CODE_MAX + 1];
    int n = 0;
    int i;

    if (!ime_open) return;
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return;

    memset(&result, 0, sizeof(result));
    sceImeDialogGetResult(&result);
    ime_open = 0;
    sceImeDialogTerm();
    ui.page = PAGE_CSS;
    if (result.button != SCE_IME_DIALOG_BUTTON_ENTER) {
        sfx |= SLIPPI_UI_SFX_BACK;
        return;
    }

    /* Slippi codes are upper case ASCII like "ABCD#123".  Leave out
     * spaces and anything that isn't ASCII. */
    for (i = 0; ime_text[i] != 0 && i < CODE_MAX; i++) {
        SceWChar16 c = ime_text[i];
        if (c <= ' ' || c > '~') continue;
        if (c >= 'a' && c <= 'z') c = (SceWChar16) (c - 'a' + 'A');
        code[n] = (char) c;
        n++;
    }
    code[n] = '\0';
    if (n == 0) {
        sfx |= SLIPPI_UI_SFX_ERROR;
        return;
    }
    sfx |= SLIPPI_UI_SFX_FORWARD;
    begin_search(code);
}

void melee_vita_slippi_render_dialog(void)
{
    if (!ime_open) return;
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_RUNNING) return;
    vita2d_common_dialog_update();
}

void melee_vita_slippi_pad_filter(PADStatus* pad)
{
    /* Held buttons from the keyboard come back only after a release. */
    swallow_buttons &= pad->button;
    pad->button &= (u16) ~swallow_buttons;

    if (ime_open) {
        /* Remember what is held now, so it is swallowed after closing. */
        swallow_buttons = pad->button;
        memset(pad, 0, sizeof(*pad));
        return;
    }
    if (ui.page == PAGE_CSS && ui.locked) {
        /* Locked in: A and B can't unselect and X and Y can't change the
         * colour (Slippi's PreventA/BPressCharUnselect and
         * PreventColorChange). */
        pad->button &= (u16) ~(GC_A | GC_B | GC_X | GC_Y);
    }
}

/* --------------------------------------------------------- quick chat */

/* Message ids are a page bit (Up 0x80, Left 0x10, Right 0x20, Down 0x40)
 * and a direction bit (Left 1, Right 2, Down 4, Up 8), as in Slippi. */
static const char* const default_chat[16] = {
    "ggs", "one more", "brb", "good luck", "well played", "that was fun", "thanks", "too good",
    "sorry", "my b", "lol", "wow", "gotta go", "one sec", "let's play again later",
    "bad connection",
};
static const int chat_page_bits[4] = { 0x80, 0x10, 0x20, 0x40 };
static const int chat_dir_bits[4] = { 8, 1, 2, 4 };
static int chat_cooldown;
static int chat_page_frames;
static int chat_age[3];

static const char* chat_message(int remote, int index)
{
    const char* text;
    if (index < 0 || index > 15) return "";
    if (remote) {
        text = g_slippi.match.remote_chat[index];
    } else {
        text = g_slippi.match.local_chat[index];
    }
    if (text[0] == '\0') return default_chat[index];
    return text;
}

static int chat_index(int id)
{
    int page;
    int dir;
    for (page = 0; page < 4; page++) {
        for (dir = 0; dir < 4; dir++) {
            if (id == (chat_page_bits[page] | chat_dir_bits[dir])) return page * 4 + dir;
        }
    }
    return -1;
}

static void chat_remove(int index)
{
    int k;
    for (k = index; k + 1 < ui.chat_count; k++) {
        memcpy(ui.chat_text[k], ui.chat_text[k + 1], sizeof(ui.chat_text[k]));
        ui.chat_port[k] = ui.chat_port[k + 1];
        chat_age[k] = chat_age[k + 1];
    }
    ui.chat_count--;
}

static void chat_add(int port, const char* line)
{
    if (ui.chat_count == 3) chat_remove(0);
    copy(ui.chat_text[ui.chat_count], sizeof(ui.chat_text[0]), line);
    ui.chat_port[ui.chat_count] = port;
    chat_age[ui.chat_count] = 0;
    ui.chat_count++;
}

static void send_chat(int index)
{
    char line[64];

    ui.chat_page = -1;
    if (chat_cooldown > 0) {
        sfx |= SLIPPI_UI_SFX_ERROR;
        return;
    }
    slippi_send_chat(chat_page_bits[index / 4] | chat_dir_bits[index % 4]);
    snprintf(line, sizeof(line), "%s: %s", ui.user_name[0] ? ui.user_name : ui.user_code,
             chat_message(0, index));
    chat_add(local_port(), line);
    chat_cooldown = 60;
    sfx |= SLIPPI_UI_SFX_FORWARD;
}

/* Once per CSS frame: messages coming in, old ones going away, and the
 * D-pad picking a page and then a message. */
static void chat_frame(int trigger)
{
    const char* who;
    char line[64];
    int dir = -1;
    int id;
    int i;

    if (chat_cooldown > 0) chat_cooldown--;
    /* A message stays about ten seconds. */
    for (i = 0; i < ui.chat_count; i++) {
        chat_age[i]++;
        if (chat_age[i] > 600) {
            chat_remove(i);
            i--;
        }
    }
    if (ui.phase != PHASE_CONNECTED) {
        ui.chat_page = -1;
        return;
    }

    id = slippi_chat_poll();
    if (id != 0) {
        who = ui.opponent_name[0] ? ui.opponent_name : ui.opponent_code;
        if (id == SLIPPI_CHAT_DISABLED) {
            snprintf(line, sizeof(line), "%s has chat disabled", who);
            chat_add(!local_port(), line);
        } else {
            i = chat_index(id);
            if (i >= 0) {
                snprintf(line, sizeof(line), "%s: %s", who, chat_message(1, i));
                chat_add(!local_port(), line);
            }
        }
        sfx |= SLIPPI_UI_SFX_MOVE;
    }

    if (!ui.chat_enabled || ui.page != PAGE_CSS) return;
    if (trigger & GC_UP) {
        dir = 0;
    } else if (trigger & GC_LEFT) {
        dir = 1;
    } else if (trigger & GC_RIGHT) {
        dir = 2;
    } else if (trigger & GC_DOWN) {
        dir = 3;
    }
    if (ui.chat_page >= 0) {
        chat_page_frames++;
        if (chat_page_frames > 300) ui.chat_page = -1;
    }
    if (dir < 0) return;
    if (ui.chat_page < 0) {
        ui.chat_page = dir;
        chat_page_frames = 0;
        sfx |= SLIPPI_UI_SFX_MOVE;
    } else {
        send_chat(ui.chat_page * 4 + dir);
    }
}

/* ------------------------------------------------- calls from the game */

static void update_net(void)
{
    int status = slippi_net_status();
    ui.net_status = status;
    if (status == SLIPPI_NET_FAILED) {
        copy(ui.error, sizeof(ui.error), slippi_net_error());
        stop(PHASE_ERROR);
        sfx |= SLIPPI_UI_SFX_ERROR;
    } else if (status == SLIPPI_NET_DISCONNECTED) {
        /* The opponent left: back to idle with no message, as Slippi. */
        stop(PHASE_IDLE);
        sfx |= SLIPPI_UI_SFX_BACK;
    } else if (status == SLIPPI_NET_MATCHMAKING || status == SLIPPI_NET_CONNECTING) {
        ui.phase = PHASE_SEARCH;
    } else if (status == SLIPPI_NET_CONNECTED) {
        if (ui.phase != PHASE_CONNECTED) {
            ui.phase = PHASE_CONNECTED;
            sp_lock();
            copy(ui.opponent_name, sizeof(ui.opponent_name), g_slippi.match.remote_name);
            copy(ui.opponent_code, sizeof(ui.opponent_code), g_slippi.match.remote_code);
            sp_unlock();
        }
        if (ui.locked && !sent) send_selections();
    }
}

int mp_platform_slippi_ui_css(int packed, int trigger, int held)
{
    int flags = 0;

    ckind = (packed >> 8) & 0xFF;
    color = (packed >> 16) & 0xFF;
    ui.ready = packed & 1;
    if (ui.page != PAGE_CODE) ui.page = PAGE_CSS;
    load_user();
    frame++;
    if (frame % 15 == 0) ui.spinner ^= 1;

    poll_code_entry();
    if (started) {
        slippi_net_poll();
        update_net();
    }
    chat_frame(trigger);

    if (ui.page == PAGE_CSS) {
        if (trigger & GC_START) pending_start = 1;
        if (trigger & GC_Z) pending_cancel = 1;
        if (ui.phase == PHASE_CONNECTED && (held & GC_Z)) {
            ui.hold_z++;
            if (ui.hold_z > HOLD_Z_FRAMES) {
                sp_log("Slippi UI: disconnected (Z held)");
                stop(PHASE_IDLE);
                sfx |= SLIPPI_UI_SFX_BACK;
            }
        } else {
            ui.hold_z = 0;
        }
    }

    if (pending_cancel) {
        pending_cancel = 0;
        if (ui.phase == PHASE_SEARCH) {
            sp_log("Slippi UI: search cancelled");
            stop(PHASE_IDLE);
            sfx |= SLIPPI_UI_SFX_BACK;
        } else if (ui.phase == PHASE_ERROR) {
            ui.phase = PHASE_IDLE;
            ui.error[0] = '\0';
            sfx |= SLIPPI_UI_SFX_BACK;
        }
    }

    if (pending_start) {
        pending_start = 0;
        if (!ui.ready) {
            sfx |= SLIPPI_UI_SFX_ERROR;
        } else if (ui.phase == PHASE_IDLE) {
            open_code_entry();
        } else if (ui.phase == PHASE_CONNECTED && !ui.locked) {
            if (ui.need_stage && !ui.chose_stage) {
                flags |= SLIPPI_UI_SSS;
            } else {
                lock_ckind = ckind;
                lock_color = color;
                ui.locked = 1;
                send_selections();
            }
            sfx |= SLIPPI_UI_SFX_FORWARD;
        }
    }

    if (ui.phase == PHASE_CONNECTED && ui.locked && sent && slippi_net_remote_ready()) {
        flags |= SLIPPI_UI_START;
    }
    if (ui.locked) flags |= SLIPPI_UI_LOCKED;
    /* The Zelda icon plays Sheik online, as in Slippi. */
    flags |= SLIPPI_UI_SHEIK;
    flags |= sfx;
    sfx = 0;
    return flags;
}

void mp_platform_slippi_ui_event(int event, int arg)
{
    if (event == SLIPPI_UI_EV_CSS_ENTER) {
        load_user();
        ui.page = PAGE_CSS;
    } else if (event == SLIPPI_UI_EV_LEAVE) {
        if (started) sp_log("Slippi UI: left the online CSS, connection closed");
        if (ime_open) {
            sceImeDialogAbort();
            sceImeDialogTerm();
            ime_open = 0;
        }
        stop(PHASE_IDLE);
        ui.page = PAGE_NONE;
    } else if (event == SLIPPI_UI_EV_STAGE) {
        if (arg >= 0) {
            stage = arg & 0xFFFF;
            alt = (arg >> 16) & 0xFF;
            ui.chose_stage = 1;
            lock_ckind = ckind;
            lock_color = color;
            ui.locked = 1;
            sent = 0;
        }
    } else if (event == SLIPPI_UI_EV_RESULT) {
        ui.need_stage = !arg;
        ui.chose_stage = 0;
        ui.locked = 0;
        sent = 0;
        games++;
    } else if (event == SLIPPI_UI_EV_MATCH) {
        ui.page = PAGE_MATCH;
    }
}

int mp_platform_slippi_ui_remote(void)
{
    unsigned char selections[13];
    int character;

    if (ui.phase != PHASE_CONNECTED) return -1;
    character = slippi_net_remote_character();
    if (character < 0) return -1;
    slippi_remote_selections(selections);
    return character | (selections[1] << 8);
}

int mp_platform_slippi_ui_alt(int toggle)
{
    if (toggle) ui.frozen_stadium ^= 1;
    return ui.frozen_stadium;
}

int mp_platform_slippi_ui_name(int local, char* out, int len)
{
    const char* name;

    out[0] = '\0';
    if (local) {
        name = ui.user_name[0] ? ui.user_name : ui.user_code;
    } else {
        name = ui.opponent_name[0] ? ui.opponent_name : ui.opponent_code;
    }
    copy(out, len, name);
    return out[0] != '\0';
}

/* ------------------------------------------------------- the CSS text */

/* Melee's SIS text takes letters, digits, space and . , - : ' " as ASCII,
 * and any other byte as the first of a Shift-JIS pair.  So other
 * punctuation becomes its full-width form ('#' is 0x8194), and anything
 * outside ASCII becomes a full-width '?'. */
static void sis(char* out, int len, const char* in)
{
    static const struct {
        char c;
        unsigned short sjis;
    } wide[] = {
        { '!', 0x8149 }, { '#', 0x8194 }, { '$', 0x8190 }, { '%', 0x8193 }, { '&', 0x8195 },
        { '(', 0x8169 }, { ')', 0x816a }, { '*', 0x8196 }, { '+', 0x817b }, { '/', 0x815e },
        { ';', 0x8147 }, { '<', 0x8183 }, { '=', 0x8181 }, { '>', 0x8184 }, { '?', 0x8148 },
        { '@', 0x8197 }, { '[', 0x816d }, { '\\', 0x815f }, { ']', 0x816e }, { '^', 0x814f },
        { '_', 0x8151 }, { '`', 0x814d }, { '{', 0x816f }, { '|', 0x8162 }, { '}', 0x8170 },
        { '~', 0x8160 },
    };
    int n = 0;
    unsigned i;

    for (; *in != '\0' && n + 3 < len; in++) {
        unsigned char c = (unsigned char) *in;
        unsigned short w = 0;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == ' ' || c == '.' || c == ',' || c == '-' || c == ':' || c == '\'' || c == '"')
        {
            out[n] = (char) c;
            n++;
            continue;
        }
        if (c >= 0x80) {
            /* One full-width '?' for a whole UTF-8 character. */
            while ((in[1] & 0xC0) == 0x80) in++;
            w = 0x8148;
        } else {
            for (i = 0; i < sizeof(wide) / sizeof(wide[0]); i++) {
                if (wide[i].c == (char) c) w = wide[i].sjis;
            }
        }
        if (w != 0) {
            out[n] = (char) (w >> 8);
            out[n + 1] = (char) w;
            n += 2;
        }
    }
    out[n] = '\0';
}

/* A check mark when done, else a turning spinner. */
static int spinner_glyph(char* out, int len, int done)
{
    unsigned short w;
    if (done) {
        w = 0x817C;
    } else if (ui.spinner) {
        w = 0x817E;
    } else {
        w = 0x817B;
    }
    if (len < 3) return TEXT_WAIT;
    out[0] = (char) (w >> 8);
    out[1] = (char) w;
    out[2] = '\0';
    return done ? TEXT_DONE : TEXT_WAIT;
}

/* Line index of the error, wrapped at about 30 characters like Slippi. */
static void error_line(int index, char* out, int len)
{
    const char* e = ui.error[0] ? ui.error : "Unknown error";
    char line[40];
    int k;

    for (k = 0; k <= index && *e != '\0'; k++) {
        int n = 0;
        int last_space = -1;
        while (e[n] != '\0' && n < 30) {
            if (e[n] == ' ') last_space = n;
            n++;
        }
        if (e[n] != '\0' && last_space > 0) n = last_space;
        snprintf(line, sizeof(line), "%.*s", n, e);
        e += n;
        while (*e == ' ') e++;
        if (k == index) {
            sis(out, len, line);
            return;
        }
    }
    out[0] = '\0';
}

/* The CSS lines, at the positions in slippi_online_mode.c:
 *   0 header       1 "User"      2 name       3 "Connect Code"   4 code
 *   5 line 1       6 spinner 1   7 line 2     8 spinner 2        9 line 3
 *  10 spinner 3   11 Z hint     12 chat hint 13 "Playing:"      14 opponent
 *  15-18 error lines            19-23 chat messages   24-28 open chat page */
int mp_platform_slippi_ui_text(int line, char* out, int len)
{
    static const char* const pages[4] = { "Up", "Left", "Right", "Down" };
    char b[96];
    int error = ui.phase == PHASE_ERROR;
    int connected = ui.phase == PHASE_CONNECTED;

    out[0] = '\0';
    switch (line) {
    case 0:
        sis(out, len, error ? "Error" : "Direct Mode");
        return TEXT_WHITE;
    case 1:
        sis(out, len, "User");
        return TEXT_GRAY;
    case 2:
        sis(out, len, ui.user_name);
        return TEXT_WHITE;
    case 3:
        sis(out, len, "Connect Code");
        return TEXT_GRAY;
    case 4:
        sis(out, len, ui.user_code);
        return TEXT_WHITE;
    case 5:
        if (!error) sis(out, len, ui.ready ? "Character selected" : "Select your character");
        return TEXT_WHITE;
    case 6:
        if (error) return TEXT_WHITE;
        return spinner_glyph(out, len, ui.ready);
    case 7:
        if (error) return TEXT_WHITE;
        if (ui.page == PAGE_CODE) {
            sis(out, len, "Enter the code on the keyboard");
        } else if (ui.locked) {
            sis(out, len, "Locked in");
        } else if (ui.phase == PHASE_IDLE) {
            sis(out, len, "Press START to enter code");
        } else if (connected && ui.need_stage && !ui.chose_stage) {
            sis(out, len, "Press START to select stage");
        } else if (connected) {
            sis(out, len, "Press START to lock in");
        }
        return TEXT_WHITE;
    case 8:
        if (error) return TEXT_WHITE;
        if (ui.phase == PHASE_IDLE && ui.page != PAGE_CODE && !ui.locked) {
            return spinner_glyph(out, len, 0);
        }
        return spinner_glyph(out, len, ui.locked);
    case 9:
        if (ui.phase == PHASE_SEARCH) {
            if (ui.net_status == SLIPPI_NET_CONNECTING) {
                snprintf(b, sizeof(b), "Connecting to %s", ui.target);
            } else {
                snprintf(b, sizeof(b), "Searching for %s", ui.target);
            }
            sis(out, len, b);
        } else if (connected) {
            sis(out, len, "Waiting on opponent");
        }
        return TEXT_WHITE;
    case 10:
        if (ui.phase == PHASE_SEARCH || connected) return spinner_glyph(out, len, 0);
        return TEXT_WHITE;
    case 11:
        /* The open chat page uses this space. */
        if (connected && ui.chat_page >= 0) return TEXT_GRAY;
        if (ui.phase == PHASE_SEARCH) {
            sis(out, len, "Press Z to cancel");
        } else if (error) {
            sis(out, len, "Press Z to clear error");
        } else if (connected) {
            sis(out, len, "Hold Z to disconnect");
        }
        return TEXT_GRAY;
    case 12:
        if (connected && ui.chat_enabled && ui.chat_page < 0) sis(out, len, "Use D-Pad to Chat");
        return TEXT_GRAY;
    case 13:
        if (connected) sis(out, len, "Playing:");
        return TEXT_GRAY;
    case 14:
        if (connected) sis(out, len, ui.opponent_name[0] ? ui.opponent_name : ui.opponent_code);
        return TEXT_WHITE;
    case 15:
    case 16:
    case 17:
    case 18:
        if (error) error_line(line - 15, out, len);
        return TEXT_RED;
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
        if (error || !connected || ui.chat_page >= 0) return TEXT_WHITE;
        if (line - 19 < ui.chat_count) {
            sis(out, len, ui.chat_text[line - 19]);
            return ui.chat_port[line - 19] ? TEXT_CHAT_P2 : TEXT_CHAT_P1;
        }
        return TEXT_WHITE;
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
        if (error || !connected || ui.chat_page < 0) return TEXT_WHITE;
        if (line == 24) {
            snprintf(b, sizeof(b), "Page: %s", pages[ui.chat_page]);
            sis(out, len, b);
            return TEXT_WHITE;
        }
        snprintf(b, sizeof(b), "%s: %s", pages[line - 25],
                 chat_message(0, ui.chat_page * 4 + line - 25));
        sis(out, len, b);
        return TEXT_GRAY;
    }
    return TEXT_WHITE;
}
