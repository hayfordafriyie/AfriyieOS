// SPDX-License-Identifier: MIT
// AfriyieOS — framebuffer, 2D primitives and the on-screen console
//
// v0.1 draws straight into the linear framebuffer the boot bridge handed us.
// Everything here is software, clipped and stride-correct.

#include "afriyie/fb.h"
#include "afriyie/font.h"
#include "afriyie/config.h"
#include "afriyie/kstring.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
static af_surface_t s_surface;
static bool         s_available = false;

// On-screen console state
static af_u32 s_console_col   = 0;
static af_u32 s_console_row   = 0;
static af_color_t s_console_fg = AF_COLOR_WHITE;
static af_color_t s_console_bg = AF_COLOR_BLACK;
static bool  s_console_ready  = false;

// The console keeps its own linear buffer so that scrolling never has to read
// back from (potentially slow, uncached) framebuffer memory.
#define AF_CONSOLE_COLS 128
#define AF_CONSOLE_ROWS 48
static char s_console_text[AF_CONSOLE_ROWS][AF_CONSOLE_COLS];

// -----------------------------------------------------------------------------
// Colour conversion
// -----------------------------------------------------------------------------

// Converts a packed 0xRRGGBB colour into the hardware's native pixel value.
AF_INLINE af_u32 fb_color_to_native(const af_surface_t *s, af_color_t color)
{
    af_u32 r = (color >> 16) & 0xFFu;
    af_u32 g = (color >> 8) & 0xFFu;
    af_u32 b = color & 0xFFu;

    switch (s->format) {
    case AF_PIXEL_RGBX8888:
        return (r << 16) | (g << 8) | b;

    case AF_PIXEL_BGRX8888:
        return (b << 16) | (g << 8) | r;

    case AF_PIXEL_RGB565:
        // 5 bits red, 6 bits green, 5 bits blue. Replicate the high bits into
        // the low bits so that full white maps to 0xFFFF rather than 0xF7DE.
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    case AF_PIXEL_INDEXED:
    default:
        // Unsupported format: emit the colour as-is rather than guessing.
        return color;
    }
}

AF_INLINE af_u8 *fb_pixel_ptr(const af_surface_t *s, af_i32 x, af_i32 y)
{
    return s->pixels + ((af_size)y * s->pitch) + ((af_size)x * (s->bpp / 8u));
}

// -----------------------------------------------------------------------------
// Initialisation
// -----------------------------------------------------------------------------
af_status_t af_fb_init(const af_boot_framebuffer_t *fb)
{
    if (fb == NULL) {
        return AF_ERR_INVAL;
    }

    if (fb->address == 0 || fb->width == 0 || fb->height == 0 || fb->bpp == 0) {
        return AF_ERR_BOOT_NOFB;
    }

    if (fb->bpp != 16 && fb->bpp != 32) {
        af_error("fb", "unsupported %u bpp framebuffer (need 16 or 32)", fb->bpp);
        return AF_ERR_NOTSUP;
    }

    af_u32 minimum_pitch = (fb->width * fb->bpp) / 8u;
    if (fb->pitch < minimum_pitch) {
        af_error("fb", "pitch %u is smaller than the minimum %u for %ux%u at %u bpp",
                 fb->pitch, minimum_pitch, fb->width, fb->height, fb->bpp);
        return AF_ERR_BOOT_NOFB;
    }

    s_surface.pixels = (af_u8 *)(af_uptr)fb->address;
    s_surface.width  = fb->width;
    s_surface.height = fb->height;
    s_surface.pitch  = fb->pitch;
    s_surface.bpp    = (af_u8)fb->bpp;
    s_surface.format = fb->format;
    s_surface.size   = fb->pitch * fb->height;

    s_available = true;

    // A stride wider than the visible width is legal but must be reported: it is
    // the classic cause of a diagonally skewed image when code uses width*bpp/8.
    af_u32 expected = minimum_pitch;
    if (fb->pitch != expected) {
        af_info("fb", "stride is %u bytes wider than the visible line; "
                      "all drawing uses the pitch", fb->pitch - expected);
    }

    af_info("fb", "framebuffer ready: %ux%u, %u bpp, pitch %u, format %s",
            s_surface.width, s_surface.height, s_surface.bpp, s_surface.pitch,
            af_pixel_format_name(s_surface.format));

    return AF_OK;
}

af_surface_t *af_fb_surface(void)
{
    return s_available ? &s_surface : NULL;
}

bool af_fb_available(void)
{
    return s_available;
}

// -----------------------------------------------------------------------------
// Drawing primitives
// -----------------------------------------------------------------------------

