// SPDX-License-Identifier: MIT
//
// =============================================================================
// STATUS: PARTIAL, AND IN THE TEST GATE WITH ITS LIMITS ASSERTED.
//
// The xz CONTAINER is complete: stream header, stream footer, block headers with
// their filter lists, block padding, the check field, the index and its records,
// and the CRC32/CRC64 that guard every one of them. The LZMA2 CHUNK layer is
// complete for uncompressed chunks, which is a legal and reachable encoding of
// any stream whose data does not compress.
//
// LZMA-COMPRESSED CHUNKS ARE NOT DECODED. They are refused with AF_ERR_NOTSUP
// and a message naming the chunk type, never guessed at. That is the whole of
// what is missing, and it is one function: the range decoder and the LZMA state
// machine. See docs/architecture/xz-lzma2.md for what it involves and why it is
// a milestone of its own.
//
// WHY THE SPLIT IS WORTH COMMITTING RATHER THAN WAITING
//
// This is the third time this project has built a decompressor in this order —
// stored DEFLATE first, raw and RLE Zstandard blocks first, uncompressed LZMA2
// chunks first — and it is deliberate. The container around a codec is where the
// framing bugs live, and framing bugs are found by vectors, not by reading.
// Getting the container verified while the codec is still missing means the
// codec, when it lands, has exactly one place to be wrong.
//
// It also means the refusal is precise today: a user handing this a real .deb
// gets "the block uses an LZMA-compressed LZMA2 chunk, which this build does not
// decode" rather than a wrong answer or a vague failure.
//
// THE TRAP THIS FORMAT HAS THAT THE OTHERS DO NOT
//
// LZMA2 has no published prose specification. The .xz CONTAINER does — the
// container layer here was written from tukaani's xz-file-format.txt, including
// its reference CRC code — but the chunk format and the LZMA algorithm are
// defined by the reference implementation (the LZMA SDK and xz-embedded, both
// public domain / 0BSD). So ADR-015's rule matters more here than it did for
// DEFLATE or Zstandard: there is nothing to check an implementation against
// except vectors from the reference encoder. Reading the reference decoder to
// learn the format is how everybody learns it; passing its vectors is the only
// thing that says the reading was right.
// =============================================================================
//
// AfriyieOS — xz decoding
//
// .xz is what a large share of current Debian, Ubuntu and Fedora packages use,
// and it is the last common package container still unread.

#include "afpkg.h"

#ifdef XZ_TRACE
#include <stdio.h>
#define XTRACE(...) do { printf("  [xz] " __VA_ARGS__); printf("\n"); } while (0)
#else
#define XTRACE(...) do { } while (0)
#endif

