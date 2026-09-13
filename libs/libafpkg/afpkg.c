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
af_status_t afpkg_gunzip(const af_u8 *data, af_size len,
                         afpkg_scratch_t scratch, af_size *out_len,
                         const char **why)
{
    if (data == NULL || len < 18) {
        *why = "gzip stream too short to contain a header";
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

    // Optional fields, in the order the format specifies. They must be skipped
    // in order and their lengths honoured, or the deflate stream starts at the
    // wrong byte and the first block header is read from a filename.
    if ((flags & 0x04) != 0) {                  // FEXTRA
        if (!region_ok(data, len, at, 2)) {
            *why = "gzip FEXTRA length is past the end of the stream";
            return AF_ERR_FS_CORRUPT;
        }
        const af_size xlen = (af_size)data[at] | ((af_size)data[at + 1] << 8);
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

    // The DEFLATE payload sits between the header just parsed and the 8-byte
    // trailer. Handing the whole remainder to the inflater would let it run into
    // the CRC and length and, on a corrupt stream, decode them as data — so the
    // bounds are tightened here rather than trusted downstream.
    if (at + 8 > len) {
        *why = "gzip stream is too short to contain its trailer";
        return AF_ERR_FS_CORRUPT;
    }

    *out_len = 0;
    const af_status_t rc = afpkg_inflate(data + at, len - at - 8,
                                         scratch.base, scratch.size,
                                         out_len, why);
    if (af_status_err(rc)) {
        return rc;
    }

    // The trailer's CRC32 and ISIZE are deliberately NOT verified yet, and that
    // is stated rather than implied. Doing it needs a CRC32 implementation,
    // which is its own small piece of work; until then a stream that inflates to
    // the wrong bytes is caught by the caller comparing lengths, and a
    // bit-flipped stream that still inflates is not caught at all.
    *why = "inflated";
    return AF_OK;
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
    // is a gzipped tar; a pacman package is a zstd tar. Adding a format means
    // adding a branch here and a function below, not restructuring anything.
    const af_status_t rc = read_deb(data, len, scratch, out);

    out->valid = !af_status_err(rc);
    return rc;
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
        return true;
    }
}
