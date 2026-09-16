/* SPDX-License-Identifier: GPL-3.0-or-later */
/* 32-byte aligned Dolphin arena heap implementation for Vita. */
#include <dolphin/os.h>

#include <stdint.h>
#include <string.h>

#define HEAP_ALIGNMENT 32u
#define HEAP_CELL_HEADER 32u
#define HEAP_MIN_CELL 64u

typedef struct HeapDesc HeapDesc;

typedef struct HeapCell {
    struct HeapCell* prev;
    struct HeapCell* next;
    s32 size;
    HeapDesc* owner;
    u8 padding[16];
} HeapCell;

struct HeapDesc {
    s32 size;
    HeapCell* free_list;
    HeapCell* allocated;
};

_Static_assert(sizeof(HeapCell) == HEAP_CELL_HEADER, "heap cell ABI");

volatile OSHeapHandle __OSCurrHeap = -1;
static HeapDesc* s_heaps;
static s32 s_heap_count;
static u8* s_heap_arena_start;
static u8* s_heap_arena_end;

static uintptr_t round_up(uintptr_t value)
{
    return (value + HEAP_ALIGNMENT - 1u) & ~(uintptr_t) (HEAP_ALIGNMENT - 1u);
}

static uintptr_t round_down(uintptr_t value)
{
    return value & ~(uintptr_t) (HEAP_ALIGNMENT - 1u);
}

static BOOL valid_heap(OSHeapHandle heap)
{
    return s_heaps != NULL && heap >= 0 && heap < s_heap_count &&
           s_heaps[heap].size >= 0;
}

static HeapCell* list_remove(HeapCell* head, HeapCell* cell)
{
    if (cell->prev != NULL) cell->prev->next = cell->next;
    else head = cell->next;
    if (cell->next != NULL) cell->next->prev = cell->prev;
    return head;
}

static HeapCell* list_push(HeapCell* head, HeapCell* cell)
{
    cell->prev = NULL;
    cell->next = head;
    if (head != NULL) head->prev = cell;
    return cell;
}

static HeapCell* insert_free(HeapCell* head, HeapCell* cell)
{
    HeapCell* previous = NULL;
    HeapCell* next = head;
    cell->owner = NULL;
    while (next != NULL && next < cell) {
        previous = next;
        next = next->next;
    }
    cell->prev = previous;
    cell->next = next;
    if (previous != NULL) previous->next = cell;
    else head = cell;
    if (next != NULL) next->prev = cell;

    if (cell->next != NULL && (u8*) cell + cell->size == (u8*) cell->next) {
        HeapCell* right = cell->next;
        cell->size += right->size;
        cell->next = right->next;
        if (cell->next != NULL) cell->next->prev = cell;
    }
    if (cell->prev != NULL &&
        (u8*) cell->prev + cell->prev->size == (u8*) cell) {
        HeapCell* left = cell->prev;
        left->size += cell->size;
        left->next = cell->next;
        if (left->next != NULL) left->next->prev = left;
    }
    return head;
}

void* OSInitAlloc(void* arena_start, void* arena_end, int max_heaps)
{
    uintptr_t descriptor_end;
    s32 i;
    if (arena_start == NULL || arena_end == NULL || max_heaps <= 0) return NULL;
    s_heaps = arena_start;
    s_heap_count = max_heaps;
    for (i = 0; i < s_heap_count; ++i) {
        s_heaps[i].size = -1;
        s_heaps[i].free_list = NULL;
        s_heaps[i].allocated = NULL;
    }
    descriptor_end = round_up((uintptr_t) arena_start +
                              sizeof(HeapDesc) * (uintptr_t) max_heaps);
    s_heap_arena_start = (u8*) descriptor_end;
    s_heap_arena_end = (u8*) round_down((uintptr_t) arena_end);
    __OSCurrHeap = -1;
    return s_heap_arena_start < s_heap_arena_end ? s_heap_arena_start : NULL;
}

OSHeapHandle OSCreateHeap(void* start, void* end)
{
    uintptr_t first = round_up((uintptr_t) start);
    uintptr_t last = round_down((uintptr_t) end);
    OSHeapHandle heap;
    if (first < (uintptr_t) s_heap_arena_start ||
        last > (uintptr_t) s_heap_arena_end || last - first < HEAP_MIN_CELL)
        return -1;
    for (heap = 0; heap < s_heap_count; ++heap) {
        HeapCell* cell;
        if (s_heaps[heap].size >= 0) continue;
        cell = (HeapCell*) first;
        memset(cell, 0, sizeof(*cell));
        cell->size = (s32) (last - first);
        s_heaps[heap].size = cell->size;
        s_heaps[heap].free_list = cell;
        s_heaps[heap].allocated = NULL;
        return heap;
    }
    return -1;
}

