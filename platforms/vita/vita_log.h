#pragma once

#ifdef MELEE_VITA_RELEASE
static inline int melee_vita_log_start(void) { return 0; }
static inline void melee_vita_log_info(const char* format, ...) { (void) format; }
static inline int melee_vita_debugger_wait(void) { return -1; }
static inline void melee_vita_log_stop(void) {}
#else
int melee_vita_log_start(void);
void melee_vita_log_info(const char* format, ...);
int melee_vita_debugger_wait(void);
void melee_vita_log_stop(void);
#endif
