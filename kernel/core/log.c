// SPDX-License-Identifier: MIT
// AfriyieOS — kernel logging and bounded formatted output

#include "afriyie/log.h"
#include "afriyie/kstring.h"
#include "afriyie/assert.h"
#include "afriyie/io.h"
#include "afriyie/arch_hooks.h"

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
static af_log_sink_fn   s_sink       = NULL;
static void            *s_sink_ctx   = NULL;
static af_log_level_t   s_min_level  = AF_LOG_MIN_LEVEL;

// Time source. Registered by kmain() once the timer is running, so that the
// log layer has no hard dependency on the HAL during very early boot.
typedef af_u64 (*af_log_time_fn)(void);
static af_log_time_fn s_time_fn = NULL;

void af_log_set_sink(af_log_sink_fn sink, void *ctx)
{
    s_sink     = sink;
    s_sink_ctx = ctx;
}

void af_log_register_time_source(af_u64 (*fn)(void))
{
    s_time_fn = fn;
}

void af_log_set_level(af_log_level_t level)
{
    s_min_level = level;
}

af_log_level_t af_log_get_level(void)
{
    return s_min_level;
}

const char *af_log_level_name(af_log_level_t level)
{
    switch (level) {
    case AF_LOG_TRACE: return "TRACE";
    case AF_LOG_DEBUG: return "DEBUG";
    case AF_LOG_INFO:  return "INFO ";
    case AF_LOG_WARN:  return "WARN ";
    case AF_LOG_ERROR: return "ERROR";
    case AF_LOG_FATAL: return "FATAL";
    default:           return "?????";
    }
}

// -----------------------------------------------------------------------------
// Raw output — used before formatting exists and by the panic path
// -----------------------------------------------------------------------------
void af_log_raw_n(const char *text, af_size len)
{
    if (text == NULL || len == 0) {
        return;
    }

    if (s_sink != NULL) {
        s_sink(text, len, s_sink_ctx);
        return;
    }

    // No sink: fall back to the architecture console one character at a time.
    for (af_size i = 0; i < len; i++) {
        af_arch_console_putc(text[i]);
    }
}

void af_log_raw(const char *text)
{
    if (text == NULL) {
        return;
    }
    af_log_raw_n(text, af_strlen(text));
}

// -----------------------------------------------------------------------------
// Bounded formatted output
//
// Supports exactly what the kernel needs: %s %c %d %i %u %x %X %p %% plus
// flag '-' (left align) and '0' (zero pad) and a decimal width. Length
// modifiers are 'l' and 'll' (treated as 64-bit). Unknown conversions are
// emitted verbatim so a typo is visible rather than silently swallowed.
//
// Semantics match snprintf: returns the length the output WOULD have had, and
// always NUL-terminates when size > 0.
// -----------------------------------------------------------------------------

typedef struct {
    char   *buf;
    af_size size;    // capacity including the terminator
    af_size pos;     // characters actually stored (excluding terminator)
    af_size total;   // characters that would have been stored
} fmt_out_t;

static void out_char(fmt_out_t *o, char c)
{
    if (o->pos + 1 < o->size) {
        o->buf[o->pos] = c;
        o->pos++;
        o->buf[o->pos] = '\0';
    }
    o->total++;
}

static void out_repeat(fmt_out_t *o, char c, af_size count)
{
    for (af_size i = 0; i < count; i++) {
        out_char(o, c);
    }
}

static void out_padded(fmt_out_t *o, const char *s, af_size len,
                       af_i32 width, bool left_align, bool zero_pad)
{
    af_size pad = 0;
    if (width > 0 && (af_size)width > len) {
        pad = (af_size)width - len;
    }

    if (left_align) {
        for (af_size i = 0; i < len; i++) {
            out_char(o, s[i]);
        }
        out_repeat(o, ' ', pad);
    } else {
        out_repeat(o, zero_pad ? '0' : ' ', pad);
        for (af_size i = 0; i < len; i++) {
            out_char(o, s[i]);
        }
    }
}

static af_size u64_to_str(af_u64 value, af_u32 base, bool uppercase,
                          char *buf, af_size buf_size)
{
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    af_size n = 0;

    if (buf_size == 0) {
        return 0;
    }

    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value != 0 && n < sizeof(tmp)) {
            tmp[n++] = digits[value % base];
            value /= base;
        }
    }

    af_size count = (n < buf_size - 1) ? n : (buf_size - 1);
    for (af_size i = 0; i < count; i++) {
        buf[i] = tmp[n - 1 - i];
    }
    buf[count] = '\0';
    return count;
}

