// SPDX-License-Identifier: MIT
// AfriyieOS — package reading
//
// Reading the metadata and contents of the package formats every Linux
// distribution uses: deb, rpm, pacman, Alpine apk, and the free-standing ones
// (AppImage, Flatpak, Snap). See docs/architecture/universal-compat.md §3.3.
//
// THE INSIGHT THAT MAKES "ALL DISTROS" TRACTABLE
//
// We do not need to run apt, dnf, pacman and apk. We need to READ their
// packages. A .deb is an ar archive containing two tar streams; an Alpine .apk
// is a gzipped tar; a pacman package is a zstd-compressed tar; an rpm is a
// binary header followed by a compressed archive. Every one of them is a
// container holding a description and a file list. That is a file format, not a
// religion.
//
// Once they are all readable, dependency resolution is ONE solver over ONE
// graph across every ecosystem — which is also how the same library ends up
// installed once rather than five times, and that is where "efficiently" in the
// objective is actually won or lost.
//
// NO ALLOCATION ANYWHERE. The caller supplies a scratch buffer for decompression
// and a structure to fill in. This is not premature frugality: user space has no
// allocator yet, and a library that needs one cannot be used by the first
// program that would want it. It also makes the library trivially usable from a
// host test, which is how it is verified today.
//
// WHAT IS NOT DONE, named rather than implied:
//
//   * LZMA-compressed chunks in .xz. The CONTAINER is complete and the
//     uncompressed-chunk layer works; a compressed chunk is refused by name.
//     This is the last common package compression still missing, and it is a
//     milestone of its own — see docs/architecture/xz-lzma2.md for what it costs
//     and for the fact that LZMA2 has no prose specification to work from.
//   * bzip2, refused by name.
//   * rpm is not written yet. Its structure is unlike the other four — a binary
//     header with its own tag directory, not an archive — and it is the last
//     reader the compatibility table needs.
//
// AND WHAT IS NOW DONE that used to be on that list: a .deb's control and data
// members are decompressed according to their MAGIC, so gzip, zstd and xz are
// all read, and a zstd-compressed .deb — what current dpkg-deb writes — opens
// end to end.

#ifndef AFRIYIE_AFPKG_H
#define AFRIYIE_AFPKG_H

// THESE LIVE UNDER kernel/include TODAY, which is the wrong place for them
// and is recorded rather than ignored. af_u8/af_u32/af_size and the status
// codes are the ABI every AfriyieOS component shares — kernel, user space,
// and the host tests — and an ABI that lives inside one of its consumers is
// an ABI that will drift. They belong in a top-level include/ that nothing
// owns. Moving them is a mechanical change and is deliberately not bundled
// into this one: it touches every file in the tree.
#include "afriyie/types.h"
#include "afriyie/status.h"
#include "afriyie/binfmt.h"   /* AF_BINFMT_* */

// -----------------------------------------------------------------------------
// Scratch space
//
// Decompression needs somewhere to write. The caller owns it, because the caller
// knows how long it can afford to be blocked and this library does not.
// -----------------------------------------------------------------------------
typedef struct {
    af_u8  *base;
    af_size size;
} afpkg_scratch_t;

// -----------------------------------------------------------------------------
// Install scripts
//
// Alpine ships these as dot-named files at the archive root: .pre-install,
// .post-install, .pre-upgrade, .post-upgrade, .pre-deinstall, .post-deinstall
// and .trigger. They are the metadata that has to be EXECUTED, as opposed to the
// metadata that is merely read, and a package manager that ignores them installs
// a package that does not work.
//
// The `data` pointer refers INTO the caller's scratch buffer — the same buffer
// the file list is walked from — so it is valid for as long as the scratch
// buffer is, and no longer. That is stated because it is the one lifetime in
// this library that is not obvious from the types.
// -----------------------------------------------------------------------------
typedef enum {
    AF_SCRIPT_NONE = 0,
    AF_SCRIPT_PRE_INSTALL,
    AF_SCRIPT_POST_INSTALL,
    AF_SCRIPT_PRE_UPGRADE,
    AF_SCRIPT_POST_UPGRADE,
    AF_SCRIPT_PRE_DEINSTALL,
    AF_SCRIPT_POST_DEINSTALL,
    AF_SCRIPT_TRIGGER,
} af_script_kind_t;

typedef struct {
    af_script_kind_t kind;
    const af_u8     *data;
    af_size          len;
} af_pkg_script_t;

#define AFPKG_MAX_SCRIPTS 8

