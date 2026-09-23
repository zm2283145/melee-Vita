#ifndef MELEE_FT_GIGAKOOPA_COSTUME_METADATA_H
#define MELEE_FT_GIGAKOOPA_COSTUME_METADATA_H

#include <stdbool.h>
#include <stdint.h>

/* PlGk.dat has one shared parts/texture-animation metadata entry even when
 * the fighter uses a separate costume archive for its visible model. */
static inline uint8_t ftGk_MetadataCostumeId(bool is_giga_koopa,
                                            uint8_t selected_costume)
{
    return is_giga_koopa ? 0 : selected_costume;
}

#endif
