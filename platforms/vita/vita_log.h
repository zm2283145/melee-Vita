#pragma once

int melee_vita_log_start(void);
void melee_vita_log_info(const char* format, ...);
int melee_vita_debugger_wait(void);
void melee_vita_log_stop(void);
