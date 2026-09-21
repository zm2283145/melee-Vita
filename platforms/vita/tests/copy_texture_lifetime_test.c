#include <assert.h>
#include <stdint.h>

#include "copy_texture_lifetime.h"

static void test_retained_copy_has_no_frame_expiry(void)
{
    MeleeVitaCopyTextureBinding portrait = { 0 };
    const void* destination = (const void*) (uintptr_t) 0x1000;
    uint32_t frame = 10;

    melee_vita_copy_texture_bind(&portrait, destination, 7);
    frame += 121;

    assert(frame > 120);
    assert(melee_vita_copy_texture_binding_matches(
        &portrait, destination, 7));
}

static void test_reused_address_invalidates_old_generation(void)
{
    MeleeVitaCopyTextureBinding portrait = { 0 };
    const void* destination = (const void*) (uintptr_t) 0x1000;

    melee_vita_copy_texture_bind(&portrait, destination, 7);
    assert(!melee_vita_copy_texture_binding_matches(
        &portrait, destination, 8));

    melee_vita_copy_texture_bind(&portrait, destination, 8);
    assert(melee_vita_copy_texture_binding_matches(
        &portrait, destination, 8));
}

static void test_independent_copy_destinations_do_not_alias(void)
{
    MeleeVitaCopyTextureBinding portrait = { 0 };
    MeleeVitaCopyTextureBinding background = { 0 };
    const void* portrait_destination =
        (const void*) (uintptr_t) 0x1000;
    const void* background_destination =
        (const void*) (uintptr_t) 0x2000;

    melee_vita_copy_texture_bind(&portrait, portrait_destination, 7);
    melee_vita_copy_texture_bind(&background, background_destination, 9);

    assert(melee_vita_copy_texture_binding_matches(
        &portrait, portrait_destination, 7));
    assert(melee_vita_copy_texture_binding_matches(
        &background, background_destination, 9));
    assert(!melee_vita_copy_texture_binding_matches(
        &portrait, background_destination, 9));
}

int main(void)
{
    test_retained_copy_has_no_frame_expiry();
    test_reused_address_invalidates_old_generation();
    test_independent_copy_destinations_do_not_alias();
    return 0;
}