// Clips a rectangle against the surface bounds. Returns false when the result
// is empty, which lets every caller bail out before computing any addresses.
static bool clip_rect(af_i32 *x, af_i32 *y, af_i32 *w, af_i32 *h)
{
    if (*w <= 0 || *h <= 0) {
        return false;
    }

    af_i32 x0 = *x;
    af_i32 y0 = *y;
    af_i32 x1 = *x + *w;
    af_i32 y1 = *y + *h;

    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > (af_i32)s_surface.width)  { x1 = (af_i32)s_surface.width; }
    if (y1 > (af_i32)s_surface.height) { y1 = (af_i32)s_surface.height; }

    if (x1 <= x0 || y1 <= y0) {
        return false;
    }

    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return true;
}

void af_fb_put_pixel(af_i32 x, af_i32 y, af_color_t color)
{
    if (!s_available) {
        return;
    }
    if (x < 0 || y < 0 ||
        (af_u32)x >= s_surface.width || (af_u32)y >= s_surface.height) {
        return;
    }

    af_u32 native = fb_color_to_native(&s_surface, color);
    af_u8 *p = fb_pixel_ptr(&s_surface, x, y);

    if (s_surface.bpp == 32) {
        *(volatile af_u32 *)(void *)p = native;
    } else {
        *(volatile af_u16 *)(void *)p = (af_u16)native;
    }
}

void af_fb_fill_rect(af_i32 x, af_i32 y, af_i32 w, af_i32 h, af_color_t color)
{
    if (!s_available || !clip_rect(&x, &y, &w, &h)) {
        return;
    }

    af_u32 native = fb_color_to_native(&s_surface, color);

    if (s_surface.bpp == 32) {
        for (af_i32 row = 0; row < h; row++) {
            volatile af_u32 *p =
                (volatile af_u32 *)(void *)fb_pixel_ptr(&s_surface, x, y + row);
            for (af_i32 col = 0; col < w; col++) {
                p[col] = native;
            }
        }
    } else {
        for (af_i32 row = 0; row < h; row++) {
            volatile af_u16 *p =
                (volatile af_u16 *)(void *)fb_pixel_ptr(&s_surface, x, y + row);
            for (af_i32 col = 0; col < w; col++) {
                p[col] = (af_u16)native;
            }
        }
    }
}

void af_fb_clear(af_color_t color)
{
    if (!s_available) {
        return;
    }
    af_fb_fill_rect(0, 0, (af_i32)s_surface.width, (af_i32)s_surface.height, color);
}

void af_fb_draw_hline(af_i32 x, af_i32 y, af_i32 w, af_color_t color)
{
    af_fb_fill_rect(x, y, w, 1, color);
}

void af_fb_draw_vline(af_i32 x, af_i32 y, af_i32 h, af_color_t color)
{
    af_fb_fill_rect(x, y, 1, h, color);
}

void af_fb_draw_rect(af_i32 x, af_i32 y, af_i32 w, af_i32 h, af_color_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    af_fb_draw_hline(x, y, w, color);                 // top
    af_fb_draw_hline(x, y + h - 1, w, color);         // bottom
    af_fb_draw_vline(x, y, h, color);                 // left
    af_fb_draw_vline(x + w - 1, y, h, color);         // right
}

void af_fb_fill_gradient_v(af_i32 x, af_i32 y, af_i32 w, af_i32 h,
                           af_color_t top, af_color_t bottom)
{
    if (!s_available || h <= 0) {
        return;
    }

    af_u32 r0 = (top >> 16) & 0xFFu, g0 = (top >> 8) & 0xFFu, b0 = top & 0xFFu;
    af_u32 r1 = (bottom >> 16) & 0xFFu, g1 = (bottom >> 8) & 0xFFu, b1 = bottom & 0xFFu;

    af_i32 dr = (af_i32)r1 - (af_i32)r0;
    af_i32 dg = (af_i32)g1 - (af_i32)g0;
    af_i32 db = (af_i32)b1 - (af_i32)b0;

    af_i32 den = (h > 1) ? (h - 1) : 1;

    for (af_i32 row = 0; row < h; row++) {
        // Signed interpolation, so gradients work in either direction.
        af_i32 r = (af_i32)r0 + (dr * row) / den;
        af_i32 g = (af_i32)g0 + (dg * row) / den;
        af_i32 b = (af_i32)b0 + (db * row) / den;

        af_color_t c = AF_RGB(r & 0xFF, g & 0xFF, b & 0xFF);
        af_fb_fill_rect(x, y + row, w, 1, c);
    }
}

