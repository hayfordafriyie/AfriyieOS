// SPDX-License-Identifier: MIT
// AfriyieOS — binary format identification
//
// The question this answers is "what IS this file, and what does it need to
// run?" — for every format AfriyieOS intends to support, without the kernel
// knowing anything about any of them.
//
// See docs/architecture/universal-compat.md and ADR-013. The short version:
//
//   * An extension is a hint, not a fact. `.exe` can be a PE, a .NET assembly
//     or a self-extracting archive; `.apk` is a ZIP; an AppImage is an ELF with
//     a filesystem glued to its end. Matching on extensions is wrong often
//     enough to be useless, and wrong *silently*, which is worse.
//
//   * So detection walks inward. A container is unwrapped until something
//     executable falls out, or until the depth limit stops it. The depth limit
//     is not decoration: a file can nest archives, and a naive unwrapper is a
//     denial-of-service against the thing doing the unwrapping.
//
//   * The result names the PERSONALITY required, not just the format. "This is
//     a PE32+" is a fact; "this needs the Windows personality, which is not
//     installed" is something a user can act on.
//
// The whole module is a pure function over a byte range. No allocation, no
// kernel calls, no logging. That is deliberate on two counts: it can run before
// anything exists, and it can be tested exhaustively on the host with synthetic
// headers instead of with a boot and a disk image.

#ifndef AFRIYIE_BINFMT_H
#define AFRIYIE_BINFMT_H

#include "types.h"

// -----------------------------------------------------------------------------
// Formats
//
// Ordered roughly by how directly they are executable. A container that unwraps
// to an executable reports the INNER format with AF_BINFMT_F_CONTAINER set,
// because that is the format that actually has to run.
// -----------------------------------------------------------------------------
typedef enum {
    AF_BINFMT_UNKNOWN = 0,

    // --- directly executable ---
    AF_BINFMT_ELF,            // ELF32/ELF64 — native, or Linux, or Android native
    AF_BINFMT_PE,             // PE32/PE32+ (.exe, .dll) — Windows
    AF_BINFMT_MACHO,          // Mach-O, thin or fat — macOS
    AF_BINFMT_DEX,            // Dalvik executable — Android
    AF_BINFMT_WASM,           // WebAssembly module
    AF_BINFMT_SCRIPT,         // #! — interpreted by something else

    // --- installable packages ---
    AF_BINFMT_DEB,
    AF_BINFMT_RPM,
    AF_BINFMT_PACMAN,         // Arch .pkg.tar.*
    AF_BINFMT_APK_PKG,        // Alpine .apk  (NOT Android — see the note below)
    AF_BINFMT_APK_ANDROID,    // Android .apk
    AF_BINFMT_AAB,            // Android App Bundle
    AF_BINFMT_FLATPAK,
    AF_BINFMT_SNAP,
    AF_BINFMT_MSI,
    AF_BINFMT_DMG,
    AF_BINFMT_APPIMAGE,
    AF_BINFMT_JAR,

    // --- containers we can look inside but not run ---
    AF_BINFMT_ZIP,
    AF_BINFMT_TAR,
    AF_BINFMT_SQUASHFS,
    AF_BINFMT_ZSTD,
    AF_BINFMT_GZIP,
    AF_BINFMT_XZ,
} af_binfmt_t;

// AF_BINFMT_APK_PKG and AF_BINFMT_APK_ANDROID SHARE THREE LETTERS AND NOTHING
// ELSE. Alpine's .apk is a gzipped tar with package metadata — a Linux package.
// Android's .apk is a ZIP containing a DEX and a manifest. Conflating them is a
// mistake that looks harmless in a format enum and produces a package manager
// that installs Android apps onto Alpine-style systems. The suffix on both
// constants exists so that confusion cannot be expressed.

// -----------------------------------------------------------------------------
// Personalities
//
// What has to exist for this format to run. A format with no personality is
// still detected — detection is not the same as the ability to execute, and
// conflating them is how "we support .exe" becomes a claim nobody can check.
// -----------------------------------------------------------------------------
typedef enum {
    AF_PERSONALITY_NONE = 0,      // nothing can run this
    AF_PERSONALITY_NATIVE,        // AfriyieOS's own ABI
    AF_PERSONALITY_LINUX,         // Linux syscall ABI
    AF_PERSONALITY_WINDOWS,       // Win32/Win64 API
    AF_PERSONALITY_ANDROID,       // ART + Android framework
    AF_PERSONALITY_DARWIN,        // Mach-O / macOS frameworks
    AF_PERSONALITY_WASM,          // WebAssembly runtime
    AF_PERSONALITY_SCRIPT,        // an interpreter named by the #! line
    AF_PERSONALITY_PACKAGE,       // handled by the package manager, not executed
} af_personality_t;