int af_vsnprintf(char *buf, af_size size, const char *fmt, va_list args)
{
    fmt_out_t o;

    if (buf == NULL || size == 0) {
        return 0;
    }

    o.buf   = buf;
    o.size  = size;
    o.pos   = 0;
    o.total = 0;
    buf[0]  = '\0';

    if (fmt == NULL) {
        return 0;
    }

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            out_char(&o, *p);
            continue;
        }

        p++;
        if (*p == '\0') {
            out_char(&o, '%');       // trailing '%' — emit it literally
            break;
        }
        if (*p == '%') {
            out_char(&o, '%');
            continue;
        }

        // --- flags ---
        bool left_align = false;
        bool zero_pad   = false;
        for (;; p++) {
            if (*p == '-') {
                left_align = true;
            } else if (*p == '0') {
                zero_pad = true;
            } else if (*p == '+') {
                // '+' is accepted and ignored (no signed padding scheme yet)
            } else {
                break;
            }
        }

        // --- width ---
        af_i32 width = 0;
        while (*p >= '0' && *p <= '9') {
            width = (width * 10) + (*p - '0');
            p++;
        }

        // --- precision (accepted, applied only to strings) ---
        af_i32 precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            while (*p >= '0' && *p <= '9') {
                precision = (precision * 10) + (*p - '0');
                p++;
            }
        }

        // --- length modifier ---
        bool is_64 = false;
        while (*p == 'l' || *p == 'h' || *p == 'z') {
            if (*p == 'l' || *p == 'z') {
                is_64 = true;
            }
            p++;
        }

        char  num[32];
        af_size len = 0;

        switch (*p) {
        case 's': {
            const char *s = va_arg(args, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            // Precision truncates the string; width pads it. Emitting the
            // characters one at a time avoids any temporary copy and therefore
            // any fixed limit on string length.
            af_size slen = af_strlen(s);
            if (precision >= 0 && (af_size)precision < slen) {
                slen = (af_size)precision;
            }

            af_size pad = 0;
            if (width > 0 && (af_size)width > slen) {
                pad = (af_size)width - slen;
            }

            if (!left_align) {
                out_repeat(&o, ' ', pad);
            }
            for (af_size i = 0; i < slen; i++) {
                out_char(&o, s[i]);
            }
            if (left_align) {
                out_repeat(&o, ' ', pad);
            }
            continue;
        }

        case 'c': {
            char c = (char)va_arg(args, int);
            out_padded(&o, &c, 1, width, left_align, false);
            continue;
        }

        case 'd':
        case 'i': {
            af_i64 v = is_64 ? (af_i64)va_arg(args, af_i64)
                             : (af_i64)va_arg(args, int);
            bool negative = v < 0;

            // Negate in unsigned space so INT64_MIN cannot overflow.
            af_u64 uv = negative ? (~(af_u64)v + 1u) : (af_u64)v;
            len = u64_to_str(uv, 10, false, num, sizeof(num));

            if (negative) {
                // Right-shift the digits to make room for the sign.
                for (af_size i = len; i > 0; i--) {
                    num[i] = num[i - 1];
                }
                num[0] = '-';
                len++;
                num[len] = '\0';
            }
            out_padded(&o, num, len, width, left_align, zero_pad);
            continue;
        }

        case 'u': {
            af_u64 v = is_64 ? va_arg(args, af_u64) : (af_u64)va_arg(args, unsigned int);
            len = u64_to_str(v, 10, false, num, sizeof(num));
            out_padded(&o, num, len, width, left_align, zero_pad);
            continue;
        }

        case 'x':
        case 'X': {
            af_u64 v = is_64 ? va_arg(args, af_u64) : (af_u64)va_arg(args, unsigned int);
            len = u64_to_str(v, 16, (*p == 'X'), num, sizeof(num));
            out_padded(&o, num, len, width, left_align, zero_pad);
            continue;
        }

        case 'p': {
            af_u64 v = (af_u64)(af_uptr)va_arg(args, void *);
            num[0] = '0';
            num[1] = 'x';
            len = 2 + u64_to_str(v, 16, false, num + 2, sizeof(num) - 2);
            out_padded(&o, num, len, width, left_align, false);
            continue;
        }

        default:
            // Unknown conversion: emit it verbatim so the bug is visible.
            out_char(&o, '%');
            out_char(&o, *p);
            continue;
        }
    }

    return (int)o.total;
}

int af_snprintf(char *buf, af_size size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = af_vsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
}

// -----------------------------------------------------------------------------
// Levelled logging
// -----------------------------------------------------------------------------
#define AF_LOG_LINE_MAX 320

void af_log(af_log_level_t level, const char *module, const char *fmt, ...)
{
    char message[AF_LOG_LINE_MAX];
    char line[AF_LOG_LINE_MAX + 48];

    if (level < s_min_level) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    af_vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    // Timestamp: seconds since boot with microsecond resolution, or zeroes when
    // no time source is registered yet (which is exactly what early boot is).
    af_u64 ns = (s_time_fn != NULL) ? s_time_fn() : 0;

    af_snprintf(line, sizeof(line), "[%5u.%06u] %s %-6s: %s\n",
                (af_u32)(ns / 1000000000ULL),
                (af_u32)((ns % 1000000000ULL) / 1000ULL),
                af_log_level_name(level),
                (module != NULL) ? module : "core",
                message);

    af_log_raw(line);
}
