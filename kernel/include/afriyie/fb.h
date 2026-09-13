// SPDX-License-Identifier: MIT
// AfriyieOS — framebuffer and 2D drawing primitives
//
// v0.1 draws directly to the linear framebuffer provided by the boot bridge.
// The surface abstraction, double buffering and dirty-rectangle tracking arrive
// in v0.6 (see docs/AfriyieOS-Blueprint.md section 11).

#ifndef AFRIYIE_FB_H
#define AFRIYIE_FB_H

#include "types.h"
#include "boot_info.h"

// Packed 0xRRGGBB colour, converted to the hardware format at write time.
typedef af_u32 af_color_t;

#define AF_RGB(r, g, b) \
    ((((af_u32)(r) & 0xFFu) << 16) | (((af_u32)(g) & 0xFFu) << 8) | ((af_u32)(b) & 0xFFu))

#define AF_COLOR_BLACK        AF_RGB(0x00, 0x00, 0x00)
#define AF_COLOR_WHITE        AF_RGB(0xFF, 0xFF, 0xFF)
#define AF_COLOR_RED          AF_RGB(0xE5, 0x39, 0x35)
#define AF_COLOR_GREEN        AF_RGB(0x43, 0xA0, 0x47)
#define AF_COLOR_BLUE         AF_RGB(0x1E, 0x88, 0xE5)
#define AF_COLOR_YELLOW       AF_RGB(0xFB, 0xBC, 0x05)
#define AF_COLOR_CYAN         AF_RGB(0x00, 0xAC, 0xC1)
#define AF_COLOR_MAGENTA      AF_RGB(0xD8, 0x1B, 0x60)
#define AF_COLOR_ORANGE       AF_RGB(0xF4, 0x51, 0x1E)
#define AF_COLOR_GRAY         AF_RGB(0x9E, 0x9E, 0x9E)
#define AF_COLOR_DARK_GRAY    AF_RGB(0x42, 0x42, 0x42)
#define AF_COLOR_LIGHT_GRAY   AF_RGB(0xE0, 0xE0, 0xE0)

// AfriyieOS brand palette (see docs/design/ui-design-system.md)
#define AF_BRAND_PRIMARY      AF_RGB(0x0B, 0x6E, 0x4F)   // deep green
#define AF_BRAND_SECONDARY    AF_RGB(0xF4, 0xB4, 0x00)   // gold
#define AF_BRAND_SURFACE      AF_RGB(0x12, 0x14, 0x18)   // near-black surface
#define AF_BRAND_ON_SURFACE   AF_RGB(0xEC, 0xEF, 0xF4)
#define AF_BRAND_ACCENT       AF_RGB(0x27, 0xC0, 0x8A)

// -----------------------------------------------------------------------------
// Surface
// -----------------------------------------------------------------------------
typedef struct {
    af_u8             *pixels;    // virtual base address of pixel memory
    af_u32             width;     // visible width in pixels
    af_u32             height;    // visible height in pixels
    af_u32             pitch;     // bytes per scan line
    af_u8              bpp;       // bits per pixel (16 or 32)
    af_pixel_format_t  format;
    af_u32             size;      // total bytes of the mapped region
} af_surface_t;

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

// Initialises the global framebuffer surface from the boot handoff.
// Returns AF_ERR_BOOT_NOFB when no framebuffer was provided, in which case the
// system continues with serial output only.
af_status_t af_fb_init(const af_boot_framebuffer_t *fb);

// The global surface, or NULL when the framebuffer is unavailable.
af_surface_t *af_fb_surface(void);
bool          af_fb_available(void);

// -----------------------------------------------------------------------------
// Drawing
//
// All operations are clipped to the surface. Coordinates are signed so that
// partially off-screen rectangles behave correctly.
// -----------------------------------------------------------------------------
void af_fb_clear(af_color_t color);
void af_fb_put_pixel(af_i32 x, af_i32 y, af_color_t color);
void af_fb_fill_rect(af_i32 x, af_i32 y, af_i32 w, af_i32 h, af_color_t color);
void af_fb_draw_rect(af_i32 x, af_i32 y, af_i32 w, af_i32 h, af_color_t color);
void af_fb_draw_hline(af_i32 x, af_i32 y, af_i32 w, af_color_t color);
void af_fb_draw_vline(af_i32 x, af_i32 y, af_i32 h, af_color_t color);

// Vertical linear gradient — the modern-UI look, used by the splash screen.
void af_fb_fill_gradient_v(af_i32 x, af_i32 y, af_i32 w, af_i32 h,
                           af_color_t top, af_color_t bottom);

// Alpha blend of a solid colour onto the surface (alpha 0..255).
void af_fb_blend_rect(af_i32 x, af_i32 y, af_i32 w, af_i32 h,
                      af_color_t color, af_u8 alpha);

// -----------------------------------------------------------------------------
// Text (8x16 bitmap font, ASCII 32..126)
// -----------------------------------------------------------------------------
void af_fb_draw_char(af_i32 x, af_i32 y, char c, af_color_t fg, af_color_t bg);
void af_fb_draw_text(af_i32 x, af_i32 y, const char *text,
                     af_color_t fg, af_color_t bg);
void af_fb_draw_text_scaled(af_i32 x, af_i32 y, const char *text,
                            af_u32 scale, af_color_t fg, af_color_t bg);

// Measures the pixel size the text will occupy with the given scale.
void af_fb_measure_text(const char *text, af_u32 scale,
                        af_u32 *out_width, af_u32 *out_height);

#define AF_FONT_WIDTH   8
#define AF_FONT_HEIGHT  16

// -----------------------------------------------------------------------------
// Console sink — routes af_log output onto the screen once the framebuffer is
// initialised, so early boot is visible without a serial cable.
// -----------------------------------------------------------------------------
void af_fb_console_init(af_color_t fg, af_color_t bg);
void af_fb_console_clear(void);
void af_fb_console_write(const char *text, af_size len);
// Called by the log sink adapter in kernel/core/log.c.
void af_fb_console_sink(const char *text, af_size len, void *ctx);

#endif // AFRIYIE_FB_H
