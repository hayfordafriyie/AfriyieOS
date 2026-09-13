// SPDX-License-Identifier: MIT
// AfriyieOS — DEFLATE
//
// RFC 1951, in full: stored, fixed-Huffman and dynamic-Huffman blocks, with LZ77
// back-references. This is what turns libafpkg from a reader that works on its
// own fixtures into a reader that works on Ubuntu's archive.
//
// WHY THIS IS THE VERSION WORTH WRITING
//
// The first version handled stored blocks only, which is legal DEFLATE and
// useless in practice: every real compressor emits Huffman codes, and a
// package built by `dpkg-deb -Zgzip` on this very machine was refused with
//
//     gzip stream uses Huffman-coded blocks, which this build does not inflate
//
// That refusal was correct and is exactly why it existed — but a package manager
// that cannot read any real package is not a package manager.
//
// THE SHAPE OF THE BUGS THIS CODE HAS
//
// A decompressor is the one kind of code where producing output means nothing.
// Wrong bytes look exactly like right bytes until they are compared, and the
// failures cluster in three places:
//
//   1. BIT ORDER. DEFLATE packs Huffman codes most-significant-bit first and
//      everything else least-significant-bit first, in the same stream, with no
//      marker. Getting this wrong produces output that is wrong only when codes
//      are longer than a byte.
//   2. THE CODE-LENGTH ALPHABET. Dynamic blocks transmit their own tables,
//      themselves Huffman-coded, using a 19-symbol alphabet in a scrambled
//      order, with 16/17/18 as repeat codes. Every one of those is a chance to
//      be off by one.
//   3. OVERLAPPING COPIES. A back-reference may point INTO the bytes it is
//      currently writing — that is how run-length encoding works here — so the
//      copy must be byte at a time, not memcpy. Using memcpy passes every test
//      whose data has no long runs and corrupts the ones that do.
//
// All three are covered by the generated vectors rather than left to luck. See
// tools/pkgsynth.py for why the payloads are the ones they are.

#include "afpkg.h"

// Local, because afpkg.c's copy is static to that file. Two one-line functions
// in two files is the right trade against either exporting a helper that has
// nothing to do with this file's job, or making the reader's bounds check part
// of the library's public surface.
static bool deflate_region_ok(const af_u8 *data, af_size len, af_size at,
                              af_size n)
{
    (void)data;
    return at <= len && (len - at) >= n;
}

// -----------------------------------------------------------------------------
// Bit reader
//
// DEFLATE bit order, stated once here because it is the thing everything else
// depends on: bits are consumed from the least-significant end of each byte, and
// multi-bit VALUES (lengths, distances, extra bits) are read least-significant
// bit first. Huffman CODES are the exception and are assembled most-significant
// bit first — which is handled in huff_decode, not here.
// -----------------------------------------------------------------------------
typedef struct {
    const af_u8 *data;
    af_size      len;
    af_size      byte;        // next byte to load

    af_u32       buffer;      // bits not yet consumed, right-aligned
    af_u32       count;       // how many valid bits are in `buffer`

    bool         overrun;     // asked for more bits than exist
} bitreader_t;

static void br_init(bitreader_t *br, const af_u8 *data, af_size len)
{
    br->data    = data;
    br->len     = len;
    br->byte    = 0;
    br->buffer  = 0;
    br->count   = 0;
    br->overrun = false;
}

// Reads `n` bits, least-significant first. n is at most 16 in DEFLATE.
static af_u32 br_bits(bitreader_t *br, af_u32 n)
{
    if (n == 0) {
        return 0;
    }

    while (br->count < n) {
        if (br->byte >= br->len) {
            // Ran out. The overrun flag is set rather than the read being
            // refused, so the caller gets a defined value and one chance to
            // notice — a stream that ends mid-symbol must be rejected, not
            // decoded from zeroes.
            br->overrun = true;
            return 0;
        }
        br->buffer |= (af_u32)br->data[br->byte++] << br->count;
        br->count += 8;
    }

    const af_u32 value = br->buffer & ((1u << n) - 1u);
    br->buffer >>= n;
    br->count -= n;
    return value;
}

static af_u32 br_bit(bitreader_t *br)
{
    return br_bits(br, 1);
}

