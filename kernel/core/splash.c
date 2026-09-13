// SPDX-License-Identifier: MIT
// AfriyieOS — the boot splash screen
//
// Drawn entirely with the v0.1 primitives: a vertical gradient background, a
// rounded panel, the wordmark, a progress bar and a footer. If this renders
// correctly, the framebuffer, the stride handling, the colour conversion and
// the font are all known to be working.
//
// It also implements the first version of the responsive rule that the full
// Shell will use from v0.8: aspect ratio decides the layout.

#include "afriyie/splash.h"
#include "afriyie/config.h"
#include "afriyie/font.h"
#include "afriyie/hal.h"
#include "afriyie/kstring.h"
#include "afriyie/log.h"

static bool s_phone_layout = false;

// -----------------------------------------------------------------------------
// A simple, fast approximation of a rounded rectangle.
//
// The corners are drawn as quarter circles by testing each pixel against the
// corner centre. Deliberately not anti-aliased: v0.1 is about correctness and
// getting something on screen, and the vector renderer arrives in v0.7 anyway.
// -----------------------------------------------------------------------------
static void fill_rounded_rect(af_i32 x, af_i32 y, af_i32 w, af_i32 h,
                              af_i32 radius, af_color_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    if (radius <= 0 || radius * 2 > w || radius * 2 > h) {
        af_fb_fill_rect(x, y, w, h, color);
        return;
    }

    af_i32 r2 = radius * radius;

    // Top and bottom bands without the corners.
    af_fb_fill_rect(x + radius, y, w - radius * 2, radius, color);
    af_fb_fill_rect(x + radius, y + h - radius, w - radius * 2, radius, color);

    // Middle band.
    af_fb_fill_rect(x, y + radius, w, h - radius * 2, color);

    // Four corner quarter-discs.
    for (af_i32 dy = 0; dy < radius; dy++) {
        af_i32 span = 0;
        for (af_i32 dx = 0; dx < radius; dx++) {
            af_i32 ddx = radius - 1 - dx;
            af_i32 ddy = radius - 1 - dy;
            if (ddx * ddx + ddy * ddy <= r2) {
                span = dx + 1;
                break;
            }
        }
        if (span == 0) {
            continue;
        }

        af_fb_fill_rect(x + radius - span, y + dy, span, 1, color);                 // TL
        af_fb_fill_rect(x + w - radius, y + dy, span, 1, color);                    // TR
        af_fb_fill_rect(x + radius - span, y + h - 1 - dy, span, 1, color);         // BL
        af_fb_fill_rect(x + w - radius, y + h - 1 - dy, span, 1, color);            // BR
    }
}

// Soft drop shadow: progressively more transparent copies of the panel, offset
// downward. Cheap, and it reads convincingly at a glance without any blur pass.
static void draw_shadow(af_i32 x, af_i32 y, af_i32 w, af_i32 h,
                        af_i32 radius, af_u8 base_alpha)
{
    for (af_i32 i = 6; i >= 1; i--) {
        af_u8 alpha = (af_u8)((base_alpha * (7 - i)) / 7);
        af_fb_blend_rect(x - i, y + i, w + i * 2, h + i * 2,
                         AF_COLOR_BLACK, alpha);
        AF_UNUSED(radius);
    }
}

// A horizontal progress bar, drawn as a rounded track with a rounded fill.
static void draw_progress_bar(af_i32 x, af_i32 y, af_i32 w, af_i32 h,
                              af_color_t track, af_color_t fill,
                              af_u32 percent)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    if (percent > 100) {
        percent = 100;
    }

    fill_rounded_rect(x, y, w, h, h / 2, track);

    af_i32 filled = (w * (af_i32)percent) / 100;
    if (filled >= h) {
        fill_rounded_rect(x, y, filled, h, h / 2, fill);
    }
}

// Draws text horizontally centred inside [x, x+w).
static void draw_text_centered(af_i32 x, af_i32 w, af_i32 y, const char *text,
                               af_u32 scale, af_color_t fg, af_color_t bg)
{
    af_u32 text_w = 0;
    af_u32 text_h = 0;
    af_fb_measure_text(text, scale, &text_w, &text_h);
    AF_UNUSED(text_h);

    af_i32 tx = x + (w - (af_i32)text_w) / 2;
    af_fb_draw_text_scaled(tx, y, text, scale, fg, bg);
}

// -----------------------------------------------------------------------------
// The AfriyieOS mark: a stylised "A" built from a triangle and a crossbar,
// which scales cleanly and needs no image asset.
// -----------------------------------------------------------------------------
static void draw_logo_mark(af_i32 cx, af_i32 cy, af_i32 size, af_color_t primary,
                           af_color_t accent)
{
    af_i32 half = size / 2;

    // Triangle outline, drawn as a stack of horizontal spans.
    for (af_i32 row = 0; row < size; row++) {
        af_i32 t = row;
        af_i32 half_span = (half * t) / size;
        af_i32 left  = cx - half_span;
        af_i32 right = cx + half_span;

        // Two-pixel thick strokes give the mark visual weight at small sizes.
        af_fb_fill_rect(left, cy - half + row, 2, 1, primary);
        af_fb_fill_rect(right - 1, cy - half + row, 2, 1, primary);
    }

    // Baseline.
    af_fb_fill_rect(cx - half, cy + half - 2, size, 2, primary);

    // Crossbar in the brand accent colour.
    af_i32 bar_y = cy + (half / 3);
    af_i32 bar_half = (half * 2) / 3;
    af_fb_fill_rect(cx - bar_half, bar_y, bar_half * 2, 3, accent);
}