// -----------------------------------------------------------------------------
// What a package says about itself
//
// Sizes are generous rather than exact. A truncated field is a package that
// installs under half its name, which is far worse than a few wasted bytes in a
// structure that exists once per package.
// -----------------------------------------------------------------------------
#define AFPKG_NAME_LEN        64
#define AFPKG_VERSION_LEN     64
#define AFPKG_ARCH_LEN        32
#define AFPKG_MAINTAINER_LEN  96
#define AFPKG_DESCRIPTION_LEN 256
#define AFPKG_MAX_DEPENDS     16

typedef struct {
    char   name[AFPKG_NAME_LEN];
    char   version[AFPKG_VERSION_LEN];
    char   architecture[AFPKG_ARCH_LEN];
    char   maintainer[AFPKG_MAINTAINER_LEN];
    char   description[AFPKG_DESCRIPTION_LEN];

    // The package's own claim about how much space it needs. Not trusted for
    // anything that matters — the file list is what determines that — but worth
    // carrying, because it is what a "do you want to install this?" prompt shows.
    af_u64 installed_size;

    // Raw dependency strings, exactly as the package wrote them.
    //
    // Deliberately NOT parsed into a dependency structure yet. Debian's grammar
    // is context-dependent and small ("libc6 (>= 2.31), libfoo | libbar" means
    // three different relationships), and a parser written before there is a
    // solver to feed is a parser written against guesses. The strings are kept
    // verbatim, which loses nothing.
    char   depends[AFPKG_MAX_DEPENDS][AFPKG_DESCRIPTION_LEN];
    af_u32 depends_count;

    // Empty for deb. Present so the structure does not change shape when the
    // formats that have one arrive.
    char   provides[AFPKG_MAX_DEPENDS][AFPKG_NAME_LEN];
    af_u32 provides_count;

    // The scripts that must be run, in the order the format requires.
    af_pkg_script_t scripts[AFPKG_MAX_SCRIPTS];
    af_u32          script_count;
} af_pkg_info_t;

// -----------------------------------------------------------------------------
// A file inside the package
// -----------------------------------------------------------------------------
#define AFPKG_PATH_LEN 192

typedef struct {
    char   path[AFPKG_PATH_LEN];   // normalised: no leading "./"
    af_u64 size;
    af_u32 mode;
    bool   is_directory;
} af_pkg_entry_t;

// -----------------------------------------------------------------------------
// The reader
//
// Opaque by size only; the caller allocates it, so its layout is fixed here.
// Everything outside `_internal` is the parsed result.
// -----------------------------------------------------------------------------
#define AFPKG_ITER_STATE_WORDS 8

typedef struct af_pkg {
    // --- results -------------------------------------------------------------
    af_pkg_info_t info;
    af_binfmt_t   format;          // AF_BINFMT_DEB, _APK_PKG or _PACMAN
    bool          valid;

    // --- why it failed, if it did --------------------------------------------
    //
    // Always set, on success too. A reader that explains only its failures
    // cannot be debugged when it succeeds wrongly.
    const char   *error;
    af_u32        error_offset;

    // --- internals -----------------------------------------------------------
    const af_u8  *data;
    af_size       len;
    afpkg_scratch_t scratch;

    // The data archive, decompressed and held for the caller to iterate.
    const af_u8  *data_tar;
    af_size       data_tar_len;

    af_u32        _reserved[AFPKG_ITER_STATE_WORDS];
} af_pkg_t;

// -----------------------------------------------------------------------------
// API
// -----------------------------------------------------------------------------

// Reads a package from a complete in-memory image.
//
// The whole package must be in memory. That is the right shape for now: the
// loader that will call this reads a file the caller already mapped, and a
// streaming API designed before there is a stream to feed it is a guess.
af_status_t afpkg_open(const af_u8 *data, af_size len,
                       const afpkg_scratch_t *scratch, af_pkg_t *out);

const char *afpkg_error(const af_pkg_t *pkg);

// Names a script kind, for diagnostics. Returns "?" for a value it does
// not know, which the self test checks cannot happen.
const char *afpkg_script_name(af_script_kind_t kind);

// Iterates the files the package would install.
typedef struct {
    af_size cursor;
} afpkg_iter_t;

void afpkg_iter_begin(afpkg_iter_t *it);

// Returns false at the end of the archive, or when the archive is malformed —
// afpkg_error on the package says which.
bool afpkg_iter_next(const af_pkg_t *pkg, afpkg_iter_t *it, af_pkg_entry_t *out);