// Aligns to a byte boundary and pushes the buffered WHOLE bytes back onto the
// stream, so the caller can read them as plain bytes.
//
// This is subtler than it looks. The bit reader loads bytes eagerly, so after
// aligning there are usually 8 or 16 bits sitting in `buffer` that have not been
// consumed and that belong to the bytes immediately following. Discarding them
// loses the beginning of the stored block; keeping them in the bit buffer while
// ALSO reading bytes directly from the stream reads the same bytes twice.
//
// The bit buffer holds them in load order — the first-loaded byte in the low
// bits — so unreading is a loop that steps `byte` back and drops eight bits off
// the bottom each time. The result is that `data[byte]` is the next byte, which
// is exactly the invariant the rest of the reader relies on.
static void br_align_unread(bitreader_t *br)
{
    const af_u32 drop = br->count % 8u;
    br->buffer >>= drop;
    br->count -= drop;

    while (br->count >= 8) {
        br->byte--;
        br->count -= 8;
        br->buffer >>= 8;
    }
}

// -----------------------------------------------------------------------------
// Huffman tables
//
// Canonical codes: for each code length, the symbols form a contiguous block of
// code values in symbol order. That is the whole reason DEFLATE can transmit a
// table as nothing but a list of lengths — the codes themselves are derivable.
//
// The decode below is the standard "count and first-code" walk, which needs no
// lookup table and no allocation. A table-driven decoder is faster; this one is
// four lines and cannot be wrong about its own bounds.
// -----------------------------------------------------------------------------
#define HUFF_MAX_BITS  15
#define HUFF_MAX_SYMS  288

typedef struct {
    af_u16 counts[HUFF_MAX_BITS + 1];   // how many codes of each length
    af_u16 symbols[HUFF_MAX_SYMS];      // symbols, sorted by (length, symbol)
} huff_t;

// Returns 0 on success, negative on an over-subscribed table, positive when the
// table is INCOMPLETE — which is legal for a distance table with one code and
// illegal elsewhere. The caller decides; collapsing the two here would hide a
// real error inside a case that is allowed.
static int huff_build(huff_t *h, const af_u8 *lengths, af_u32 n)
{
    for (af_u32 i = 0; i <= HUFF_MAX_BITS; i++) {
        h->counts[i] = 0;
    }
    for (af_u32 i = 0; i < n; i++) {
        h->counts[lengths[i]]++;
    }

    if (h->counts[0] == n) {
        // No codes at all. Legal only for a distance table in a block that
        // never references a distance, and the caller has to know, because
        // huff_decode on this table can never succeed.
        return (int)n;
    }

    // Kraft inequality: each level doubles the space, codes consume it. Going
    // negative means the table claims more codes than the code space can hold —
    // a corrupt stream, and one that would otherwise be decoded into nonsense.
    int left = 1;
    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        left <<= 1;
        left -= (int)h->counts[len];
        if (left < 0) {
            return -1;
        }
    }

    af_u16 offsets[HUFF_MAX_BITS + 2];
    offsets[1] = 0;
    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        offsets[len + 1] = (af_u16)(offsets[len] + h->counts[len]);
    }

    for (af_u32 i = 0; i < n; i++) {
        if (lengths[i] != 0) {
            h->symbols[offsets[lengths[i]]++] = (af_u16)i;
        }
    }

    return left;    // >0 means incomplete
}

// Decodes one symbol. Returns -1 when the bits match no code — which happens on
// a corrupt stream and must be an error rather than a symbol.
static int huff_decode(bitreader_t *br, const huff_t *h)
{
    int code  = 0;
    int first = 0;
    int index = 0;

    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        code |= (int)br_bit(br);
        if (br->overrun) {
            return -1;
        }

        const int count = (int)h->counts[len];
        if (code - first < count) {
            return (int)h->symbols[index + (code - first)];
        }

        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }

    return -1;
}

// -----------------------------------------------------------------------------
// The fixed Huffman tables (RFC 1951 §3.2.6)
//
// Written as a function rather than a constant array so the code-length list is
// visible: 0-143 are 8 bits, 144-255 are 9, 256-279 are 7, 280-287 are 8. Those
// four ranges are the entire definition, and a table built from a transcription
// of the resulting bytes is a table nobody can check.
// -----------------------------------------------------------------------------
static void build_fixed_literal(huff_t *h)
{
    af_u8 lengths[288];

    for (int i = 0; i < 144; i++) lengths[i] = 8;
    for (int i = 144; i < 256; i++) lengths[i] = 9;
    for (int i = 256; i < 280; i++) lengths[i] = 7;
    for (int i = 280; i < 288; i++) lengths[i] = 8;

    (void)huff_build(h, lengths, 288);
}

