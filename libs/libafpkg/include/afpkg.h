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
//   * The inflater handles DEFLATE STORED blocks only. That is a legal gzip
//     stream and it is what the generated fixtures contain, but real-world
//     packages use Huffman coding and will not decompress until the fixed and
//     dynamic Huffman paths land. The failure is an explicit error, never
//     garbage output.
//   * xz and zstd members are refused by name rather than misread.
//   * rpm, pacman and apk readers are not written yet. deb is first because it
//     is the most widely used and because its container exercises the most
//     machinery.

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
    af_binfmt_t   format;          // AF_BINFMT_DEB for now
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

// Inflates a gzip stream into `out`. Handles stored DEFLATE blocks; returns
// AF_ERR_NOTSUP for Huffman-coded ones rather than producing wrong bytes.
af_status_t afpkg_gunzip(const af_u8 *data, af_size len,
                         afpkg_scratch_t scratch, af_size *out_len,
                         const char **why);

// Iterates a tar archive held in memory.
af_status_t afpkg_tar_next(const af_u8 *data, af_size len, af_size *cursor,
                           af_pkg_entry_t *out, const af_u8 **contents,
                           af_size *contents_len, const char **why);

// Parses a Debian control file into an info structure.
void afpkg_parse_control(const char *text, af_size len, af_pkg_info_t *out);

#endif // AFRIYIE_AFPKG_H
