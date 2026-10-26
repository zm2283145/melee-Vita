/* Slippi gameplay codes, as C (port/engine/slippi/rules.c).
 *
 * Slippi Dolphin always runs its "Required" Gecko code sections (General
 * Codes, Slippi Recording, Slippi Online). The ones that change the
 * simulation are reproduced here and at their call sites
 * (tools/slippi_edits/online_rules.py in the 3DS port; docs/slippi/rules.md
 * there lists them).  Copied from the 3DS Slippi port (melee-3ds
 * slippi-beta-v0.1.0, native-3ds/port/include/slippi_rules.h). */
#ifndef MP_SLIPPI_RULES_H
#define MP_SLIPPI_RULES_H

#ifdef MELEE_VITA_SLIPPI
/* Master switch (default 0 on the Vita, so offline play stays vanilla) and
 * one bit per code (default all set), so a code can be compared with and
 * without. Poke them from a debugger. */
extern volatile unsigned mp_slippi_rules_enabled;
extern volatile unsigned mp_slippi_rules_mask;
#endif

#define MP_SR_UCF            (1u << 0)  /* UCF 0.84 (port/engine/ucf.c) */
#define MP_SR_NEUTRAL_SPAWN  (1u << 1)
#define MP_SR_FREEZE_GLITCH  (1u << 2)  /* Prevent freeze glitch (tauKhan) */
#define MP_SR_INIT_PLAYER    (1u << 3)  /* Initialize Player Data, 80068eec */
#define MP_SR_INIT_STAGE     (1u << 4)  /* Initialize Stage Data, 801c154c */
#define MP_SR_NANA_DET       (1u << 5)  /* NanaDeterminism, 800ac5b8 */
#define MP_SR_OFFSCREEN      (1u << 6)  /* BrawlOffscreenDamage, 8006a880 */
#define MP_SR_DEAD_UP_FALL   (1u << 7)  /* FreezeDeadUpFallPhysics */
#define MP_SR_WHISPY         (1u << 8)  /* WhispyBlowDirFix, 8008653c */
#define MP_SR_FD_BG          (1u << 9)  /* DesyncProofBGTransformations */
#define MP_SR_WOBBLE         (1u << 10) /* PreventWobbling */
#define MP_SR_PS_MONITOR     (1u << 11) /* PSCameraIndependentMonitor */
#define MP_SR_PS_FROZEN      (1u << 12) /* IngameCheckIfFrozen */
#define MP_SR_PS_LOAD        (1u << 13) /* StadiumFileLoad + GrPsxIsValid */
#define MP_SR_PS_LIVE_VIEW   (1u << 14) /* keep vanilla monitor modes 7/8 */
#define MP_SR_DYNAMICS       (1u << 15) /* FastForward/DynamicsFix, 8009e090 */
#define MP_SR_LGL            (1u << 16) /* LGLExceededGameEnd (online only) */
#define MP_SR_ONLINE_INIT    (1u << 17) /* InitOnlinePlay's held-A clear */

#ifdef MELEE_VITA_SLIPPI
static inline int mp_slippi_rule(unsigned bit)
{
    return mp_slippi_rules_enabled && (mp_slippi_rules_mask & bit) != 0;
}

/* ---- per-match settings, set by the netplay coordinator ---- */
/* Turns all the codes on (replay test, online match) or off (offline). */
void mp_slippi_rules_set_enabled(int enabled);
/* Frozen Pokemon Stadium (alt_stage_mode / the SSS toggle byte that
 * IngameCheckIfFrozen reads). Kept until changed; 0 at boot. */
void mp_slippi_rules_set_frozen_stadium(int frozen);
int mp_slippi_rules_frozen_stadium(void);
/* Final Destination's background never transforms (Slippi 3.14 replaces
 * the call at 8021aae4 with a nop; newer Slippi calls it with the RNG seed
 * saved, MP_SR_FD_BG).  Kept until changed; 0 at boot. */
void mp_slippi_rules_set_frozen_fd(int frozen);
int mp_slippi_rules_frozen_fd(void);
/* An online match (scene 0x0208 on console): online mode (0 ranked,
 * 1 unranked, 2 direct, 3 teams, 4 party) and this machine's player index.
 * mode < 0 clears it (offline, replays). */
void mp_slippi_rules_set_online(int mode, int local_player);
/* gmvs match start (8016e748): InitOnlinePlay's per-match work. */
void mp_slippi_rules_match_start(void);

/* ---- hooks (see online_rules.py for the sites) ---- */
struct Fighter;
struct HSD_GObj;
void mp_slippi_neutral_spawn(int slot, int is_teams);
int mp_slippi_offscreen_zone(struct Fighter* fp);
void mp_slippi_wobble_reset(struct Fighter* fp);
int mp_slippi_wobble_check(struct HSD_GObj* gobj);
void mp_slippi_dead_up_init(struct Fighter* fp, float vel_y, float vel_z);
void mp_slippi_dead_up_fall(struct Fighter* fp, float gravity, float terminal);
int mp_slippi_ps_monitor_ok(int slot, int vanilla);
int mp_slippi_ps_archive_valid(void);
void mp_slippi_dynamics_fix(struct Fighter* fp);
int mp_slippi_lgl_timeout_message(void);
void mp_slippi_clear_image(void* image_desc);

#else
/* Builds without -EnableSlippi: every code is off and each hook does what
 * the game did before, so the calls in the game code compile away. */
struct Fighter;
struct HSD_GObj;
#define mp_slippi_rules_enabled 0
static inline int mp_slippi_rule(unsigned bit)
{
    (void) bit;
    return 0;
}
static inline void mp_slippi_rules_set_enabled(int enabled) { (void) enabled; }
static inline void mp_slippi_rules_set_frozen_stadium(int frozen) { (void) frozen; }
static inline int mp_slippi_rules_frozen_stadium(void) { return 0; }
static inline void mp_slippi_rules_set_frozen_fd(int frozen) { (void) frozen; }
static inline int mp_slippi_rules_frozen_fd(void) { return 0; }
static inline void mp_slippi_rules_set_online(int mode, int local_player)
{
    (void) mode;
    (void) local_player;
}
static inline void mp_slippi_rules_match_start(void) {}
static inline void mp_slippi_neutral_spawn(int slot, int is_teams)
{
    (void) slot;
    (void) is_teams;
}
static inline int mp_slippi_offscreen_zone(struct Fighter* fp)
{
    (void) fp;
    return 0;
}
static inline void mp_slippi_wobble_reset(struct Fighter* fp) { (void) fp; }
static inline int mp_slippi_wobble_check(struct HSD_GObj* gobj)
{
    (void) gobj;
    return 0;
}
static inline void mp_slippi_dead_up_init(struct Fighter* fp, float vel_y, float vel_z)
{
    (void) fp;
    (void) vel_y;
    (void) vel_z;
}
static inline void mp_slippi_dead_up_fall(struct Fighter* fp, float gravity, float terminal)
{
    (void) fp;
    (void) gravity;
    (void) terminal;
}
/* The vanilla monitor test decides. */
static inline int mp_slippi_ps_monitor_ok(int slot, int vanilla)
{
    (void) slot;
    return vanilla;
}
static inline int mp_slippi_ps_archive_valid(void) { return 0; }
static inline void mp_slippi_dynamics_fix(struct Fighter* fp) { (void) fp; }
static inline int mp_slippi_lgl_timeout_message(void) { return 0; }
static inline void mp_slippi_clear_image(void* image_desc) { (void) image_desc; }
#endif

#endif
