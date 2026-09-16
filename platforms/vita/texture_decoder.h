#pragma once

#include <stdint.h>

int melee_vita_decode_texture(const unsigned char* image_desc,
                              const unsigned char* archive_data,
                              uint32_t archive_size, uint32_t* rgba,
                              uint32_t pixel_capacity, uint32_t* width,
                              uint32_t* height, uint32_t* format);

int melee_vita_decode_texture_raw(const unsigned char* source,
                                  uint32_t width, uint32_t height,
                                  uint32_t format,
                                  const unsigned char* palette,
                                  uint32_t palette_format,
                                  uint32_t palette_entries,
                                  uint32_t* rgba,
                                  uint32_t pixel_capacity);
