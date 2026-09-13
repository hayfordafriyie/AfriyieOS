// SPDX-License-Identifier: MIT
// AfriyieOS — assertions and the panic path

#ifndef AFRIYIE_ASSERT_H
#define AFRIYIE_ASSERT_H

#include "types.h"
#include "config.h"
#include "log.h"
#include "status.h"   // af_status_t and af_status_err, used by AF_TRY below

#ifndef AF_ASSERT_ENABLED
#  define AF_ASSERT_ENABLED 1
#endif

// -----------------------------------------------------------------------------
// Panic — never returns
// -----------------------------------------------------------------------------

// `file`, `line` and `func` are supplied by the AF_PANIC macros.
AF_NORETURN void af_panic_at(const char *file, int line, const char *func,
                             const char *fmt, ...) AF_PRINTF(4, 5);

#define af_panic(fmt, ...) \
    af_panic_at(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

// -----------------------------------------------------------------------------
// Assertions
//
// AF_ASSERT in a debug build halts the machine with a full diagnostic.
// In a release build it compiles to nothing at all.
// -----------------------------------------------------------------------------
#if AF_ASSERT_ENABLED
#  define AF_ASSERT(cond)                                                   \
      do {                                                                  \
          if (__builtin_expect(!(cond), 0)) {                               \
              af_panic_at(__FILE__, __LINE__, __func__,                     \
                          "assertion failed: %s", #cond);                   \
          }                                                                 \
      } while (0)

#  define AF_ASSERT_MSG(cond, fmt, ...)                                     \
      do {                                                                  \
          if (__builtin_expect(!(cond), 0)) {                               \
              af_panic_at(__FILE__, __LINE__, __func__,                     \
                          "assertion failed: %s — " fmt, #cond,             \
                          ##__VA_ARGS__);                                   \
          }                                                                 \
      } while (0)
#else
#  define AF_ASSERT(cond)                    ((void)0)
#  define AF_ASSERT_MSG(cond, fmt, ...)      ((void)0)
#endif

// -----------------------------------------------------------------------------
// Compile-time assertion (available in both build types)
// -----------------------------------------------------------------------------
#define AF_BUILD_BUG_ON(cond) _Static_assert(!(cond), "build bug: " #cond)

// -----------------------------------------------------------------------------
// Unreachable / error propagation
// -----------------------------------------------------------------------------

// Mark a path that the compiler can prove is dead but which we reach only if
// the kernel is already broken.
#define AF_UNREACHABLE()                                                    \
    do {                                                                    \
        af_panic_at(__FILE__, __LINE__, __func__, "unreachable code reached"); \
    } while (0)

// Propagate a failing status from within a function returning af_status_t.
#define AF_TRY(expr)                                                        \
    do {                                                                    \
        af_status_t _af_rc = (expr);                                        \
        if (af_status_err(_af_rc)) {                                        \
            return _af_rc;                                                  \
        }                                                                   \
    } while (0)

#define AF_TRY_MSG(expr, mod, fmt, ...)                                     \
    do {                                                                    \
        af_status_t _af_rc = (expr);                                        \
        if (af_status_err(_af_rc)) {                                        \
            af_error(mod, fmt " (status %s)", ##__VA_ARGS__,                \
                     af_status_name(_af_rc));                               \
            return _af_rc;                                                  \
        }                                                                   \
    } while (0)

#endif // AFRIYIE_ASSERT_H