static void build_fixed_distance(huff_t *h)
{
    af_u8 lengths[30];
    for (int i = 0; i < 30; i++) {
        lengths[i] = 5;
    }
    (void)huff_build(h, lengths, 30);
}

// -----------------------------------------------------------------------------
// Length and distance tables (RFC 1951 §3.2.5)
//
// base is the smallest value in the code's range; extra is how many following
// bits are added to it. Code 285 is the special case: 258 bytes, no extra bits,
// which is why len_extra ends in 0 while the entry before it uses 5.
// -----------------------------------------------------------------------------
static const af_u16 s_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const af_u8 s_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const af_u16 s_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const af_u8 s_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

// The order dynamic blocks transmit code-length code lengths in (RFC 1951
// §3.2.7). It is deliberately scrambled — the common lengths come first — and
// using 0..18 instead produces a table that decodes a few blocks correctly and
// then diverges, which is the worst possible failure signature.
static const af_u8 s_clc_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

// -----------------------------------------------------------------------------
// Output
// -----------------------------------------------------------------------------
typedef struct {
    af_u8  *base;
    af_size size;
    af_size used;
    bool    full;
} sink_t;

static void sink_byte(sink_t *out, af_u8 value)
{
    if (out->used >= out->size) {
        out->full = true;
        return;
    }
    out->base[out->used++] = value;
}

// A back-reference.
//
// BYTE AT A TIME, DELIBERATELY. `distance` may be smaller than `length`, which
// means the copy reads bytes this loop wrote moments ago — that is what makes
// "aaaaaaaa" compress to a handful of bits. A memcpy of the whole span reads
// beyond what has been written and produces garbage for exactly the inputs that
// compress best.
static void sink_copy(sink_t *out, af_size distance, af_size length)
{
    for (af_size i = 0; i < length; i++) {
        if (out->used < distance) {
            out->full = true;
            return;
        }
        sink_byte(out, out->base[out->used - distance]);
    }
}

// -----------------------------------------------------------------------------
// One block
// -----------------------------------------------------------------------------
static af_status_t inflate_block(bitreader_t *br, sink_t *out,
                                 const huff_t *lit, const huff_t *dist,
                                 const char **why)
{
    while (true) {
        const int symbol = huff_decode(br, lit);
        if (symbol < 0) {
            *why = "DEFLATE stream contains an invalid literal/length code";
            return AF_ERR_FS_CORRUPT;
        }

        // 0-255: a literal byte.
        if (symbol < 256) {
            sink_byte(out, (af_u8)symbol);
            if (out->full) {
                *why = "decompressed data does not fit the output buffer";
                return AF_ERR_TOOMANY;
            }
            continue;
        }

        // 256: end of block.
        if (symbol == 256) {
            return AF_OK;
        }

        // 257-285: a back-reference. 286 and 287 are not valid in a stream and
        // must be rejected rather than indexing past the table.
        const int length_code = symbol - 257;
        if (length_code < 0 || length_code > 28) {
            *why = "DEFLATE stream uses a length code that is not valid";
            return AF_ERR_FS_CORRUPT;
        }

        const af_size length = (af_size)s_len_base[length_code] +
                               (af_size)br_bits(br, s_len_extra[length_code]);

        const int dist_symbol = huff_decode(br, dist);
        if (dist_symbol < 0 || dist_symbol > 29) {
            *why = "DEFLATE stream uses a distance code that is not valid";
            return AF_ERR_FS_CORRUPT;
        }

        const af_size distance = (af_size)s_dist_base[dist_symbol] +
                                 (af_size)br_bits(br, s_dist_extra[dist_symbol]);

        if (br->overrun) {
            *why = "DEFLATE stream ended in the middle of a symbol";
            return AF_ERR_FS_CORRUPT;
        }

        sink_copy(out, distance, length);
        if (out->full) {
            *why = "decompressed data does not fit the output buffer";
            return AF_ERR_TOOMANY;
        }
    }
}