// -----------------------------------------------------------------------------
// Flags
// -----------------------------------------------------------------------------
#define AF_BINFMT_F_CONTAINER  (1u << 0)  // the answer came from inside a wrapper
#define AF_BINFMT_F_64BIT      (1u << 1)
#define AF_BINFMT_F_STATIC     (1u << 2)  // no dynamic interpreter needed
#define AF_BINFMT_F_TRUNCATED  (1u << 3)  // ran out of bytes before deciding
#define AF_BINFMT_F_AMBIGUOUS  (1u << 4)  // recognised, but cannot name one format
#define AF_BINFMT_F_NOEXEC     (1u << 5)  // a container with nothing executable

// How deep to unwrap. A file may legitimately nest (a .deb containing a .tar.xz,
// an AppImage containing a squashfs), and an attacker may nest deliberately. Two
// levels covers everything real; four is headroom that costs nothing, because
// the walk is over an in-memory prefix rather than a stream.
#define AF_BINFMT_MAX_DEPTH 4

// The prefix the caller must supply. Sized for the formats that need a look past
// the first bytes: PE's e_lfanew is at offset 0x3C and points at the PE header,
// which for a well-formed image is under 0x200; ZIP end-of-central-directory is
// at the END, which this API does not read — container identification that needs
// a trailer is handled by the caller passing the tail separately (see
// af_binfmt_detect_tail).
#define AF_BINFMT_PREFIX_LEN 4096

typedef struct {
    af_binfmt_t      format;
    af_personality_t personality;
    af_u32           flags;

    // Set when AF_BINFMT_F_CONTAINER: the format of the OUTER wrapper.
    af_binfmt_t      outer_format;

    // Where inside the prefix the decision was made. Diagnostic, and the thing
    // that makes a wrong answer debuggable without a rebuild.
    af_u32           decided_at;

    // Human-readable reason, always set, including on success. A detector that
    // only explains failures is a detector nobody can debug when it succeeds
    // wrongly.
    const char      *reason;
} af_binfmt_info_t;

// -----------------------------------------------------------------------------
// API
// -----------------------------------------------------------------------------

// Identifies `data[0..len)`. Returns the format and the personality it needs.
//
// Always fills `out`. A file it cannot identify yields AF_BINFMT_UNKNOWN with a
// reason saying what it looked at, rather than failing — an unrecognised file is
// a normal outcome, not an error.
void af_binfmt_detect(const af_u8 *data, af_size len, af_binfmt_info_t *out);

// Identifies a format that is found at the END of a file rather than the start.
//
// DMG is the reason this exists: a disk image's signature is the 'koly' trailer
// in its last 512 bytes, and the beginning is filesystem data that looks like
// anything. Zstandard frames and ZIP central directories are the same shape of
// problem.
void af_binfmt_detect_tail(const af_u8 *tail, af_size len, af_binfmt_info_t *out);

const char *af_binfmt_name(af_binfmt_t format);
const char *af_personality_name(af_personality_t personality);

// True when this FORMAT is one a loader can enter, in principle.
//
// This is a property of the format alone, and it answers "could a well-formed
// file of this kind be executed?" — not "can this particular file be executed?".
// A relocatable ELF is AF_BINFMT_ELF and is not runnable; a truncated PE is
// AF_BINFMT_PE and is not runnable. For that question use af_binfmt_can_run.
bool af_binfmt_is_executable(af_binfmt_t format);

// True when THIS file can be entered directly: the format is executable, it was
// identified confidently, and nothing in the flags says otherwise.
//
// Added because the first version of the self test caught the gap. It asserted
// `!af_binfmt_is_executable(info.format)` for a relocatable object, and
// af_binfmt_is_executable answered "yes, ELF is executable" — which is true and
// useless at the same time. A caller asking "can I run this file?" needs one
// function that means that, or every caller has to remember to check the flags,
// and one of them will not.
bool af_binfmt_can_run(const af_binfmt_info_t *info);

// Exercises the detector against synthetic headers for every format it claims to
// know, and emits AF_BINFMT_OK when they all pass.
//
// Runs at boot rather than in a host test suite because the module is kernel C
// and reimplementing it in Python to test it would mean testing the
// reimplementation. Every case is built from a byte array in memory, so it costs
// microseconds and needs no disk, no image and no firmware.
//
// A detector is exactly the kind of code that passes by recognising nothing and
// returning "unknown" for everything, so the test asserts POSITIVE results —
// the specific format and the specific personality — not merely that the call
// returned.
void af_binfmt_selftest(void);

#endif // AFRIYIE_BINFMT_H
