// SPDX-License-Identifier: MIT
// AfriyieOS — bitmap font interface (v0.1)

#ifndef AFRIYIE_FONT_H
#define AFRIYIE_FONT_H

#include "types.h"

#define AF_FONT8X16_WIDTH        8
#define AF_FONT8X16_HEIGHT       16
#define AF_FONT8X16_FIRST_CHAR   32    // ' '
#define AF_FONT8X16_LAST_CHAR    126   // '~'
#define AF_FONT8X16_GLYPH_COUNT  (AF_FONT8X16_LAST_CHAR - AF_FONT8X16_FIRST_CHAR + 1)

// One bit per pixel. Bit 7 of each row byte is the leftmost pixel.
extern const af_u8 af_font8x16[AF_FONT8X16_GLYPH_COUNT][AF_FONT8X16_HEIGHT];

// Returns the glyph bitmap for `c`, substituting '?' for anything outside the
// printable ASCII range. Never returns NULL.
const af_u8 *af_font8x16_glyph(char c);

#endif // AFRIYIE_FONT_H
