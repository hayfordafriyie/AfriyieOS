// SPDX-License-Identifier: MIT
// AfriyieOS — package reading
//
// ar, gzip (stored blocks), tar, and the Debian control format. See afpkg.h for
// what this is for and what is deliberately not here.
//
// THE HOUSE STYLE FOR PARSERS, stated once because it is applied everywhere
// below: every field read from the input is a CLAIM by that input, and a claim
// is checked against the number of bytes actually available before it is used.
// A size field is not a size until it has been compared with what remains. This
// is not defensive programming; it is the difference between a reader that
// rejects a malformed package and one that walks off the end of a buffer.

#include "afpkg.h"

// -----------------------------------------------------------------------------
// Everything this file needs from a C library, written out.
//
// NOT <string.h>, AND NOT THE KERNEL'S kstring.h EITHER.
//
// The kernel's af_memcpy and friends exist only inside the kernel, so including
// them would make this library unbuildable anywhere else. <string.h> would make
// it unbuildable in AfriyieOS user space, which has no libc at all — that is
// the whole point of libaf. The library has to build in three places: a host
// test, kernel-adjacent code today, and user space tomorrow.
//
// Six one-line functions is a smaller cost than any of those dependencies.
// -----------------------------------------------------------------------------
static void afpkg_memset(void *dst, int value, af_size n)
{
    af_u8 *d = (af_u8 *)dst;
    for (af_size i = 0; i < n; i++) {
        d[i] = (af_u8)value;
    }
}