void OSDestroyHeap(OSHeapHandle heap)
{
    if (!valid_heap(heap)) return;
    s_heaps[heap].size = -1;
    s_heaps[heap].free_list = NULL;
    s_heaps[heap].allocated = NULL;
    if (__OSCurrHeap == heap) __OSCurrHeap = -1;
}

void* OSAllocFromHeap(OSHeapHandle heap, u32 size)
{
    HeapDesc* descriptor;
    HeapCell* cell;
    u32 required;
    if (!valid_heap(heap) || size == 0) return NULL;
    descriptor = &s_heaps[heap];
    required = (u32) round_up(size + HEAP_CELL_HEADER);
    for (cell = descriptor->free_list; cell != NULL; cell = cell->next)
        if ((u32) cell->size >= required) break;
    if (cell == NULL) return NULL;

    descriptor->free_list = list_remove(descriptor->free_list, cell);
    if ((u32) cell->size >= required + HEAP_MIN_CELL) {
        HeapCell* remainder = (HeapCell*) ((u8*) cell + required);
        memset(remainder, 0, sizeof(*remainder));
        remainder->size = cell->size - (s32) required;
        cell->size = (s32) required;
        descriptor->free_list = insert_free(descriptor->free_list, remainder);
    }
    cell->owner = descriptor;
    descriptor->allocated = list_push(descriptor->allocated, cell);
    return (u8*) cell + HEAP_CELL_HEADER;
}

void OSFreeToHeap(OSHeapHandle heap, void* pointer)
{
    HeapDesc* descriptor;
    HeapCell* cell;
    if (!valid_heap(heap) || pointer == NULL) return;
    descriptor = &s_heaps[heap];
    cell = (HeapCell*) ((u8*) pointer - HEAP_CELL_HEADER);
    if (cell->owner != descriptor) return;
    descriptor->allocated = list_remove(descriptor->allocated, cell);
    descriptor->free_list = insert_free(descriptor->free_list, cell);
}

OSHeapHandle OSSetCurrentHeap(OSHeapHandle heap)
{
    OSHeapHandle previous = __OSCurrHeap;
    if (heap == -1 || valid_heap(heap)) __OSCurrHeap = heap;
    return previous;
}

s32 OSCheckHeap(OSHeapHandle heap)
{
    HeapCell* cell;
    s32 free_bytes = 0;
    if (!valid_heap(heap)) return -1;
    for (cell = s_heaps[heap].free_list; cell != NULL; cell = cell->next) {
        if (cell->size < (s32) HEAP_MIN_CELL || cell->owner != NULL) return -1;
        free_bytes += cell->size - (s32) HEAP_CELL_HEADER;
    }
    return free_bytes;
}

u32 OSReferentSize(void* pointer)
{
    HeapCell* cell;
    if (pointer == NULL) return 0;
    cell = (HeapCell*) ((u8*) pointer - HEAP_CELL_HEADER);
    return cell->owner != NULL ? (u32) cell->size - HEAP_CELL_HEADER : 0;
}

void OSAddToHeap(OSHeapHandle heap, void* start, void* end)
{
    HeapCell* cell;
    uintptr_t first = round_up((uintptr_t) start);
    uintptr_t last = round_down((uintptr_t) end);
    if (!valid_heap(heap) || last - first < HEAP_MIN_CELL) return;
    cell = (HeapCell*) first;
    memset(cell, 0, sizeof(*cell));
    cell->size = (s32) (last - first);
    s_heaps[heap].size += cell->size;
    s_heaps[heap].free_list = insert_free(s_heaps[heap].free_list, cell);
}

void* OSAllocFixed(void* start, void* end)
{
    (void) start;
    (void) end;
    return NULL;
}

void OSDumpHeap(OSHeapHandle heap) { (void) heap; }

void OSVisitAllocated(void (*visitor)(void*, u32))
{
    s32 heap;
    if (visitor == NULL) return;
    for (heap = 0; heap < s_heap_count; ++heap) {
        HeapCell* cell;
        if (!valid_heap(heap)) continue;
        for (cell = s_heaps[heap].allocated; cell != NULL; cell = cell->next)
            visitor((u8*) cell + HEAP_CELL_HEADER,
                    (u32) cell->size - HEAP_CELL_HEADER);
    }
}
