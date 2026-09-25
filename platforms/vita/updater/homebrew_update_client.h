#ifndef HOMEBREW_UPDATE_CLIENT_H
#define HOMEBREW_UPDATE_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum HomebrewUpdateStatus {
    HOMEBREW_UPDATE_NOT_DETECTED = -1,
    HOMEBREW_UPDATE_DISABLED = 0,
    HOMEBREW_UPDATE_HOOK_ERROR = 1,
    HOMEBREW_UPDATE_READY = 2
} HomebrewUpdateStatus;

/* The calling process must initialize SceNet before calling this function. */
int HomebrewUpdateClientGetStatus(void);

/*
 * Query status and identify the caller to a title-scoped plugin backend.
 * Unknown titles receive the normal status response without arming hooks.
 */
int HomebrewUpdateClientGetStatusForTitle(const char *title_id);

/* Launch the shared installer when this title has a staged update.
 * Returns 1 after a successful handoff, 0 when no matching update is pending,
 * and a negative value when the pending record or helper launch failed. */
int HomebrewUpdateClientLaunchPendingInstaller(const char *title_id);

#ifdef __cplusplus
}
#endif

#endif
