#include "mnvitadebug.h"

#include "debug_menu_sequence.h"

#include <dolphin/pad.h>
#include <melee/db/db.h>

#ifdef TARGET_VITA
static MeleeVitaDebugMenuSequence activation;
static bool debug_route_active;
static bool previous_debug_enabled;
static DbLKind previous_debug_level;

_Static_assert(MELEE_VITA_DEBUG_LEFT == PAD_BUTTON_LEFT, "left input mismatch");
_Static_assert(MELEE_VITA_DEBUG_RIGHT == PAD_BUTTON_RIGHT,
               "right input mismatch");
_Static_assert(MELEE_VITA_DEBUG_DOWN == PAD_BUTTON_DOWN, "down input mismatch");
_Static_assert(MELEE_VITA_DEBUG_UP == PAD_BUTTON_UP, "up input mismatch");
_Static_assert(MELEE_VITA_DEBUG_SELECT == PAD_TRIGGER_Z,
               "Select/Z input mismatch");

bool mnVitaDebug_FeedActivation(u64 triggers)
{
    return melee_vita_debug_menu_feed(&activation, triggers);
}

bool mnVitaDebug_IsUnlocked(void)
{
    return activation.unlocked;
}

bool mnVitaDebug_IsActive(void)
{
    return debug_route_active;
}

void mnVitaDebug_Begin(void)
{
    if (!debug_route_active) {
        previous_debug_enabled = db_804D6B20;
        previous_debug_level = DbLevel;
    }
    debug_route_active = true;
    db_804D6B20 = true;
    DbLevel = DbLKind_DebugRom;
}

void mnVitaDebug_End(void)
{
    if (debug_route_active) {
        debug_route_active = false;
        db_804D6B20 = previous_debug_enabled;
        DbLevel = previous_debug_level;
    }
}
#else
bool mnVitaDebug_FeedActivation(u64 triggers)
{
    return false;
}

bool mnVitaDebug_IsUnlocked(void)
{
    return false;
}

bool mnVitaDebug_IsActive(void)
{
    return false;
}

void mnVitaDebug_Begin(void) {}

void mnVitaDebug_End(void) {}
#endif