static af_size afpkg_strlen(const char *s)
{
    af_size n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int afpkg_strcmp(const char *a, const char *b)
{
    af_size i = 0;
    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return (int)(af_u8)a[i] - (int)(af_u8)b[i];
}

static void afpkg_strlcpy(char *dst, const char *src, af_size size)
{
    af_size i = 0;
    if (size == 0) {
        return;
    }
    while (i + 1 < size && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

// -----------------------------------------------------------------------------
// Small helpers
//
// af_memcpy and friends come from the kernel's kstring.h in kernel builds; in a
// host build they come from libc. Both are declared, so the code below does not
// care which.
// -----------------------------------------------------------------------------
// `data` is not read — the parameter is kept so every bounds check reads the
// same way as the code that follows it.
static bool region_ok(AF_UNUSED_PARAM const af_u8 *data, af_size len,
                      af_size at, af_size n)
{
    AF_UNUSED(data);
    return at <= len && (len - at) >= n;
}

static bool bytes_eq(const af_u8 *p, const char *lit, af_size n)
{
    for (af_size i = 0; i < n; i++) {
        if (p[i] != (af_u8)lit[i]) {
            return false;
        }
    }
    return true;
}

static bool starts_with(const char *s, const char *prefix)
{
    af_size i = 0;
    while (prefix[i] != '\0') {
        if (s[i] != prefix[i]) {
            return false;
        }
        i++;
    }
    return true;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool case_starts_with(const char *s, const char *prefix)
{
    af_size i = 0;
    while (prefix[i] != '\0') {
        if (lower(s[i]) != lower(prefix[i])) {
            return false;
        }
        i++;
    }
    return true;
}

// Copies at most dst_size-1 bytes and terminates. A field that does not fit is
// TRUNCATED rather than rejected: a name one byte too long should not make a
// package unreadable, but it must not overflow either.
static void copy_trim(char *dst, af_size dst_size, const char *src, af_size n)
{
    af_size i = 0;

    while (i < n && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';

    // Strip trailing whitespace. Control fields are written with a trailing
    // newline and often with trailing spaces, and a version string with a
    // trailing space fails a comparison that looks like it should succeed.
    while (i > 0 && (dst[i - 1] == ' ' || dst[i - 1] == '\t' ||
                     dst[i - 1] == '\r' || dst[i - 1] == '\n')) {
        dst[--i] = '\0';
    }
}

// =============================================================================
// ar
//
// "!<arch>\n" then 60-byte headers. Each header's size field is ASCII DECIMAL
// (unlike tar's octal, which is the sort of inconsistency that makes a shared
// "parse a number" helper a bug waiting to happen — so there is no shared
// helper).
// =============================================================================
#define AR_MAGIC     "!<arch>\n"
#define AR_MAGIC_LEN 8
#define AR_HEADER_LEN 60

static af_u64 parse_ascii_dec(const af_u8 *p, af_size n)
{
    af_u64 value = 0;
    af_size i = 0;

    while (i < n && p[i] == ' ') {
        i++;
    }
    while (i < n && p[i] >= '0' && p[i] <= '9') {
        value = value * 10u + (af_u64)(p[i] - '0');
        i++;
    }
    return value;
}

af_status_t afpkg_ar_find(const af_u8 *data, af_size len, const char *name,
                          const af_u8 **out, af_size *out_len,
                          const char **why)
{
    if (data == NULL || len < AR_MAGIC_LEN || !bytes_eq(data, AR_MAGIC, AR_MAGIC_LEN)) {
        *why = "not an ar archive: missing the !<arch> magic";
        return AF_ERR_INVAL;
    }

    af_size at = AR_MAGIC_LEN;

    while (at + AR_HEADER_LEN <= len) {
        const af_u8 *header = data + at;

        // The header terminator. A 60-byte header whose last two bytes are not
        // "`\n" is not a header, and continuing past it means reading file data
        // as structure.
        if (header[58] != 0x60 || header[59] != 0x0A) {
            *why = "ar header does not end with the required 0x60 0x0A";
            return AF_ERR_FS_CORRUPT;
        }

        // Member name: padded with spaces, and may end with '/' (the GNU long
        // form) or contain '/' for the embedded filename.
        char member[17];
        copy_trim(member, sizeof(member), (const char *)header, 16);

        const af_u64 size = parse_ascii_dec(header + 48, 10);

        const af_size payload = at + AR_HEADER_LEN;

        // THE CLAIM CHECKED AGAINST REALITY. A size field that runs past the end
        // of the file is the classic malformed-input case, and believing it is
        // how a reader walks off a buffer.
        if (size > len || payload > len || (len - payload) < size) {
            *why = "ar member size runs past the end of the archive";
            return AF_ERR_FS_CORRUPT;
        }

        if (afpkg_strcmp(member, name) == 0) {
            *out = data + payload;
            *out_len = (af_size)size;
            *why = "found";
            return AF_OK;
        }

        // Members are padded to an even boundary.
        af_size advance = (af_size)size;
        if (advance % 2 != 0) {
            advance++;
        }
        at = payload + advance;
    }

    *why = "ar member not found";
    return AF_ERR_NOENT;
}

af_status_t afpkg_ar_find_control(const af_u8 *data, af_size len,
                                  const af_u8 **out, af_size *out_len,
                                  const char **why)
{
    if (data == NULL || len < AR_MAGIC_LEN || !bytes_eq(data, AR_MAGIC, AR_MAGIC_LEN)) {
        *why = "not an ar archive";
        return AF_ERR_INVAL;
    }

    af_size at = AR_MAGIC_LEN;

    while (at + AR_HEADER_LEN <= len) {
        const af_u8 *header = data + at;

        if (header[58] != 0x60 || header[59] != 0x0A) {
            *why = "ar header terminator missing";
            return AF_ERR_FS_CORRUPT;
        }

        char member[17];
        copy_trim(member, sizeof(member), (const char *)header, 16);

        const af_u64 size = parse_ascii_dec(header + 48, 10);
        const af_size payload = at + AR_HEADER_LEN;

        if (size > len || payload > len || (len - payload) < size) {
            *why = "ar member size runs past the end of the archive";
            return AF_ERR_FS_CORRUPT;
        }

        if (starts_with(member, "control.tar")) {
            // AN UNSUPPORTED COMPRESSION IS REFUSED BY NAME, never guessed at.
            //
            // Treating an xz member as if it were gzip produces garbage that
            // parses as a control file with a wrong name, and the package then
            // installs under that name. An error is strictly better than a
            // plausible wrong answer.
            const char *suffix = member + 11;   // past "control.tar"

            if (suffix[0] == '\0') {
                *out = data + payload;
                *out_len = (af_size)size;
                *why = "found uncompressed control.tar";
                return AF_OK;
            }
            if (afpkg_strcmp(suffix, ".gz") == 0) {
                *out = data + payload;
                *out_len = (af_size)size;
                *why = "found control.tar.gz";
                return AF_OK;
            }

            *why = "control member uses a compression this build does not "
                   "support (only control.tar and control.tar.gz are handled)";
            return AF_ERR_NOTSUP;
        }

        af_size advance = (af_size)size;
        if (advance % 2 != 0) {
            advance++;
        }
        at = payload + advance;
    }

    *why = "no control.tar member in the archive";
    return AF_ERR_NOENT;
}

// =============================================================================
// gzip
//
// Enough of DEFLATE to decompress a stored-block stream. Stored blocks are
// legal, are what the fixtures contain, and are the case that exercises the
// framing — which is where the bugs live.
//
// Huffman-coded blocks are refused with AF_ERR_NOTSUP. That is the honest choice:
// a partial Huffman implementation produces wrong bytes, and wrong bytes from a
// decompressor are indistinguishable from a corrupt package.
// =============================================================================
// -----------------------------------------------------------------------------
// CRC32
//
// The gzip trailer carries a CRC32 of the decompressed bytes, and it is the only
// thing in the format that detects corruption which still decompresses. A
// bit-flip inside the compressed data frequently produces a perfectly valid
// stream that decodes to the wrong bytes, and without the check that package
// installs.
//
// BITWISE RATHER THAN TABLE-DRIVEN. A 256-entry table is 1 KiB of .rodata and is
// faster; this is eight shifts per byte and costs nothing at the sizes involved.
// The table can arrive when profiling says it should, which is the same argument
// as everywhere else: write the version whose correctness is obvious, and let a
// measurement, rather than an instinct, ask for the other one.
// -----------------------------------------------------------------------------
static af_u32 crc32_bytes(const af_u8 *data, af_size len)
{
    af_u32 crc = 0xFFFFFFFFu;

    for (af_size i = 0; i < len; i++) {
        crc ^= (af_u32)data[i];
        for (int bit = 0; bit < 8; bit++) {
            // 0xEDB88320 is the reversed CRC-32 polynomial. The mask is `-(crc & 1)`
            // rather than a branch, because a branch here is a misprediction on
            // half of all bytes.
            crc = (crc >> 1) ^ (0xEDB88320u & (af_u32)(-(af_i32)(crc & 1u)));
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

// -----------------------------------------------------------------------------
// One gzip member
//
// A gzip FILE may contain several members concatenated — that is how Alpine's
// .apk is built, and how `cat a.gz b.gz` works. This inflates exactly one and
// reports how many input bytes it consumed, so a caller can walk them.
//
// The trailer is VERIFIED here: CRC32 of the output and the output length. The
// first version of this function checked neither and said so in a comment, which
// was honest and was still a hole — a corrupted package would install and the
// damage would surface in whatever the package did.
// -----------------------------------------------------------------------------
af_status_t afpkg_gunzip_member(const af_u8 *data, af_size len,
                                afpkg_scratch_t scratch, af_size *out_len,
                                af_size *consumed, const char **why)
{
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (consumed != NULL) {
        *consumed = 0;
    }

    if (data == NULL || len < 18) {
        *why = "gzip stream too short to contain a header and a trailer";
        return AF_ERR_INVAL;
    }

    if (data[0] != 0x1F || data[1] != 0x8B) {
        *why = "not a gzip stream: missing 0x1f 0x8b";
        return AF_ERR_INVAL;
    }
    if (data[2] != 8) {
        *why = "gzip stream uses a compression method other than DEFLATE";
        return AF_ERR_NOTSUP;
    }

    const af_u8 flags = data[3];
    af_size at = 10;

    if ((flags & 0xE0) != 0) {
        // The three high bits are reserved and must be zero. A stream that sets
        // one is not gzip, whatever else it looks like.
        *why = "gzip header has reserved flag bits set";
        return AF_ERR_FS_CORRUPT;
    }

    // Optional fields, in the order the format specifies. They must be skipped
    // in order and their lengths honoured, or the deflate stream starts at the
    // wrong byte and the first block header is read out of a filename.
    if ((flags & 0x04) != 0) {                  // FEXTRA
        if (!region_ok(data, len, at, 2)) {
            *why = "gzip FEXTRA length is past the end of the stream";
            return AF_ERR_FS_CORRUPT;
        }
        const af_size xlen = (af_size)data[at] | ((af_size)data[at + 1] << 8);
        if (!region_ok(data, len, at + 2, xlen)) {
            *why = "gzip FEXTRA runs past the end of the stream";
            return AF_ERR_FS_CORRUPT;
        }
        at += 2 + xlen;
    }
    if ((flags & 0x08) != 0) {                  // FNAME
        while (at < len && data[at] != 0) {
            at++;
        }
        at++;
    }
    if ((flags & 0x10) != 0) {                  // FCOMMENT
        while (at < len && data[at] != 0) {
            at++;
        }
        at++;
    }
    if ((flags & 0x02) != 0) {                  // FHCRC
        at += 2;
    }

    if (scratch.base == 0 || scratch.size == 0) {
        *why = "no scratch buffer supplied for decompression";
        return AF_ERR_INVAL;
    }

    if (at + 8 > len) {
        *why = "gzip stream is too short to contain its trailer";
        return AF_ERR_FS_CORRUPT;
    }

    // THE DEFLATE STREAM IS NOT NECESSARILY THE REST OF THE BUFFER, and computing
    // the trailer as "eight bytes before the end" is wrong the moment a second
    // member follows — which is every Alpine .apk. The inflater reports where it
    // finished instead.
    af_size produced = 0;
    af_size deflate_used = 0;

    const af_status_t rc = afpkg_inflate(data + at, len - at,
                                         scratch.base, scratch.size,
                                         &produced, &deflate_used, why);
    if (af_status_err(rc)) {
        return rc;
    }

    if (!region_ok(data, len, at + deflate_used, 8)) {
        *why = "gzip stream has no trailer after its deflate data";
        return AF_ERR_FS_CORRUPT;
    }

    // The trailer. ISIZE is the uncompressed length MODULO 2^32, so the
    // comparison has to be masked the same way — checking a 64-bit length
    // against a 32-bit field rejects every stream over 4 GiB and, worse, would
    // have looked like a CRC failure.
    const af_u8 *trailer = data + at + deflate_used;
    const af_u32 want_crc = (af_u32)trailer[0] | ((af_u32)trailer[1] << 8) |
                            ((af_u32)trailer[2] << 16) | ((af_u32)trailer[3] << 24);
    const af_u32 want_size = (af_u32)trailer[4] | ((af_u32)trailer[5] << 8) |
                             ((af_u32)trailer[6] << 16) | ((af_u32)trailer[7] << 24);

    if (want_size != (af_u32)(produced & 0xFFFFFFFFu)) {
        *why = "gzip trailer ISIZE does not match the decompressed length — "
               "the stream is corrupt";
        return AF_ERR_FS_CORRUPT;
    }

    const af_u32 got_crc = crc32_bytes(scratch.base, produced);
    if (got_crc != want_crc) {
        *why = "gzip trailer CRC32 does not match the decompressed bytes — "
               "the stream is corrupt";
        return AF_ERR_FS_CORRUPT;
    }

    if (out_len != NULL) {
        *out_len = produced;
    }
    if (consumed != NULL) {
        // Header + this member's deflate data + its trailer. This is what makes
        // a concatenated file walkable: the caller advances by exactly this and
        // lands on the next member's magic.
        *consumed = at + deflate_used + 8;
    }

    *why = "inflated one member, trailer verified";
    return AF_OK;
}

af_status_t afpkg_gunzip(const af_u8 *data, af_size len,
                         afpkg_scratch_t scratch, af_size *out_len,
                         const char **why)
{
    af_size consumed = 0;
    return afpkg_gunzip_member(data, len, scratch, out_len, &consumed, why);
}

// =============================================================================
// tar
//
// 512-byte headers, OCTAL numeric fields, two zero blocks to end. Octal is the
// detail that matters: a reader that parses them as decimal reports a file size
// of zero for "00000000144" and a plausible wrong number for others.
// =============================================================================
#define TAR_BLOCK 512

static af_u64 parse_octal(const af_u8 *p, af_size n)
{
    af_u64 value = 0;
    af_size i = 0;

    while (i < n && (p[i] == ' ' || p[i] == '\0')) {
        i++;
    }
    while (i < n && p[i] >= '0' && p[i] <= '7') {
        value = value * 8u + (af_u64)(p[i] - '0');
        i++;
    }
    return value;
}

static bool tar_block_is_zero(const af_u8 *block)
{
    for (af_size i = 0; i < TAR_BLOCK; i++) {
        if (block[i] != 0) {
            return false;
        }
    }
    return true;
}

// Normalises "./usr/bin/x" to "usr/bin/x". tar entries in a .deb are written
// with a leading "./", and leaving it produces paths that look different from
// every other tool's for the same file.
static void normalise_path(char *dst, af_size dst_size, const char *src)
{
    while (src[0] == '.' && src[1] == '/') {
        src += 2;
    }
    afpkg_strlcpy(dst, src, dst_size);
}

af_status_t afpkg_tar_next(const af_u8 *data, af_size len, af_size *cursor,
                           af_pkg_entry_t *out, const af_u8 **contents,
                           af_size *contents_len, const char **why)
{
    af_size at = *cursor;

    // Two consecutive zero blocks end the archive. This loop tolerates the
    // single one that some writers emit, because rejecting an archive over a
    // cosmetic difference is a compatibility bug with no upside.
    while (region_ok(data, len, at, TAR_BLOCK) && tar_block_is_zero(data + at)) {
        at += TAR_BLOCK;
    }

    if (!region_ok(data, len, at, TAR_BLOCK)) {
        *why = "end of tar archive";
        return AF_ERR_NOENT;
    }

    const af_u8 *header = data + at;

    const af_u64 size = parse_octal(header + 124, 12);
    const af_u8  type = header[156];

    char name[AFPKG_PATH_LEN];
    {
        char raw[101];
        copy_trim(raw, sizeof(raw), (const char *)header, 100);
        normalise_path(name, sizeof(name), raw);
    }

    const af_size payload = at + TAR_BLOCK;

    if (size > len || payload > len || (len - payload) < size) {
        *why = "tar entry size runs past the end of the archive";
        return AF_ERR_FS_CORRUPT;
    }

    af_size advance = TAR_BLOCK + (af_size)size;
    advance = (advance + (TAR_BLOCK - 1)) & ~(af_size)(TAR_BLOCK - 1);
    *cursor = at + advance;

    // A long-name entry (type 'L') carries the real name in its payload. .deb
    // archives do not use it, and rather than half-support it the entry is
    // skipped — but it is skipped EXPLICITLY, so a package that relies on it
    // produces a missing file rather than a file with the wrong name.
    if (type == 'L' || type == 'K' || type == 'x' || type == 'g') {
        *why = "skipped a tar extension entry";
        return AF_ERR_AGAIN;
    }

    // Only regular files and directories are reported. Links, devices and FIFOs
    // exist in packages and are not implemented; reporting them as zero-length
    // regular files would install the wrong thing.
    const bool is_dir = (type == '5') || (name[0] != '\0' &&
                                          name[afpkg_strlen(name) - 1] == '/');
    const bool is_file = (type == '0' || type == '\0');

    if (!is_dir && !is_file) {
        *why = "skipped a tar entry that is not a regular file or directory";
        return AF_ERR_AGAIN;
    }

    afpkg_memset(out, 0, sizeof(*out));
    afpkg_strlcpy(out->path, name, sizeof(out->path));
    out->size         = is_dir ? 0 : size;
    out->mode         = (af_u32)parse_octal(header + 100, 8);
    out->is_directory = is_dir;

    *contents     = data + payload;
    *contents_len = is_dir ? 0 : (af_size)size;
    *why = is_dir ? "directory" : "file";
    return AF_OK;
}

// =============================================================================
// The Debian control file
//
// "Field: value" lines, a space at the start of a line continues the previous
// field, and the first blank line ends the paragraph. Comments do not exist.
// =============================================================================
static void set_depends(af_pkg_info_t *out, const char *value, af_size n)
{
    if (out->depends_count >= AFPKG_MAX_DEPENDS) {
        return;
    }
    copy_trim(out->depends[out->depends_count], AFPKG_DESCRIPTION_LEN, value, n);
    out->depends_count++;
}

void afpkg_parse_control(const char *text, af_size len, af_pkg_info_t *out)
{
    afpkg_memset(out, 0, sizeof(*out));

    af_size at = 0;

    while (at < len) {
        // End of the paragraph.
        if (text[at] == '\n') {
            break;
        }

        // Find the end of this line.
        af_size eol = at;
        while (eol < len && text[eol] != '\n') {
            eol++;
        }
        const af_size line_len = eol - at;

        // A continuation line belongs to the field above it. The description in
        // particular is multi-line, and dropping the continuations loses the
        // only prose a user ever reads about a package.
        if (line_len > 0 && (text[at] == ' ' || text[at] == '\t')) {
            if (out->description[0] != '\0') {
                const af_size used = afpkg_strlen(out->description);
                if (used + 1 < AFPKG_DESCRIPTION_LEN) {
                    out->description[used] = ' ';
                    copy_trim(out->description + used + 1,
                              AFPKG_DESCRIPTION_LEN - used - 1,
                              text + at + 1, line_len - 1);
                }
            }
            at = eol + 1;
            continue;
        }

        // "Field: value"
        af_size colon = at;
        while (colon < eol && text[colon] != ':') {
            colon++;
        }

        if (colon < eol) {
            const char *value = text + colon + 1;
            af_size value_len = eol - colon - 1;

            // Skip the single space after the colon, if there is one.
            while (value_len > 0 && (*value == ' ' || *value == '\t')) {
                value++;
                value_len--;
            }

            char field[64];
            copy_trim(field, sizeof(field), text + at, colon - at);

            if (case_starts_with(field, "Package")) {
                copy_trim(out->name, AFPKG_NAME_LEN, value, value_len);
            } else if (case_starts_with(field, "Version")) {
                copy_trim(out->version, AFPKG_VERSION_LEN, value, value_len);
            } else if (case_starts_with(field, "Architecture")) {
                copy_trim(out->architecture, AFPKG_ARCH_LEN, value, value_len);
            } else if (case_starts_with(field, "Maintainer")) {
                copy_trim(out->maintainer, AFPKG_MAINTAINER_LEN, value, value_len);
            } else if (case_starts_with(field, "Description")) {
                copy_trim(out->description, AFPKG_DESCRIPTION_LEN, value, value_len);
            } else if (case_starts_with(field, "Installed-Size")) {
                out->installed_size = parse_ascii_dec((const af_u8 *)value, value_len);
            } else if (case_starts_with(field, "Depends")) {
                set_depends(out, value, value_len);
            } else if (case_starts_with(field, "Provides")) {
                if (out->provides_count < AFPKG_MAX_DEPENDS) {
                    copy_trim(out->provides[out->provides_count], AFPKG_NAME_LEN,
                              value, value_len);
                    out->provides_count++;
                }
            }
        }

        at = eol + 1;
    }
}


// =============================================================================
// Alpine Linux packages
//
// NOT AN ANDROID .apk. Alpine's is a gzipped tar containing a `.PKGINFO` file;
// Android's is a ZIP containing a DEX and a manifest. They share three letters
// and nothing else, which is why the format constants in binfmt.h are named
// AF_BINFMT_APK_PKG and AF_BINFMT_APK_ANDROID rather than both being "apk".
//
// THE STRUCTURE THAT MAKES THIS WORTH ITS OWN READER: a .apk is not one gzip
// stream holding one tar. It is several gzip members CONCATENATED in one file —
// the signature tar, then the control tar holding .PKGINFO, then the data tar.
// A reader that inflates one member and stops gets the signature and installs
// nothing. A reader that inflates them all into one buffer and walks the result
// as a single tar gets every file in the package, including the ones that are
// metadata rather than payload.
//
// So the members are walked one at a time, each treated as its own archive, and
// the LAST member is the data tar — which is the only one whose contents are
// files to install.
// =============================================================================
#define APK_PKGINFO_MAX (16 * 1024)

// Parses a `.PKGINFO` file: "key = value" lines, with '#' comments.
//
// Separate from the Debian control parser because the formats are separate. They
// look alike and are not: Debian uses "Field: value" with continuation lines and
// a blank line terminating a paragraph; Alpine uses "key = value" with no
// continuations and no paragraphs. Sharing one parser between them would mean a
// parser that is wrong about both.
static void afpkg_parse_pkginfo(const char *text, af_size len, af_pkg_info_t *out)
{
    af_size at = 0;

    while (at < len) {
        af_size eol = at;
        while (eol < len && text[eol] != '\n') {
            eol++;
        }

        af_size line_start = at;
        while (line_start < eol && (text[line_start] == ' ' ||
                                    text[line_start] == '\t' ||
                                    text[line_start] == '\r')) {
            line_start++;
        }

        if (line_start < eol && text[line_start] != '#') {
            // Find the '='.
            af_size eq = line_start;
            while (eq < eol && text[eq] != '=') {
                eq++;
            }

            if (eq < eol) {
                char key[64];
                copy_trim(key, sizeof(key), text + line_start, eq - line_start);

                const char *value = text + eq + 1;
                af_size value_len = eol - eq - 1;
                while (value_len > 0 && (*value == ' ' || *value == '\t')) {
                    value++;
                    value_len--;
                }

                if (afpkg_strcmp(key, "pkgname") == 0) {
                    copy_trim(out->name, AFPKG_NAME_LEN, value, value_len);
                } else if (afpkg_strcmp(key, "pkgver") == 0) {
                    copy_trim(out->version, AFPKG_VERSION_LEN, value, value_len);
                } else if (afpkg_strcmp(key, "arch") == 0) {
                    copy_trim(out->architecture, AFPKG_ARCH_LEN, value, value_len);
                } else if (afpkg_strcmp(key, "packager") == 0) {
                    copy_trim(out->maintainer, AFPKG_MAINTAINER_LEN, value, value_len);
                } else if (afpkg_strcmp(key, "pkgdesc") == 0) {
                    copy_trim(out->description, AFPKG_DESCRIPTION_LEN, value, value_len);
                } else if (afpkg_strcmp(key, "size") == 0) {
                    out->installed_size = parse_ascii_dec((const af_u8 *)value, value_len);
                } else if (afpkg_strcmp(key, "depend") == 0) {
                    // One `depend` line per dependency, not a comma-separated
                    // list. Treating it as a list would make "so:libc.musl-x86_64.so.1"
                    // parse as two dependencies with a colon in the first.
                    set_depends(out, value, value_len);
                } else if (afpkg_strcmp(key, "provides") == 0) {
                    if (out->provides_count < AFPKG_MAX_DEPENDS) {
                        copy_trim(out->provides[out->provides_count], AFPKG_NAME_LEN,
                                  value, value_len);
                        out->provides_count++;
                    }
                }
            }
        }

        at = eol + 1;
    }
}

// True for the entries at the root of an Alpine archive that describe the
// package rather than being part of it.
//
// THE RULE IS "A DOT-NAME AT THE ROOT", and it was learned from a real package.
// The first version excluded exactly .PKGINFO and .SIGN.*, naming the two kinds
// it knew about. busybox-1.36.1-r31.apk then arrived carrying .post-install,
// .post-upgrade and .trigger — Alpine's install scripts — and all three were
// offered as files to install. That is wrong twice: they would be written into
// the root directory as files called ".post-install", and the scripts' actual
// purpose would never happen.
//
// Naming the special cases guarantees being wrong the next time one is added.
// The structural rule — dot-name, no directory separator — covers .PKGINFO,
// every .SIGN.* algorithm, all four script names and .trigger, and whatever
// Alpine introduces next.
//
// A package may legitimately ship something like usr/share/.hidden; the "no
// separator" test is what keeps that as payload.
//
// NOT YET EXPOSED: the install scripts are recognised and skipped, but nothing
// returns them to a caller yet. An installer needs them — .post-install is how a
// package configures itself — so this is a known gap, not a design choice. It is
// named here because "the file list is right" is not the same as "the package
// can be installed".
static bool apk_metadata_entry(const char *path)
{
    if (path[0] != '.') {
        return false;
    }

    for (af_size i = 1; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            return false;      // not at the root: ordinary payload
        }
    }

    return true;
}

// The dot-named root entries that must be RUN, as opposed to merely read.
//
// Returning AF_SCRIPT_NONE for anything unrecognised is deliberate: Alpine's
// rule for "is this metadata" is structural, but "is this a script, and when does
// it run" is a closed list defined by apk-tools. An unknown dot-name is
// therefore metadata that this build does not execute — which is worth knowing
// and is why the count of skipped names is not silently zero.
static af_script_kind_t apk_script_kind(const char *path)
{
    if (afpkg_strcmp(path, ".pre-install") == 0)    return AF_SCRIPT_PRE_INSTALL;
    if (afpkg_strcmp(path, ".post-install") == 0)   return AF_SCRIPT_POST_INSTALL;
    if (afpkg_strcmp(path, ".pre-upgrade") == 0)    return AF_SCRIPT_PRE_UPGRADE;
    if (afpkg_strcmp(path, ".post-upgrade") == 0)   return AF_SCRIPT_POST_UPGRADE;
    if (afpkg_strcmp(path, ".pre-deinstall") == 0)  return AF_SCRIPT_PRE_DEINSTALL;
    if (afpkg_strcmp(path, ".post-deinstall") == 0) return AF_SCRIPT_POST_DEINSTALL;
    if (afpkg_strcmp(path, ".trigger") == 0)        return AF_SCRIPT_TRIGGER;
    return AF_SCRIPT_NONE;
}

static af_status_t read_apk(const af_u8 *data, af_size len,
                            const afpkg_scratch_t *scratch, af_pkg_t *out)
{
    if (scratch == NULL) {
        out->error = "reading a compressed package needs a scratch buffer";
        return AF_ERR_INVAL;
    }

    if (data[0] != 0x1F || data[1] != 0x8B) {
        out->error = "not an Alpine package: it does not begin with a gzip member";
        return AF_ERR_INVAL;
    }

    af_pkg_info_t info;
    afpkg_memset(&info, 0, sizeof(info));

    // EVERY MEMBER IS PART OF ONE TAR, and this is where the first version of
    // this reader was wrong in a way no generated fixture could show.
    //
    // The reader treated each gzip member as its own tar and reported the
    // entries of the last one. That works when each member happens to hold a
    // whole archive, which is what the fixture did — the fixture was written
    // from the same misunderstanding, so it agreed with the code and both were
    // wrong.
    //
    // A real .apk from Alpine's CDN is ONE tar split across members at arbitrary
    // boundaries, including mid-entry, so that a signature can be checked before
    // the payload is read. The visible symptom was 11 entries reported instead
    // of 13. The dangerous version of the same bug is a package whose payload
    // straddles a boundary: the reader reports a partial file list and installs
    // a program with files missing.
    //
    // So the members are inflated one after another into consecutive space in
    // the scratch buffer, and the result is walked as a single tar.
    af_size offset = 0;
    af_size total = 0;
    af_size members = 0;
    const char *why = "";

    while (offset < len) {
        if (total >= scratch->size) {
            out->error = "the package's decompressed contents do not fit the "
                         "scratch buffer";
            return AF_ERR_TOOMANY;
        }

        afpkg_scratch_t sub;
        sub.base = scratch->base + total;
        sub.size = scratch->size - total;

        af_size out_len = 0;
        af_size consumed = 0;

        const af_status_t rc = afpkg_gunzip_member(data + offset, len - offset,
                                                   sub, &out_len, &consumed, &why);
        if (af_status_err(rc)) {
            // A truncated final member is common enough to be worth naming: an
            // interrupted download produces exactly this, and "corrupt" alone
            // sends the reader looking for a parsing bug.
            out->error = (offset > 0)
                ? "the package ends inside a gzip member — it is truncated"
                : why;
            return rc;
        }

        members++;
        total += out_len;

        if (consumed == 0) {
            out->error = "a gzip member consumed no input — malformed stream";
            return AF_ERR_FS_CORRUPT;
        }

        offset += consumed;
    }

    if (total == 0) {
        out->error = "the package decompressed to nothing";
        return AF_ERR_FS_CORRUPT;
    }

    // --- one walk, for the metadata and the scripts ---------------------------
    //
    // ONE PASS, because the tar is a stream and walking it twice to collect two
    // kinds of thing would double the cost for no benefit. It also cannot break
    // early at .PKGINFO any more: the scripts come after it.
    {
        af_size cursor = 0;
        af_pkg_entry_t entry;
        const af_u8 *contents = NULL;
        af_size contents_len = 0;
        bool found = false;
        af_u32 unknown_dot_names = 0;

        while (true) {
            const af_status_t walked =
                afpkg_tar_next(scratch->base, total, &cursor, &entry,
                               &contents, &contents_len, &why);
            if (walked == AF_ERR_AGAIN) {
                continue;
            }
            if (af_status_err(walked)) {
                break;
            }

            if (afpkg_strcmp(entry.path, ".PKGINFO") == 0) {
                if (contents_len > APK_PKGINFO_MAX) {
                    out->error = ".PKGINFO is implausibly large";
                    return AF_ERR_FS_CORRUPT;
                }
                afpkg_parse_pkginfo((const char *)contents, contents_len, &info);
                found = true;
                continue;
            }

            if (apk_metadata_entry(entry.path)) {
                const af_script_kind_t kind = apk_script_kind(entry.path);

                if (kind == AF_SCRIPT_NONE) {
                    // Metadata this build does not act on. Counted rather than
                    // ignored, so "we ran everything" is a claim the reader can
                    // support or not.
                    unknown_dot_names++;
                    continue;
                }

                if (info.script_count < AFPKG_MAX_SCRIPTS) {
                    info.scripts[info.script_count].kind = kind;
                    info.scripts[info.script_count].data = contents;
                    info.scripts[info.script_count].len = contents_len;
                    info.script_count++;
                } else {
                    out->error = "the package carries more install scripts than "
                                 "this build can represent";
                    return AF_ERR_TOOMANY;
                }
            }
        }

        if (!found) {
            out->error = "no .PKGINFO in the archive — this is a gzipped tar, "
                         "but it is not an Alpine package";
            return AF_ERR_INVAL;
        }

        if (unknown_dot_names > 0) {
            // Not an error. A package may carry metadata from a newer apk-tools,
            // and refusing the whole package over it would be worse than
            // installing it without that one hook.
            (void)unknown_dot_names;
        }
    }

    if (info.name[0] == '\0' || info.version[0] == '\0') {
        out->error = ".PKGINFO has no pkgname or pkgver";
        return AF_ERR_FS_CORRUPT;
    }

    out->info         = info;
    out->data_tar     = scratch->base;
    out->data_tar_len = total;
    out->format       = AF_BINFMT_APK_PKG;

    (void)members;
    return AF_OK;
}

// =============================================================================
// Reading a package
// =============================================================================
static af_status_t read_deb(const af_u8 *data, af_size len,
                            const afpkg_scratch_t *scratch, af_pkg_t *out)
{
    // --- the version marker ---------------------------------------------------
    //
    // "debian-binary" is the member that makes an ar archive a .deb. Its absence
    // means this is an ar archive of something else — object files, a static
    // library, mail — and treating it as a package would be a category error.
    const af_u8 *version = NULL;
    af_size version_len = 0;
    const char *why = "";

    af_status_t rc = afpkg_ar_find(data, len, "debian-binary",
                                   &version, &version_len, &why);
    if (af_status_err(rc)) {
        out->error = "ar archive with no debian-binary member — this is an ar "
                     "archive, but it is not a Debian package";
        return AF_ERR_INVAL;
    }

    if (version_len < 3 || version[0] != '2' || version[1] != '.') {
        // The format version is a claim by the file. A "3.x" package is a future
        // format, and reading it with 2.x rules is guessing.
        out->error = "unsupported deb format version (only 2.x is handled)";
        return AF_ERR_NOTSUP;
    }

    // --- the control member ---------------------------------------------------
    const af_u8 *control = NULL;
    af_size control_len = 0;

    rc = afpkg_ar_find_control(data, len, &control, &control_len, &why);
    if (af_status_err(rc)) {
        out->error = why;
        return rc;
    }

    const af_u8 *control_tar = control;
    af_size control_tar_len = control_len;

    // gzip when the member says .gz. The uncompressed case is legal and dpkg
    // accepts it, which is why both are handled rather than assuming.
    if (control_len >= 2 && control[0] == 0x1F && control[1] == 0x8B) {
        if (scratch == NULL) {
            out->error = "a compressed control member needs a scratch buffer";
            return AF_ERR_INVAL;
        }

        rc = afpkg_gunzip(control, control_len, *scratch, &control_tar_len, &why);
        if (af_status_err(rc)) {
            out->error = why;
            return rc;
        }
        control_tar = scratch->base;
    }

    // --- find ./control inside it ---------------------------------------------
    af_size cursor = 0;
    af_pkg_entry_t entry;
    const af_u8 *contents = NULL;
    af_size contents_len = 0;
    bool found = false;

    while (true) {
        rc = afpkg_tar_next(control_tar, control_tar_len, &cursor, &entry,
                            &contents, &contents_len, &why);
        if (rc == AF_ERR_AGAIN) {
            continue;                       // a skipped extension entry
        }
        if (af_status_err(rc)) {
            break;
        }
        if (afpkg_strcmp(entry.path, "control") == 0) {
            found = true;
            break;
        }
    }

    if (!found) {
        out->error = "the control archive contains no ./control file — the "
                     "package describes itself nowhere";
        return AF_ERR_FS_CORRUPT;
    }

    // The control file is text. It is not NUL-terminated in the archive, so it
    // is parsed with an explicit length — a parser that used af_strlen on it
    // would read past the end of the entry and into the next tar header.
    afpkg_parse_control((const char *)contents, contents_len, &out->info);

    if (out->info.name[0] == '\0') {
        out->error = "the control file has no Package field";
        return AF_ERR_FS_CORRUPT;
    }
    if (out->info.version[0] == '\0') {
        out->error = "the control file has no Version field";
        return AF_ERR_FS_CORRUPT;
    }

    // --- the data member, decompressed for the caller -------------------------
    const af_u8 *data_member = NULL;
    af_size data_member_len = 0;

    rc = afpkg_ar_find(data, len, "data.tar.gz", &data_member,
                       &data_member_len, &why);
    if (af_status_err(rc)) {
        rc = afpkg_ar_find(data, len, "data.tar", &data_member,
                           &data_member_len, &why);
        if (af_status_err(rc)) {
            out->error = why;
            return rc;
        }
    }

    if (data_member_len >= 2 && data_member[0] == 0x1F && data_member[1] == 0x8B) {
        if (scratch == NULL) {
            out->error = "a compressed data member needs a scratch buffer";
            return AF_ERR_INVAL;
        }

        af_size inflated = 0;
        rc = afpkg_gunzip(data_member, data_member_len, *scratch, &inflated, &why);
        if (af_status_err(rc)) {
            out->error = why;
            return rc;
        }
        out->data_tar     = scratch->base;
        out->data_tar_len = inflated;
    } else {
        out->data_tar     = data_member;
        out->data_tar_len = data_member_len;
    }

    out->format = AF_BINFMT_DEB;
    out->error  = "read as a Debian package";
    return AF_OK;
}

af_status_t afpkg_open(const af_u8 *data, af_size len,
                       const afpkg_scratch_t *scratch, af_pkg_t *out)
{
    if (out == NULL) {
        return AF_ERR_INVAL;
    }

    afpkg_memset(out, 0, sizeof(*out));
    out->data    = data;
    out->len     = len;
    out->error   = "not read";
    out->valid   = false;

    if (scratch != NULL) {
        out->scratch = *scratch;
    }

    if (data == NULL || len == 0) {
        out->error = "no data supplied";
        return AF_ERR_INVAL;
    }

    // The container decides the reader. A .deb is an ar archive; an Alpine .apk
    // is a chain of gzipped tars; a pacman package is a zstd tar. Adding a
    // format means adding a branch here and a function below, not restructuring
    // anything — which is the whole reason the layers were separated.
    af_status_t rc;

    if (len >= 2 && data[0] == 0x1F && data[1] == 0x8B) {
        rc = read_apk(data, len, scratch, out);
        out->valid = !af_status_err(rc);
        return rc;
    }

    rc = read_deb(data, len, scratch, out);

    out->valid = !af_status_err(rc);
    return rc;
}

const char *afpkg_script_name(af_script_kind_t kind)
{
    switch (kind) {
    case AF_SCRIPT_NONE:           return "none";
    case AF_SCRIPT_PRE_INSTALL:    return "pre-install";
    case AF_SCRIPT_POST_INSTALL:   return "post-install";
    case AF_SCRIPT_PRE_UPGRADE:    return "pre-upgrade";
    case AF_SCRIPT_POST_UPGRADE:   return "post-upgrade";
    case AF_SCRIPT_PRE_DEINSTALL:  return "pre-deinstall";
    case AF_SCRIPT_POST_DEINSTALL: return "post-deinstall";
    case AF_SCRIPT_TRIGGER:        return "trigger";
    }
    return "?";
}

const char *afpkg_error(const af_pkg_t *pkg)
{
    return (pkg != NULL && pkg->error != NULL) ? pkg->error : "no package";
}

void afpkg_iter_begin(afpkg_iter_t *it)
{
    it->cursor = 0;
}

bool afpkg_iter_next(const af_pkg_t *pkg, afpkg_iter_t *it, af_pkg_entry_t *out)
{
    if (pkg == NULL || !pkg->valid || pkg->data_tar == NULL || it == NULL) {
        return false;
    }

    const char *why = "";

    while (true) {
        const af_u8 *contents = NULL;
        af_size contents_len = 0;

        const af_status_t rc = afpkg_tar_next(pkg->data_tar, pkg->data_tar_len,
                                              &it->cursor, out, &contents,
                                              &contents_len, &why);
        if (rc == AF_ERR_AGAIN) {
            continue;
        }
        if (af_status_err(rc)) {
            return false;
        }

        if (pkg->format == AF_BINFMT_APK_PKG && apk_metadata_entry(out->path)) {
            continue;
        }

        return true;
    }
}
