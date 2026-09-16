#include <stdint.h>

struct __attribute__((scalar_storage_order("big-endian"))) big_endian_probe {
    uint16_t half;
    uint32_t word;
};

_Static_assert(sizeof(struct big_endian_probe) == 8,
               "unexpected Vita structure layout");

int melee_vita_big_endian_storage_order_works(void)
{
    struct big_endian_probe value = {
        .half = 0x1234u,
        .word = 0x89abcdefu,
    };
    const unsigned char* bytes = (const unsigned char*) &value;
    return bytes[0] == 0x12u && bytes[1] == 0x34u &&
           bytes[4] == 0x89u && bytes[5] == 0xabu &&
           bytes[6] == 0xcdu && bytes[7] == 0xefu;
}
