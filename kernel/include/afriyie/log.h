// SPDX-License-Identifier: MIT
// AfriyieOS — kernel logging
//
// The serial console is the primary debugging channel. Everything that matters
// is logged in a machine-greppable format so that CI and the development
// tooling can assert on boot state without a screen.
//
// Format:  [   0.000123] INFO  pmm    : 32768 frames available
//           ^seconds    ^level ^module  ^message

#ifndef AFRIYIE_LOG_H
#define AFRIYIE_LOG_H

#include "types.h"

typedef enum {
    AF_LOG_TRACE = 0,
    AF_LOG_DEBUG = 1,
    AF_LOG_INFO  = 2,
    AF_LOG_WARN  = 3,
    AF_LOG_ERROR = 4,
    AF_LOG_FATAL = 5,
} af_log_level_t;

// Compile-time minimum level. TRACE/DEBUG are stripped from release builds.
#ifndef AF_LOG_MIN_LEVEL
#  if AF_DEBUG
#    define AF_LOG_MIN_LEVEL AF_LOG_TRACE
#  else
#    define AF_LOG_MIN_LEVEL AF_LOG_INFO
#  endif
#endif

// -----------------------------------------------------------------------------
// Backend — implemented per architecture and set during early boot
// -----------------------------------------------------------------------------

// Installs the console writer. Called by the arch early-boot code once a UART
// is usable, and again when a framebuffer-backed console becomes available.
typedef void (*af_log_sink_fn)(const char *text, af_size len, void *ctx);
void af_log_set_sink(af_log_sink_fn sink, void *ctx);

// Minimum level actually emitted at runtime (may be raised at boot).
void       af_log_set_level(af_log_level_t level);
af_log_level_t af_log_get_level(void);

// Registers the clock used to stamp log lines. kmain() calls this once the
// timer is running; before that, timestamps are zero, which is exactly what
// early boot should look like. Returns nanoseconds since boot.
void af_log_register_time_source(af_u64 (*fn)(void));

// Raw write — used by the panic handler and the boot bridge before the
// formatting layer is ready. Serialised against concurrent threads.
void af_log_raw(const char *text);
void af_log_raw_n(const char *text, af_size len);

// Unserialised raw write. Used by the panic path, which must print even if the
// lock is held by the very thread that panicked, and before the scheduler exists
// so that no lock is taken during early boot.
void af_log_raw_unlocked(const char *text, af_size len);

// -----------------------------------------------------------------------------
// Formatted logging
// -----------------------------------------------------------------------------
void af_log(af_log_level_t level, const char *module, const char *fmt, ...)
    AF_PRINTF(3, 4);

// Formats into a caller-supplied buffer. Returns the number of characters that
// would have been written (snprintf semantics): a return >= `size` means the
// output was truncated. Bounded, never overruns, always NUL-terminates.
int af_vsnprintf(char *buf, af_size size, const char *fmt, va_list args);
int af_snprintf(char *buf, af_size size, const char *fmt, ...) AF_PRINTF(3, 4);

// -----------------------------------------------------------------------------
// Convenience macros
// -----------------------------------------------------------------------------
#if AF_LOG_MIN_LEVEL <= AF_LOG_TRACE
#  define af_trace(mod, ...) af_log(AF_LOG_TRACE, mod, __VA_ARGS__)
#else
#  define af_trace(mod, ...) ((void)0)
#endif

#if AF_LOG_MIN_LEVEL <= AF_LOG_DEBUG
#  define af_debug(mod, ...) af_log(AF_LOG_DEBUG, mod, __VA_ARGS__)
#else
#  define af_debug(mod, ...) ((void)0)
#endif

#define af_info(mod, ...)  af_log(AF_LOG_INFO,  mod, __VA_ARGS__)
#define af_warn(mod, ...)  af_log(AF_LOG_WARN,  mod, __VA_ARGS__)
#define af_error(mod, ...) af_log(AF_LOG_ERROR, mod, __VA_ARGS__)

// FATAL logs and then panics; it never returns.
#define af_fatal(mod, ...)                                                  \
    do {                                                                    \
        af_log(AF_LOG_FATAL, mod, __VA_ARGS__);                             \
        af_panic("fatal error in " mod);                                    \
    } while (0)

// Machine-readable boot milestones. CI greps the serial log for these exact
// strings — see .github/workflows/ci.yml. Do not rename them.
#define af_marker(name) af_log(AF_LOG_INFO, "boot", "%s", (name))

const char *af_log_level_name(af_log_level_t level);

#endif // AFRIYIE_LOG_H
