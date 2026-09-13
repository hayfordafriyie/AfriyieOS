// SPDX-License-Identifier: MIT
// AfriyieOS — boot splash

#ifndef AFRIYIE_SPLASH_H
#define AFRIYIE_SPLASH_H

#include "types.h"
#include "fb.h"

// Draws the AfriyieOS boot screen using only raw primitives (gradients,
// rectangles and the bitmap font). It proves the whole graphics path works
// before any rendering library is introduced in v0.7.
//
// The layout adapts to the display: a portrait (phone-shaped) framebuffer gets
// the stacked phone layout, a landscape one gets the desktop layout.
void af_splash_draw(void);

// Reports which layout the splash selected — handy for the log and for the
// responsive-layout work that begins in v0.8.
bool af_splash_used_phone_layout(void);

#endif // AFRIYIE_SPLASH_H
