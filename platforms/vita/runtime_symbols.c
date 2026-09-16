#include "runtime_symbols.h"

#include <string.h>

struct runtime_symbol {
    char name[MELEE_VITA_RUNTIME_SYMBOL_NAME_MAX];
    char owner[MELEE_VITA_RUNTIME_SYMBOL_NAME_MAX];
    void* address;
};

static struct runtime_symbol symbols[MELEE_VITA_RUNTIME_SYMBOL_CAPACITY];
static size_t symbol_count;

void melee_vita_runtime_symbols_reset(void)
{
    memset(symbols, 0, sizeof(symbols));
    symbol_count = 0;
}

int melee_vita_runtime_symbol_register(const char* name, void* address,
                                       const char* owner)
{
    if (name == NULL || name[0] == '\0' || address == NULL || owner == NULL ||
        strlen(name) >= MELEE_VITA_RUNTIME_SYMBOL_NAME_MAX ||
        strlen(owner) >= MELEE_VITA_RUNTIME_SYMBOL_NAME_MAX) return -1;
    for (size_t i = 0; i < symbol_count; ++i) {
        if (strcmp(symbols[i].name, name) == 0) {
            symbols[i].address = address;
            strcpy(symbols[i].owner, owner);
            return 0;
        }
    }
    if (symbol_count == MELEE_VITA_RUNTIME_SYMBOL_CAPACITY) return -2;
    strcpy(symbols[symbol_count].name, name);
    strcpy(symbols[symbol_count].owner, owner);
    symbols[symbol_count].address = address;
    ++symbol_count;
    return 0;
}

void* melee_vita_runtime_symbol_find(const char* name, const char** owner)
{
    if (name == NULL) return NULL;
    for (size_t i = 0; i < symbol_count; ++i) {
        if (strcmp(symbols[i].name, name) == 0) {
            if (owner != NULL) *owner = symbols[i].owner;
            return symbols[i].address;
        }
    }
    return NULL;
}

size_t melee_vita_runtime_symbol_count(void)
{
    return symbol_count;
}