// -----------------------------------------------------------------------------
// A stored block
// -----------------------------------------------------------------------------
static af_status_t inflate_stored(bitreader_t *br, sink_t *out, const char **why)
{
    br_align_unread(br);

    // Past the alignment, a stored block is plain bytes: LEN, NLEN, then the
    // data. No Huffman, no back-references. It is the case a decoder written
    // against Huffman-only data gets wrong, which is why the generated vectors
    // include an incompressible payload — no compressor emits anything else for
    // random data.
    if (!deflate_region_ok(br->data, br->len, br->byte, 4)) {
        *why = "stored block header is truncated";
        return AF_ERR_FS_CORRUPT;
    }

    const af_size blen = (af_size)br->data[br->byte] |
                         ((af_size)br->data[br->byte + 1] << 8);
    const af_size nlen = (af_size)br->data[br->byte + 2] |
                         ((af_size)br->data[br->byte + 3] << 8);
    br->byte += 4;

    if ((blen ^ nlen) != 0xFFFFu) {
        *why = "stored block LEN and NLEN are not ones-complements";
        return AF_ERR_FS_CORRUPT;
    }

    if (!deflate_region_ok(br->data, br->len, br->byte, blen)) {
        *why = "stored block runs past the end of the stream";
        return AF_ERR_FS_CORRUPT;
    }

    if (out->used + blen > out->size) {
        *why = "decompressed data does not fit the output buffer";
        return AF_ERR_TOOMANY;
    }

    // Bytes now, not bits — the block is not Huffman-coded, so it is copied
    // rather than decoded one symbol at a time.
    for (af_size i = 0; i < blen; i++) {
        sink_byte(out, br->data[br->byte + i]);
    }
    br->byte += blen;

    return AF_OK;
}

// -----------------------------------------------------------------------------
// A dynamic block's tables
// -----------------------------------------------------------------------------
static af_status_t read_dynamic_tables(bitreader_t *br,
                                       huff_t *lit, huff_t *dist,
                                       const char **why)
{
    const af_u32 hlit  = br_bits(br, 5) + 257;   // 257..286
    const af_u32 hdist = br_bits(br, 5) + 1;     // 1..32
    const af_u32 hclen = br_bits(br, 4) + 4;     // 4..19

    if (br->overrun || hlit > 286 || hdist > 30) {
        *why = "dynamic block declares more codes than DEFLATE allows";
        return AF_ERR_FS_CORRUPT;
    }

    // The code-length alphabet's own lengths, in the scrambled order.
    af_u8 clc_lengths[19];
    for (af_u32 i = 0; i < 19; i++) {
        clc_lengths[i] = 0;
    }
    for (af_u32 i = 0; i < hclen; i++) {
        clc_lengths[s_clc_order[i]] = (af_u8)br_bits(br, 3);
    }

    huff_t clc;
    if (huff_build(&clc, clc_lengths, 19) < 0) {
        *why = "dynamic block has an over-subscribed code-length table";
        return AF_ERR_FS_CORRUPT;
    }

    // The literal and distance lengths, themselves Huffman-coded, with 16, 17
    // and 18 as repeat codes. This is the third of the three places this file
    // says bugs live.
    af_u8 lengths[288 + 30];
    af_u32 total = hlit + hdist;

    for (af_u32 i = 0; i < total; ) {
        const int symbol = huff_decode(br, &clc);
        if (symbol < 0) {
            *why = "dynamic block has an invalid code-length code";
            return AF_ERR_FS_CORRUPT;
        }

        if (symbol < 16) {
            lengths[i++] = (af_u8)symbol;
            continue;
        }

        af_u32 repeat = 0;
        af_u8  value = 0;

        if (symbol == 16) {
            // Copy the PREVIOUS length 3-6 times. At the very start there is no
            // previous length, and a stream that does this is corrupt.
            if (i == 0) {
                *why = "dynamic block repeats a code length before any was given";
                return AF_ERR_FS_CORRUPT;
            }
            value = lengths[i - 1];
            repeat = 3 + br_bits(br, 2);
        } else if (symbol == 17) {
            repeat = 3 + br_bits(br, 3);
        } else {
            repeat = 11 + br_bits(br, 7);
        }

        if (i + repeat > total) {
            *why = "dynamic block's code-length repeat runs past the table";
            return AF_ERR_FS_CORRUPT;
        }

        while (repeat-- > 0) {
            lengths[i++] = value;
        }
    }

    if (br->overrun) {
        *why = "DEFLATE stream ended inside the dynamic block header";
        return AF_ERR_FS_CORRUPT;
    }

    // The literal table must be usable. The distance table may be empty for a
    // block that references no distance, which is why the two results differ:
    // huff_build returns >0 for incomplete, and an incomplete literal table is
    // an error while an empty distance table is not.
    if (huff_build(lit, lengths, hlit) < 0) {
        *why = "dynamic block has an over-subscribed literal/length table";
        return AF_ERR_FS_CORRUPT;
    }
    if (huff_build(dist, lengths + hlit, hdist) < 0) {
        *why = "dynamic block has an over-subscribed distance table";
        return AF_ERR_FS_CORRUPT;
    }

    return AF_OK;
}