void af_fb_blend_rect(af_i32 x, af_i32 y, af_i32 w, af_i32 h,
                      af_color_t color, af_u8 alpha)
{
    if (!s_available || alpha == 0 || !clip_rect(&x, &y, &w, &h)) {
        return;
    }
    if (alpha == 255) {
        af_fb_fill_rect(x, y, w, h, color);
        return;
    }

    af_u32 sr = (color >> 16) & 0xFFu;
    af_u32 sg = (color >> 8) & 0xFFu;
    af_u32 sb = color & 0xFFu;
    af_u32 a  = alpha;
    af_u32 ia = 255u - a;

    for (af_i32 row = 0; row < h; row++) {
        for (af_i32 col = 0; col < w; col++) {
            af_u8 *p = fb_pixel_ptr(&s_surface, x + col, y + row);

            af_u32 dr, dg, db;
            if (s_surface.bpp == 32) {
                af_u32 v = *(volatile af_u32 *)(void *)p;
                if (s_surface.format == AF_PIXEL_RGBX8888) {
                    dr = (v >> 16) & 0xFFu; dg = (v >> 8) & 0xFFu; db = v & 0xFFu;
                } else {
                    db = (v >> 16) & 0xFFu; dg = (v >> 8) & 0xFFu; dr = v & 0xFFu;
                }
                af_u32 nr = (sr * a + dr * ia + 127u) / 255u;
                af_u32 ng = (sg * a + dg * ia + 127u) / 255u;
                af_u32 nb = (sb * a + db * ia + 127u) / 255u;
                af_u32 nv = (s_surface.format == AF_PIXEL_RGBX8888)
                                ? ((nr << 16) | (ng << 8) | nb)
                                : ((nb << 16) | (ng << 8) | nr);
                *(volatile af_u32 *)(void *)p = nv;
            } else {
                af_u16 v = *(volatile af_u16 *)(void *)p;
                dr = ((v >> 11) & 0x1Fu) << 3;
                dg = ((v >> 5) & 0x3Fu) << 2;
                db = (v & 0x1Fu) << 3;
                af_u32 nr = (sr * a + dr * ia + 127u) / 255u;
                af_u32 ng = (sg * a + dg * ia + 127u) / 255u;
                af_u32 nb = (sb * a + db * ia + 127u) / 255u;
                *(volatile af_u16 *)(void *)p =
                    (af_u16)(((nr >> 3) << 11) | ((ng >> 2) << 5) | (nb >> 3));
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Text
// -----------------------------------------------------------------------------
void af_fb_draw_char(af_i32 x, af_i32 y, char c, af_color_t fg, af_color_t bg)
{
    if (!s_available) {
        return;
    }

    const af_u8 *glyph = af_font8x16_glyph(c);

    for (af_u32 row = 0; row < AF_FONT8X16_HEIGHT; row++) {
        af_u8 bits = glyph[row];
        for (af_u32 col = 0; col < AF_FONT8X16_WIDTH; col++) {
            // Bit 7 is the leftmost pixel.
            bool set = (bits & (0x80u >> col)) != 0;
            af_fb_put_pixel(x + (af_i32)col, y + (af_i32)row,
                            set ? fg : bg);
        }
    }
}

void af_fb_draw_text(af_i32 x, af_i32 y, const char *text,
                     af_color_t fg, af_color_t bg)
{
    if (!s_available || text == NULL) {
        return;
    }

    af_i32 cx = x;
    while (*text != '\0') {
        if (*text == '\n') {
            cx = x;
            y += AF_FONT8X16_HEIGHT;
        } else if (*text == '\r') {
            cx = x;
        } else if (*text == '\t') {
            cx += AF_FONT8X16_WIDTH * 4;
        } else {
            af_fb_draw_char(cx, y, *text, fg, bg);
            cx += AF_FONT8X16_WIDTH;
        }
        text++;
    }
}

void af_fb_draw_text_scaled(af_i32 x, af_i32 y, const char *text,
                            af_u32 scale, af_color_t fg, af_color_t bg)
{
    if (!s_available || text == NULL) {
        return;
    }
    if (scale <= 1) {
        af_fb_draw_text(x, y, text, fg, bg);
        return;
    }

    af_i32 cx = x;
    while (*text != '\0') {
        if (*text == '\n') {
            cx = x;
            y += (af_i32)(AF_FONT8X16_HEIGHT * scale);
            text++;
            continue;
        }

        const af_u8 *glyph = af_font8x16_glyph(*text);
        for (af_u32 row = 0; row < AF_FONT8X16_HEIGHT; row++) {
            af_u8 bits = glyph[row];
            for (af_u32 col = 0; col < AF_FONT8X16_WIDTH; col++) {
                bool set = (bits & (0x80u >> col)) != 0;
                af_fb_fill_rect(cx + (af_i32)(col * scale),
                                y + (af_i32)(row * scale),
                                (af_i32)scale, (af_i32)scale,
                                set ? fg : bg);
            }
        }
        cx += (af_i32)(AF_FONT8X16_WIDTH * scale);
        text++;
    }
}

void af_fb_measure_text(const char *text, af_u32 scale,
                        af_u32 *out_width, af_u32 *out_height)
{
    af_size len = af_strlen(text);
    if (scale == 0) {
        scale = 1;
    }
    if (out_width != NULL) {
        *out_width = (af_u32)len * AF_FONT8X16_WIDTH * scale;
    }
    if (out_height != NULL) {
        *out_height = AF_FONT8X16_HEIGHT * scale;
    }
}

// -----------------------------------------------------------------------------
// On-screen console
//
// A character grid kept in RAM, blitted with a single dirty-row redraw. This is
// deliberately simple: it exists so that a developer without a serial cable can
// still see the boot log.
// -----------------------------------------------------------------------------
void af_fb_console_init(af_color_t fg, af_color_t bg)
{
    if (!s_available) {
        return;
    }

    s_console_fg    = fg;
    s_console_bg    = bg;
    s_console_col   = 0;
    s_console_row   = 0;
    s_console_ready = true;

    af_memset(s_console_text, ' ', sizeof(s_console_text));
    af_fb_clear(bg);
}

void af_fb_console_clear(void)
{
    if (!s_console_ready) {
        return;
    }
    s_console_col = 0;
    s_console_row = 0;
    af_memset(s_console_text, ' ', sizeof(s_console_text));
    af_fb_clear(s_console_bg);
}

// Redraws a single text row from the RAM grid. Called after each mutation, so
// scrolling costs one row of pixel work rather than a full framebuffer copy.
static void console_redraw_row(af_u32 row)
{
    if (row >= AF_CONSOLE_ROWS) {
        return;
    }

    af_i32 y = (af_i32)(row * AF_FONT8X16_HEIGHT);

    for (af_u32 col = 0; col < AF_CONSOLE_COLS; col++) {
        af_i32 x = (af_i32)(col * AF_FONT8X16_WIDTH);
        if ((af_u32)x + AF_FONT8X16_WIDTH > s_surface.width) {
            break;
        }
        af_fb_draw_char(x, y, s_console_text[row][col],
                        s_console_fg, s_console_bg);
    }
}

static void console_scroll(void)
{
    af_memmove(&s_console_text[0][0],
               &s_console_text[1][0],
               (af_size)(AF_CONSOLE_ROWS - 1) * AF_CONSOLE_COLS);
    af_memset(&s_console_text[AF_CONSOLE_ROWS - 1][0], ' ', AF_CONSOLE_COLS);

    // Compact the pixel area too, then repaint only the last row. Moving the
    // block up is far cheaper than re-rendering every glyph.
    af_u32 line_height = AF_FONT8X16_HEIGHT;
    if (s_surface.height > line_height) {
        af_size bytes_per_line = (af_size)s_surface.pitch * line_height;
        af_u8 *dst = s_surface.pixels;
        af_u8 *src = s_surface.pixels + bytes_per_line;
        af_size total = (af_size)s_surface.pitch * (s_surface.height - line_height);
        af_memmove(dst, src, total);
    }

    console_redraw_row(AF_CONSOLE_ROWS - 1);
}

void af_fb_console_write(const char *text, af_size len)
{
    if (!s_console_ready || text == NULL) {
        return;
    }

    for (af_size i = 0; i < len; i++) {
        char c = text[i];

        if (c == '\n') {
            console_redraw_row(s_console_row);
            s_console_col = 0;
            s_console_row++;
            if (s_console_row >= AF_CONSOLE_ROWS) {
                console_scroll();
                s_console_row = AF_CONSOLE_ROWS - 1;
            }
            continue;
        }

        if (c == '\r') {
            s_console_col = 0;
            continue;
        }

        // The grid is a fixed width; a carriage-return-less long line wraps
        // rather than overwriting the row.
        if (s_console_col >= AF_CONSOLE_COLS) {
            console_redraw_row(s_console_row);
            s_console_col = 0;
            s_console_row++;
            if (s_console_row >= AF_CONSOLE_ROWS) {
                console_scroll();
                s_console_row = AF_CONSOLE_ROWS - 1;
            }
        }

        s_console_text[s_console_row][s_console_col] = (c >= 32 && c < 127) ? c : '.';
        s_console_col++;
    }

    // Paint the row being written so output appears as it arrives.
    console_redraw_row(s_console_row);
}

void af_fb_console_sink(const char *text, af_size len, void *ctx)
{
    AF_UNUSED(ctx);
    af_fb_console_write(text, len);
}
