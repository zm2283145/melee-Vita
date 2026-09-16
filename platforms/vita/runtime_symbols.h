#pragma once

#include <stddef.h>

#define MELEE_VITA_RUNTIME_SYMBOL_CAPACITY 128u
#define MELEE_VITA_RUNTIME_SYMBOL_NAME_MAX 96u

void melee_vita_runtime_symbols_reset(void);
int melee_vita_runtime_symbol_register(const char* name, void* address,
                                       const char* owner);
void* melee_vita_runtime_symbol_find(const char* name, const char** owner);
size_t melee_vita_runtime_symbol_count(void);
