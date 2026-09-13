// SPDX-License-Identifier: MIT
// AfriyieOS — status codes
//
// Every fallible function in AfriyieOS returns af_status_t. There are no magic
// sentinel return values and no errno. Syscalls return the negated status on
// failure and a non-negative value on success.

#ifndef AFRIYIE_STATUS_H
#define AFRIYIE_STATUS_H

#include "types.h"

typedef af_i32 af_status_t;

// Success is exactly zero. Error codes are negative so that a plain
// `if (rc < 0)` catches every failure without a table lookup.
#define AF_OK               ((af_status_t)0)

// --- Generic ---------------------------------------------------------------
#define AF_ERR_GENERIC      ((af_status_t)-1)    // unspecified failure
#define AF_ERR_INVAL        ((af_status_t)-2)    // invalid argument
#define AF_ERR_NOMEM        ((af_status_t)-3)    // out of memory
#define AF_ERR_NOENT        ((af_status_t)-4)    // no such entry
#define AF_ERR_EXIST        ((af_status_t)-5)    // already exists
#define AF_ERR_BUSY         ((af_status_t)-6)    // resource is busy
#define AF_ERR_AGAIN        ((af_status_t)-7)    // try again (transient)
#define AF_ERR_NOTSUP       ((af_status_t)-8)    // operation not supported
#define AF_ERR_FAULT        ((af_status_t)-9)    // bad memory access
#define AF_ERR_OVERFLOW     ((af_status_t)-10)   // value out of range
#define AF_ERR_TIMEOUT      ((af_status_t)-11)   // operation timed out
#define AF_ERR_CANCELED     ((af_status_t)-12)   // operation cancelled

// --- Capability / security -------------------------------------------------
#define AF_ERR_PERM         ((af_status_t)-20)   // capability right missing
#define AF_ERR_CAP_INVALID  ((af_status_t)-21)   // capability slot empty/unknown
#define AF_ERR_CAP_EXHAUST  ((af_status_t)-22)   // no free capability slots
#define AF_ERR_PEER_DEAD    ((af_status_t)-23)   // IPC peer exited

// --- Object / process ------------------------------------------------------
#define AF_ERR_NOOBJ        ((af_status_t)-30)   // object no longer exists
#define AF_ERR_TOOMANY      ((af_status_t)-31)   // table full
#define AF_ERR_NOTREADY     ((af_status_t)-32)   // subsystem not initialised
#define AF_ERR_AFFINITY     ((af_status_t)-33)   // wrong CPU affinity

// --- Boot / firmware -------------------------------------------------------
#define AF_ERR_BOOT_MAGIC   ((af_status_t)-40)   // boot_info magic mismatch
#define AF_ERR_BOOT_MEMMAP  ((af_status_t)-41)   // unusable memory map
#define AF_ERR_BOOT_NOFB    ((af_status_t)-42)   // no framebuffer provided
#define AF_ERR_BOOT_VERSION ((af_status_t)-43)   // boot_info version mismatch

// --- Storage / file system -------------------------------------------------
#define AF_ERR_IO           ((af_status_t)-50)   // device I/O error
#define AF_ERR_NODEV        ((af_status_t)-51)   // no such device
#define AF_ERR_FS_CORRUPT   ((af_status_t)-52)   // on-disk structure invalid
#define AF_ERR_NOTDIR       ((af_status_t)-53)   // not a directory
#define AF_ERR_ISDIR        ((af_status_t)-54)   // is a directory
#define AF_ERR_NOSPC        ((af_status_t)-55)   // no space left
#define AF_ERR_ROFS         ((af_status_t)-56)   // read-only file system

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

AF_INLINE bool af_status_ok(af_status_t s)  { return s == AF_OK; }
AF_INLINE bool af_status_err(af_status_t s) { return s < AF_OK; }

// Human-readable name for logging. Never returns NULL.
// Defined in kernel/core/status.c so the strings live in one object file.
const char *af_status_name(af_status_t status);

#endif // AFRIYIE_STATUS_H