// -----------------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------------
void af_splash_draw(void)
{
    af_surface_t *s = af_fb_surface();
    if (s == NULL) {
        return;
    }

    af_i32 W = (af_i32)s->width;
    af_i32 H = (af_i32)s->height;

    // --- responsive decision --------------------------------------------------
    // The same rule the Shell uses from v0.8: portrait-ish and small means the
    // phone layout; otherwise the desktop layout.
    s_phone_layout = ((af_u64)W * 1000 / (af_u64)H) < 1150;

    const af_color_t bg_top    = AF_RGB(0x0A, 0x14, 0x10);
    const af_color_t bg_bottom = AF_RGB(0x12, 0x14, 0x18);

    af_fb_fill_gradient_v(0, 0, W, H, bg_top, bg_bottom);

    // A subtle accent glow near the logo, blended rather than drawn.
    af_fb_blend_rect(0, H / 4, W, H / 3, AF_BRAND_PRIMARY, 18);

    if (s_phone_layout) {
        // ---------------------------------------------------------------------
        // PHONE LAYOUT — centred, stacked, generous vertical rhythm
        // ---------------------------------------------------------------------
        af_i32 logo_size = (W < H ? W : H) / 4;
        if (logo_size > 160) { logo_size = 160; }
        if (logo_size < 48)  { logo_size = 48; }

        af_i32 cx = W / 2;
        af_i32 cy = H / 2 - logo_size;

        draw_logo_mark(cx, cy, logo_size, AF_BRAND_PRIMARY, AF_BRAND_SECONDARY);

        af_i32 title_y = cy + logo_size / 2 + logo_size / 3;
        draw_text_centered(0, W, title_y, AF_NAME, 4, AF_BRAND_ON_SURFACE, bg_bottom);

        draw_text_centered(0, W, title_y + AF_FONT8X16_HEIGHT * 4 + 12,
                           "v" AF_VERSION_STRING " " AF_CODENAME, 2,
                           AF_BRAND_ACCENT, bg_bottom);

        af_i32 bar_w = W * 3 / 4;
        draw_progress_bar((W - bar_w) / 2, H - H / 5, bar_w, 8,
                          AF_RGB(0x24, 0x2A, 0x33), AF_BRAND_ACCENT, 100);

        draw_text_centered(0, W, H - H / 5 + 28,
                           "AfriyieOS " AF_VERSION_STRING " " AF_CODENAME, 2,
                           AF_COLOR_GRAY, bg_bottom);

        draw_text_centered(0, W, H - AF_FONT8X16_HEIGHT * 2 - 12,
                           "phone layout detected", 2, AF_COLOR_DARK_GRAY, bg_bottom);
    } else {
        // ---------------------------------------------------------------------
        // DESKTOP LAYOUT — a centred card, wide and short
        // ---------------------------------------------------------------------
        af_i32 card_w = W / 2;
        if (card_w > 720) { card_w = 720; }
        if (card_w < 320) { card_w = W - 40; }

        af_i32 card_h = H / 3;
        if (card_h > 280) { card_h = 280; }
        if (card_h < 160) { card_h = H - 40; }

        af_i32 card_x = (W - card_w) / 2;
        af_i32 card_y = (H - card_h) / 2 - 20;

        draw_shadow(card_x, card_y, card_w, card_h, 16, 60);
        fill_rounded_rect(card_x, card_y, card_w, card_h, 16,
                          AF_RGB(0x16, 0x1A, 0x22));

        // Accent strip along the top edge of the card.
        fill_rounded_rect(card_x, card_y, card_w, 4, 2, AF_BRAND_PRIMARY);

        af_i32 logo_size = card_h / 3;
        if (logo_size > 96) { logo_size = 96; }
        draw_logo_mark(card_x + logo_size / 2 + 40, card_y + card_h / 2,
                       logo_size, AF_BRAND_PRIMARY, AF_BRAND_SECONDARY);

        af_i32 text_x = card_x + logo_size + 70;

        af_fb_draw_text_scaled(text_x, card_y + card_h / 2 - 40,
                               AF_NAME, 5, AF_BRAND_ON_SURFACE,
                               AF_RGB(0x16, 0x1A, 0x22));

        af_fb_draw_text_scaled(text_x, card_y + card_h / 2 + 20,
                               "a minimal OS for PCs and phones", 2,
                               AF_COLOR_GRAY, AF_RGB(0x16, 0x1A, 0x22));

        af_i32 bar_w = card_w - 80;
        draw_progress_bar(card_x + 40, card_y + card_h - 44, bar_w, 6,
                          AF_RGB(0x24, 0x2A, 0x33), AF_BRAND_ACCENT, 100);

        // Footer, bottom-left of the screen.
        char footer[96];
        af_snprintf(footer, sizeof(footer),
                    "%s %s (%s)  |  %ux%u  |  %s  |  kernel alive",
                    AF_NAME, AF_VERSION_STRING, AF_CODENAME,
                    (af_u32)W, (af_u32)H, HAL_ARCH_NAME);
        af_fb_draw_text(20, H - AF_FONT8X16_HEIGHT - 12, footer,
                        AF_COLOR_DARK_GRAY, bg_bottom);
    }

    af_info("splash", "drew %s layout on %ux%u",
            s_phone_layout ? "phone" : "desktop", (af_u32)W, (af_u32)H);
}

bool af_splash_used_phone_layout(void)
{
    return s_phone_layout;
}