// -----------------------------------------------------------------------------
// The entry point
// -----------------------------------------------------------------------------
af_status_t afpkg_inflate(const af_u8 *data, af_size len,
                          af_u8 *out_base, af_size out_size,
                          af_size *out_len, af_size *out_consumed,
                          const char **why)
{
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (out_consumed != NULL) {
        *out_consumed = 0;
    }

    if (data == NULL || out_base == NULL || out_len == NULL) {
        *why = "no input or output buffer";
        return AF_ERR_INVAL;
    }

    bitreader_t br;
    br_init(&br, data, len);

    sink_t out;
    out.base = out_base;
    out.size = out_size;
    out.used = 0;
    out.full = false;

    af_u32 blocks = 0;

    while (true) {
        const af_u32 final = br_bit(&br);
        const af_u32 type  = br_bits(&br, 2);

        if (br.overrun) {
            *why = "DEFLATE stream ended before its final block";
            return AF_ERR_FS_CORRUPT;
        }

        switch (type) {
        case 0: {
            const af_status_t rc = inflate_stored(&br, &out, why);
            if (af_status_err(rc)) {
                return rc;
            }
            break;
        }

        case 1: {
            huff_t lit, dist;
            build_fixed_literal(&lit);
            build_fixed_distance(&dist);

            const af_status_t rc = inflate_block(&br, &out, &lit, &dist, why);
            if (af_status_err(rc)) {
                return rc;
            }
            break;
        }

        case 2: {
            huff_t lit, dist;
            af_status_t rc = read_dynamic_tables(&br, &lit, &dist, why);
            if (af_status_err(rc)) {
                return rc;
            }
            rc = inflate_block(&br, &out, &lit, &dist, why);
            if (af_status_err(rc)) {
                return rc;
            }
            break;
        }

        default:
            // Type 3 is reserved. A stream containing one is corrupt, and
            // treating it as something else is how a decoder invents data.
            *why = "DEFLATE block type 3 is reserved and must not appear";
            return AF_ERR_FS_CORRUPT;
        }

        if (final) {
            break;
        }

        // A stream that never sets BFINAL would spin here forever assembling
        // blocks from whatever bytes follow. Bounded, because a decompressor
        // that can be made to loop is a decompressor that can be made to hang.
        if (++blocks > 0x100000u) {
            *why = "DEFLATE stream contains an implausible number of blocks";
            return AF_ERR_FS_CORRUPT;
        }

        if (br.byte >= br.len && br.count == 0) {
            *why = "DEFLATE stream ended without a final block";
            return AF_ERR_FS_CORRUPT;
        }
    }

    *out_len = out.used;

    if (out_consumed != NULL) {
        // How far into the input the stream reached, ROUNDED UP to a byte
        // boundary — which is what gzip pads to before writing the trailer.
        //
        // The bit reader loads bytes eagerly, so `br.byte` counts bytes it has
        // READ, not bits it has CONSUMED; the difference is what is still
        // sitting in the buffer. Subtracting those bits and rounding up gives
        // the position of the first byte after the stream, which is where the
        // caller's trailer begins.
        const af_size bits_used = br.byte * 8u - br.count;
        const af_size bytes_used = (bits_used + 7u) / 8u;
        *out_consumed = (bytes_used <= len) ? bytes_used : len;
    }

    *why = "inflated";
    return AF_OK;
}