// -----------------------------------------------------------------------------
// The layers underneath, exposed because they are independently useful and
// independently testable. An `ar` reader is not a package reader, and testing
// them separately is how a bug in one is not attributed to the other.
// -----------------------------------------------------------------------------

// Finds a member of an ar archive. Returns AF_ERR_NOENT when it is not there.
af_status_t afpkg_ar_find(const af_u8 *data, af_size len, const char *name,
                          const af_u8 **out, af_size *out_len,
                          const char **why);

// Finds a member whose name starts with `prefix` and ends with `suffix`, for
// .debs whose control member may be control.tar.{gz,xz,zst}.
af_status_t afpkg_ar_find_control(const af_u8 *data, af_size len,
                                  const af_u8 **out, af_size *out_len,
                                  const char **why);

// Inflates a gzip stream into `scratch`.
//
// Handles all three DEFLATE block types — stored, fixed Huffman and dynamic
// Huffman — with LZ77 back-references, and VERIFIES the trailer: CRC32 of the
// decompressed bytes and their length. A bit-flip inside compressed data often
// produces a valid stream that decodes to the wrong bytes, and the trailer is
// the only thing that detects it.
af_status_t afpkg_gunzip(const af_u8 *data, af_size len,
                         afpkg_scratch_t scratch, af_size *out_len,
                         const char **why);

// Inflates ONE gzip member and reports how many input bytes it consumed.
//
// A gzip FILE may hold several members concatenated — that is how Alpine's .apk
// is built, and how `cat a.gz b.gz` works. A caller that needs to walk them uses
// this; a caller with a single member uses afpkg_gunzip, which is this function
// with the count discarded.
af_status_t afpkg_gunzip_member(const af_u8 *data, af_size len,
                                afpkg_scratch_t scratch, af_size *out_len,
                                af_size *consumed, const char **why);

// Zstandard (RFC 8878), for modern .deb, pacman's .pkg.tar.zst and Snap.
//
// VERIFIED against 48 frames the reference implementation produced — 42 without a
// content checksum and 6 with one — every one compared byte for byte against the
// payload it came from. See tools/pkgsynth.py for how they are generated and
// tests/native/test_afpkg.c for what is asserted.
//
// Dictionaries and skippable frames are refused with AF_ERR_NOTSUP. Neither
// appears in a distribution package.
af_status_t afpkg_zstd(const af_u8 *data, af_size len,
                       af_u8 *out_base, af_size out_size,
                       af_size *out_len, const char **why);

// xz (the .xz container with the LZMA2 filter).
//
// PARTIAL BY DESIGN, and the boundary is exact: the CONTAINER is complete —
// stream header, block headers and their filter lists, block padding, the check
// field, the index and its records, and the CRC32/CRC64 guarding each — and
// LZMA2 chunks carrying UNCOMPRESSED data are decoded. A chunk that is
// LZMA-compressed is refused with AF_ERR_NOTSUP naming the chunk type, so a
// caller can tell "this is beyond me" apart from "this is broken".
//
// Read docs/architecture/xz-lzma2.md before extending it. The LZMA2 chunk layer
// and the LZMA algorithm have no prose specification — the reference
// implementation is the definition — which makes ADR-015's byte-exact-vector
// rule the only thing that can check the work.
af_status_t afpkg_xz(const af_u8 *data, af_size len,
                     af_u8 *out_base, af_size out_size,
                     af_size *out_len, const char **why);

// The raw DEFLATE layer, exposed because it is independently testable and
// because gzip is a wrapper around it rather than the thing itself. A bug in the
// wrapper and a bug in the encoder should not be attributable to each other.
//
// `out_consumed`, when not NULL, receives the number of INPUT bytes the stream
// occupied. gzip needs this: a member's trailer follows its deflate data
// immediately, and in a file holding several concatenated members the trailer is
// nowhere near the end of the buffer — so the position has to be reported rather
// than calculated.
af_status_t afpkg_inflate(const af_u8 *data, af_size len,
                          af_u8 *out_base, af_size out_size,
                          af_size *out_len, af_size *out_consumed,
                          const char **why);

// Iterates a tar archive held in memory.
af_status_t afpkg_tar_next(const af_u8 *data, af_size len, af_size *cursor,
                           af_pkg_entry_t *out, const af_u8 **contents,
                           af_size *contents_len, const char **why);

// Parses a Debian control file into an info structure.
void afpkg_parse_control(const char *text, af_size len, af_pkg_info_t *out);

#endif // AFRIYIE_AFPKG_H