// -----------------------------------------------------------------------------
// The two checks the format uses
//
// CRC32 is the same polynomial gzip uses with the same reflected convention, but
// the framing is not: the .xz specification computes it over a field with a zero
// seed, and the stream flags are CRC'd BEFORE they are trusted. That ordering is
// what lets a decoder tell a corrupt file from an unsupported one instead of
// reporting the second for both.
//
// CRC64 uses a different polynomial (0xC96C5795D7870F42, reflected) and is what
// the reference tools write by default, so it is not deferrable: a decoder that
// handled only CRC32 streams would refuse essentially every .xz file in
// existence.
//
// Bitwise rather than table-driven. A CRC64 table is 2 KiB of .rodata and this
// is eight shifts per byte; at package sizes the difference is not measurable and
// a table is one more thing to get wrong.
// -----------------------------------------------------------------------------
static af_u32 xz_crc32(const af_u8 *p, af_size n, af_u32 crc)
{
    crc = ~crc;
    for (af_size i = 0; i < n; i++) {
        crc ^= (af_u32)p[i];
        for (af_u32 k = 0; k < 8; k++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return ~crc;
}

static af_u64 xz_crc64(const af_u8 *p, af_size n, af_u64 crc)
{
    crc = ~crc;
    for (af_size i = 0; i < n; i++) {
        crc ^= (af_u64)p[i];
        for (af_u32 k = 0; k < 8; k++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xC96C5795D7870F42ull)
                             : (crc >> 1);
        }
    }
    return ~crc;
}

static af_u32 rd32(const af_u8 *p)
{
    return (af_u32)p[0] | ((af_u32)p[1] << 8) | ((af_u32)p[2] << 16) |
           ((af_u32)p[3] << 24);
}

static af_u64 rd64(const af_u8 *p)
{
    af_u64 v = 0;
    for (af_u32 i = 0; i < 8; i++) {
        v |= (af_u64)p[i] << (8u * i);
    }
    return v;
}

// -----------------------------------------------------------------------------
// Variable-length integers
//
// The .xz encoding of "a number that is usually small": seven bits per byte,
// high bit set on every byte but the last. Two properties the format REQUIRES and
// a decoder must therefore check:
//
//   * the last byte is never 0x00, because a 0x00 with the high bit clear would
//     encode the same number two ways; and
//   * at most nine bytes, because the value is limited to 63 bits.
//
// A decoder that skips those checks accepts streams the format calls invalid and
// cannot tell a truncated field from a complete one.
// -----------------------------------------------------------------------------
static af_status_t xz_vli(const af_u8 *d, af_size limit, af_size *at, af_u64 *out,
                          const char **why)
{
    if (*at >= limit) {
        *why = "an .xz variable-length integer runs past the end of the file";
        return AF_ERR_FS_CORRUPT;
    }

    af_u64 v = (af_u64)(d[*at] & 0x7Fu);
    af_u32 shift = 0;
    af_size i = 0;

    while (d[*at + i] & 0x80u) {
        i++;
        if (*at + i >= limit) {
            *why = "an .xz variable-length integer runs past the end of the file";
            return AF_ERR_FS_CORRUPT;
        }
        if (i >= 9) {
            *why = "an .xz variable-length integer is longer than nine bytes";
            return AF_ERR_FS_CORRUPT;
        }
        if (d[*at + i] == 0x00) {
            *why = "an .xz variable-length integer is not minimally encoded";
            return AF_ERR_FS_CORRUPT;
        }
        shift += 7;
        v |= (af_u64)(d[*at + i] & 0x7Fu) << shift;
    }

    *at += i + 1;
    *out = v;
    return AF_OK;
}

// -----------------------------------------------------------------------------
// Output
// -----------------------------------------------------------------------------
typedef struct {
    af_u8  *base;
    af_size size;
    af_size used;
    bool    full;
} xout_t;

static void xout_byte(xout_t *o, af_u8 v)
{
    if (o->used >= o->size) {
        o->full = true;
        return;
    }
    o->base[o->used++] = v;
}

// =============================================================================
// The LZMA2 chunk layer
//
// A block's Compressed Data, when its filter is LZMA2, is a sequence of chunks.
// Each starts with a control byte that says what follows:
//
//   0x00        end of the LZMA2 stream
//   0x01        dictionary reset, then an UNCOMPRESSED chunk
//   0x02        an UNCOMPRESSED chunk, no reset
//   0x03..0x7F  reserved and invalid
//   0x80..0xFF  an LZMA-compressed chunk; the low five bits are the top five
//               bits of the uncompressed size and the top three bits say which
//               resets happen first:
//                 0x80  nothing is reset
//                 0xA0  state reset, old properties
//                 0xC0  state reset, new properties follow
//                 0xE0  dictionary reset, state reset, new properties
//
// An uncompressed chunk is a three-byte header — control, then the size minus
// one as a big-endian 16-bit value — followed by that many bytes copied
// verbatim. It exists because LZMA2 must not expand incompressible data, and the
// reference encoder emits one whenever a chunk would not compress. That makes it
// reachable from a real file rather than a theoretical branch, which is the only
// reason it is worth having before the codec is written.
//
// The dictionary is reset at the start of each BLOCK, so the "first chunk must
// reset" rule is per block, not per file.
// =============================================================================
static af_status_t lzma2_chunks(const af_u8 *d, af_size len, xout_t *out,
                                af_size *consumed, const char **why)
{
    af_size at = 0;
    bool have_dict = false;

    while (true) {
        if (at >= len) {
            *why = "the LZMA2 chunk stream ends without its end marker";
            return AF_ERR_FS_CORRUPT;
        }

        const af_u8 control = d[at++];

        if (control == 0x00) {
            *consumed = at;
            return AF_OK;
        }

        if (control >= 0xE0u || control == 0x01) {
            have_dict = true;                 // dictionary reset
        } else if (!have_dict) {
            *why = "the LZMA2 stream does not begin with a dictionary reset, "
                   "which the format requires";
            return AF_ERR_FS_CORRUPT;
        }

        if (control >= 0x80u) {
            *why = "the block uses an LZMA-compressed LZMA2 chunk, which this "
                   "build does not decode";
            return AF_ERR_NOTSUP;
        }

        if (control > 0x02u) {
            *why = "the LZMA2 chunk stream contains a reserved control byte";
            return AF_ERR_FS_CORRUPT;
        }

        // Uncompressed chunk: two size bytes, big-endian, minus one.
        if (at + 2 > len) {
            *why = "an LZMA2 uncompressed chunk header is truncated";
            return AF_ERR_FS_CORRUPT;
        }
        const af_u32 size = (((af_u32)d[at] << 8) | (af_u32)d[at + 1]) + 1u;
        at += 2;

        if (at + size > len) {
            *why = "an LZMA2 uncompressed chunk runs past the end of the block";
            return AF_ERR_FS_CORRUPT;
        }

        for (af_u32 i = 0; i < size; i++) {
            xout_byte(out, d[at + i]);
        }
        if (out->full) {
            *why = "the decompressed stream does not fit the output buffer";
            return AF_ERR_TOOMANY;
        }

        at += size;
        XTRACE("uncompressed chunk: %u bytes, next at %lu", size,
               (unsigned long)at);
    }
}

// =============================================================================
// The stream
// =============================================================================

// How many blocks a stream may have before this build stops tracking them for the
// index check. Real packages have one to ten. Named rather than silently ignored,
// because the alternative — skipping index verification on large streams — would
// mean the check exists for small files and quietly does not for big ones.
#define XZ_MAX_TRACKED_BLOCKS 256

// The check type and its size, or -1 for one this build does not support.
static int xz_check_size(af_u8 id)
{
    switch (id) {
    case 0x00: return 0;      // None
    case 0x01: return 4;      // CRC32
    case 0x04: return 8;      // CRC64
    default:   return -1;     // reserved, or SHA-256 (0x0A)
    }
}

af_status_t afpkg_xz(const af_u8 *data, af_size len,
                     af_u8 *out_base, af_size out_size,
                     af_size *out_len, const char **why)
{
    if (out_len != NULL) {
        *out_len = 0;
    }

    if (data == NULL || out_base == NULL || out_len == NULL || len < 24) {
        *why = "no input or output buffer, or the file is too short to be .xz";
        return AF_ERR_INVAL;
    }

    static const af_u8 magic[6] = { 0xFD, '7', 'z', 'X', 'Z', 0x00 };
    for (af_u32 i = 0; i < 6; i++) {
        if (data[i] != magic[i]) {
            *why = "not an .xz file: the header magic bytes are wrong";
            return AF_ERR_INVAL;
        }
    }

    // --- Stream Header --------------------------------------------------------
    const af_u8 flags0 = data[6];
    const af_u8 flags1 = data[7];

    // The flags are CRC'd before they are believed, so "corrupt" and
    // "unsupported" stay distinguishable.
    if (xz_crc32(data + 6, 2, 0) != rd32(data + 8)) {
        *why = "the .xz stream header's CRC32 does not match its flags — the "
               "file is corrupt";
        return AF_ERR_FS_CORRUPT;
    }

    if (flags0 != 0x00 || (flags1 & 0xF0u) != 0) {
        *why = "the .xz stream flags have reserved bits set, so this file uses a "
               "feature this build does not know about";
        return AF_ERR_NOTSUP;
    }

    const int check_size = xz_check_size(flags1 & 0x0Fu);
    if (check_size < 0) {
        *why = "the .xz stream uses a check type this build does not support";
        return AF_ERR_NOTSUP;
    }

    xout_t out;
    out.base = out_base;
    out.size = out_size;
    out.used = 0;
    out.full = false;

    af_size at = 12;

    af_u64 rec_unpadded[XZ_MAX_TRACKED_BLOCKS];
    af_u64 rec_uncompressed[XZ_MAX_TRACKED_BLOCKS];
    af_u32 blocks = 0;

    // --- Blocks ---------------------------------------------------------------
    while (true) {
        if (at >= len) {
            *why = "the .xz file ends before its index";
            return AF_ERR_FS_CORRUPT;
        }

        // The Index Indicator is 0x00 and occupies the same byte position as the
        // Block Header Size field — which is how a decoder knows the blocks have
        // ended.
        if (data[at] == 0x00) {
            break;
        }

        if (blocks >= XZ_MAX_TRACKED_BLOCKS) {
            *why = "the .xz stream has more blocks than this build tracks for its "
                   "index check";
            return AF_ERR_NOTSUP;
        }

        const af_size block_start = at;
        const af_size header_size = ((af_size)data[at] + 1) * 4;

        if (header_size < 8 || header_size > len - block_start) {
            *why = "the .xz block header size is out of range";
            return AF_ERR_FS_CORRUPT;
        }

        const af_size header_end = block_start + header_size;

        // The header's CRC32 covers everything but itself and is checked BEFORE
        // the contents are parsed. That ordering is the format's.
        if (xz_crc32(data + block_start, header_size - 4, 0) !=
            rd32(data + header_end - 4)) {
            *why = "an .xz block header's CRC32 does not match — the file is "
                   "corrupt";
            return AF_ERR_FS_CORRUPT;
        }

        af_size p = block_start + 1;
        const af_u8 bflags = data[p++];

        if ((bflags & 0x3Cu) != 0) {
            *why = "an .xz block header has reserved flag bits set";
            return AF_ERR_FS_CORRUPT;
        }

        const af_u32 n_filters = (af_u32)(bflags & 0x03u) + 1u;
        af_u64 compressed_size = 0;
        af_u64 declared_uncompressed = 0;
        af_status_t rc;

        if (bflags & 0x40u) {
            rc = xz_vli(data, header_end, &p, &compressed_size, why);
            if (af_status_err(rc)) {
                return rc;
            }
        }
        if (bflags & 0x80u) {
            rc = xz_vli(data, header_end, &p, &declared_uncompressed, why);
            if (af_status_err(rc)) {
                return rc;
            }
        }

        // --- the filter chain -------------------------------------------------
        af_u64 filter_id = 0;
        af_u64 props_size = 0;
        const af_u8 *props = NULL;

        for (af_u32 f = 0; f < n_filters; f++) {
            af_u64 id = 0;
            af_u64 psz = 0;

            rc = xz_vli(data, header_end, &p, &id, why);
            if (af_status_err(rc)) {
                return rc;
            }
            rc = xz_vli(data, header_end, &p, &psz, why);
            if (af_status_err(rc)) {
                return rc;
            }
            if (psz > header_end - p) {
                *why = "an .xz filter's properties run past the block header";
                return AF_ERR_FS_CORRUPT;
            }

            if (f == 0) {
                filter_id = id;
                props_size = psz;
                props = data + p;
            }
            p += (af_size)psz;
        }

        if (n_filters != 1) {
            *why = "the .xz block chains several filters, which this build does "
                   "not support";
            return AF_ERR_NOTSUP;
        }
        if (filter_id != 0x21) {
            // 0x03 is delta; 0x04..0x0B are the branch/call/jump filters. Saying
            // which family it is beats saying "unsupported".
            *why = "the .xz block uses a filter other than LZMA2 — the delta and "
                   "branch/call/jump filters are not supported";
            return AF_ERR_NOTSUP;
        }
        if (props_size != 1) {
            *why = "the LZMA2 filter's properties are not one byte";
            return AF_ERR_FS_CORRUPT;
        }
        if (props[0] > 40) {
            *why = "the LZMA2 filter declares a dictionary larger than 4 GiB";
            return AF_ERR_FS_CORRUPT;
        }

        for (af_size k = p; k < header_end - 4; k++) {
            if (data[k] != 0x00) {
                *why = "an .xz block header's padding is not zero";
                return AF_ERR_FS_CORRUPT;
            }
        }

        // --- Compressed Data ---------------------------------------------------
        const af_size data_start = header_end;
        const af_size avail = len - data_start;
        af_size limit = (compressed_size != 0) ? (af_size)compressed_size : avail;

        if (limit > avail) {
            *why = "the block's declared compressed size runs past the end of "
                   "the file";
            return AF_ERR_FS_CORRUPT;
        }

        const af_size out_before = out.used;
        af_size chunk_used = 0;

        rc = lzma2_chunks(data + data_start, limit, &out, &chunk_used, why);
        if (af_status_err(rc)) {
            return rc;
        }

        if (compressed_size != 0 && chunk_used != (af_size)compressed_size) {
            *why = "the LZMA2 chunk stream is shorter than the block's declared "
                   "compressed size";
            return AF_ERR_FS_CORRUPT;
        }

        const af_size produced = out.used - out_before;

        if (declared_uncompressed != 0 &&
            declared_uncompressed != (af_u64)produced) {
            *why = "the block's uncompressed size does not match the block "
                   "header";
            return AF_ERR_FS_CORRUPT;
        }

        // --- Block Padding ----------------------------------------------------
        //
        // Padding is NOT part of Unpadded Size, which is what the index records.
        // It is the one field in the whole format whose name says what it is and
        // whose arithmetic is still easy to get wrong: the block's size on disk
        // and the block's size in the index differ by up to three bytes.
        af_size q = data_start + chunk_used;
        while ((q - block_start) % 4u != 0u) {
            if (q >= len) {
                *why = "the .xz block padding runs past the end of the file";
                return AF_ERR_FS_CORRUPT;
            }
            if (data[q] != 0x00) {
                *why = "the .xz block padding is not zero";
                return AF_ERR_FS_CORRUPT;
            }
            q++;
        }

        // --- Check ------------------------------------------------------------
        //
        // Computed over the bytes THIS BLOCK produced, which is why the output
        // position was recorded before the block rather than reconstructed
        // after it. A check over the whole output would pass for a stream whose
        // blocks were decoded into the wrong places.
        if (q + (af_size)check_size > len) {
            *why = "the .xz block check field runs past the end of the file";
            return AF_ERR_FS_CORRUPT;
        }

        if (check_size == 4) {
            if (xz_crc32(out_base + out_before, produced, 0) != rd32(data + q)) {
                *why = "a block's CRC32 does not match its decompressed bytes — "
                       "the file is corrupt";
                return AF_ERR_FS_CORRUPT;
            }
        } else if (check_size == 8) {
            if (xz_crc64(out_base + out_before, produced, 0) != rd64(data + q)) {
                *why = "a block's CRC64 does not match its decompressed bytes — "
                       "the file is corrupt";
                return AF_ERR_FS_CORRUPT;
            }
        }

        // Block header + compressed data + check, with the padding left out.
        rec_unpadded[blocks] =
            (af_u64)((data_start + chunk_used) - block_start) +
            (af_u64)check_size;
        rec_uncompressed[blocks] = (af_u64)produced;
        blocks++;

        XTRACE("block %u: %lu bytes in, %lu out, unpadded %llu", blocks,
               (unsigned long)chunk_used, (unsigned long)produced,
               (unsigned long long)rec_unpadded[blocks - 1]);

        at = q + (af_size)check_size;
    }

    // --- Index ----------------------------------------------------------------
    //
    // The index is a CLAIM about the blocks that were just decoded, and it is
    // checkable, so it is checked. A decoder that skips it accepts a file whose
    // index disagrees with its contents — which is exactly the shape of a
    // truncated or spliced stream that still happens to decompress.
    const af_size index_start = at;
    at++;                                  // the 0x00 indicator, already seen

    af_u64 n_records = 0;
    af_status_t rc = xz_vli(data, len, &at, &n_records, why);
    if (af_status_err(rc)) {
        return rc;
    }

    if (n_records != (af_u64)blocks) {
        *why = "the .xz index describes a different number of blocks than the "
               "stream contains";
        return AF_ERR_FS_CORRUPT;
    }

    for (af_u32 i = 0; i < blocks; i++) {
        af_u64 unpadded = 0, uncompressed = 0;

        rc = xz_vli(data, len, &at, &unpadded, why);
        if (af_status_err(rc)) {
            return rc;
        }
        rc = xz_vli(data, len, &at, &uncompressed, why);
        if (af_status_err(rc)) {
            return rc;
        }

        if (unpadded != rec_unpadded[i] ||
            uncompressed != rec_uncompressed[i]) {
            *why = "an .xz index record does not match the block it describes — "
                   "the file is corrupt";
            return AF_ERR_FS_CORRUPT;
        }
    }

    while ((at - index_start) % 4u != 0u) {
        if (at >= len || data[at] != 0x00) {
            *why = "the .xz index padding is not zero";
            return AF_ERR_FS_CORRUPT;
        }
        at++;
    }

    if (at + 4 > len) {
        *why = "the .xz index has no CRC32";
        return AF_ERR_FS_CORRUPT;
    }
    if (xz_crc32(data + index_start, at - index_start, 0) != rd32(data + at)) {
        *why = "the .xz index CRC32 does not match — the file is corrupt";
        return AF_ERR_FS_CORRUPT;
    }
    at += 4;

    const af_size index_size = at - index_start;

    // --- Stream Footer --------------------------------------------------------
    if (at + 12 > len) {
        *why = "the .xz file has no stream footer";
        return AF_ERR_FS_CORRUPT;
    }
    if (xz_crc32(data + at + 4, 6, 0) != rd32(data + at)) {
        *why = "the .xz stream footer's CRC32 does not match — the file is "
               "corrupt";
        return AF_ERR_FS_CORRUPT;
    }
    if ((rd32(data + at + 4) + 1u) * 4u != index_size) {
        *why = "the .xz stream footer's backward size does not match the index";
        return AF_ERR_FS_CORRUPT;
    }
    if (data[at + 8] != flags0 || data[at + 9] != flags1) {
        *why = "the .xz stream header and footer declare different flags";
        return AF_ERR_FS_CORRUPT;
    }
    if (data[at + 10] != 'Y' || data[at + 11] != 'Z') {
        *why = "the .xz stream footer's magic bytes are missing, so the file is "
               "incomplete";
        return AF_ERR_FS_CORRUPT;
    }
    at += 12;

    // Stream padding is only zero bytes in a multiple of four. Anything else is
    // either a second concatenated stream, which this build does not follow, or
    // garbage.
    if (at < len) {
        if ((len - at) % 4u != 0u) {
            *why = "there is data after the .xz stream that is not whole stream "
                   "padding — the file is corrupt or concatenated";
            return AF_ERR_FS_CORRUPT;
        }
        for (af_size i = at; i < len; i++) {
            if (data[i] != 0x00) {
                *why = "there is data after the .xz stream that is not whole "
                       "stream padding — the file is corrupt or concatenated";
                return AF_ERR_FS_CORRUPT;
            }
        }
    }

    *out_len = out.used;
    *why = "decompressed";
    return AF_OK;
}
