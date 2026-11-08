// main/langstudy_buddy_sprite.h
// 24x24 palette-index art for the digital buddy.
//
// Index 0 is transparent. Every other index has a per-species colour, so the
// UI can paint the sprite into a small canvas without an image decoder.
#pragma once

#include "langstudy_buddy.h"

#include <stdint.h>

#define LS_BUDDY_SPRITE_W 24
#define LS_BUDDY_SPRITE_H 24
#define LS_BUDDY_PALETTE_COUNT 8

// Returns LS_BUDDY_SPRITE_W * LS_BUDDY_SPRITE_H palette indices, row-major.
const uint8_t *ls_buddy_sprite(uint8_t species, uint8_t stage);

// 0xRRGGBB for a palette index; index 0 (transparent) returns 0.
uint32_t ls_buddy_palette_color(uint8_t species, uint8_t index);
