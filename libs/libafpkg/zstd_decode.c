// SPDX-License-Identifier: MIT
//
// =============================================================================
// STATUS: COMPLETE FOR WHAT IT CLAIMS. 48 of 48 vectors pass byte for byte, and
// it is in the test gate (tests/native/test_afpkg.c, run by tools/native_test.sh).
//
// It was committed one milestone ago as INCOMPLETE, at 21 of 42, with a note
// saying where to look next. The note named the compressed-block path and that
// was right, but the remaining bugs were not in one place: NINE separate defects
// were between "21 of 42" and "42 of 42", and six of them are the kind that
// produce output of the right LENGTH with the wrong bytes.
//
//   1. The match-length predefined distribution had its low-probability tail in
//      the wrong place — -1 at 48..52 instead of 46..52. It still summed to
//      exactly 64, so every consistency check the table builder has passed.
//   2. The symbol lookup and the state renormalisation were one operation. The
//      format separates them: extras are read OF, ML, LL with the states as they
//      are, and only then are the states updated LL, ML, OF — and NOT after the
//      last sequence.
//   3. The repeat-offset rules. When literals_length is 0 the whole mapping
//      shifts by one (1 means Repeat2, 2 means Repeat3, 3 means Repeat1 - 1) and
//      the history is STILL updated. Treating "3 with no literals" as a read that
//      leaves the history alone is the natural reading and it is wrong.
//   4. FSE count tables do not always use the full bit width. The low nb_bits-1
//      bits are read first and the top bit is fetched only when needed; always
//      reading nb_bits gets the right VALUE and the wrong bit position.
//   5. The FSE decoder does not stop where the bits stop. It emits one more
//      symbol from each state — two more, interleaved — because the reference
//      reader refills in whole bytes and reports overflow after the fact.
//   6. The raw/RLE literals header. Size_Format is a two-bit field whose values
//      00 and 10 mean the SAME thing, and the multi-byte size does not start at
//      bit 3. Reading it as one bit made a 39-byte literal section 78 bytes long.
//   7. Each FSE table description is byte-aligned. Feeding one bit reader into
//      all three leaves the second one a bit-shift out, and its counts still sum
//      to the right total, so it looks like a table and decodes to wrong symbols.
//   8. The frame content size adds 256 when the field is two bytes wide
//      (RFC 8878 §3.1.1.1.4) — an oddity, documented, and easy to skip.
//   9. The content checksum was read and ignored. XXH64 is now implemented and
//      the frame's checksum is VERIFIED, because without it a flipped bit inside
//      literal data decodes to a valid frame, a valid tar, and a package that
//      installs with one wrong byte in it.
//
// WHAT IS DELIBERATELY NOT HERE:
//   * Dictionaries. A frame that names one is refused with AF_ERR_NOTSUP rather
//     than decoded against nothing. Distribution packages do not use them.
//   * Multiple frames in one buffer. This decodes one frame; a caller with a
//     concatenated stream calls it again. The frame length is not returned yet,
//     which is the missing piece and is named here rather than discovered.
//   * Skirmish frames (magic 0x184D2A5?), which are skippable metadata.
//   * Speed. Both bit readers are a bit at a time. That is the right first
//     version and it is the reason the vectors could localise each bug.
// =============================================================================
//
// AfriyieOS — Zstandard decoding
//
// RFC 8878 in full: frames, blocks, FSE-coded sequence tables, Huffman-coded
// literals, and the repeat-offset scheme. This is what turns "reads a .deb dpkg
// built when asked nicely" into "reads what Debian and Ubuntu actually ship",
// unblocking three formats at once — modern .deb, pacman's .pkg.tar.zst, Snap.
//
// FIVE THINGS IN THIS FORMAT SURPRISE EVERYBODY ONCE
//
//   1. THE MAIN BITSTREAM IS READ BACKWARD. Sequences live at the END of a
//      block, written backwards. The last byte's HIGHEST SET BIT IS A SENTINEL,
//      not data — so the valid bit count is (len-1)*8 + highbit(last) and
//      reading starts one bit below it. Off by one produces plausible garbage.
//
//   2. BIT ORDER DIFFERS BY CONTEXT, WITH NO MARKER. The backward stream is
//      read most-significant-bit first; the table descriptions at the FRONT of
//      the block are read in a forward stream, least-significant-bit first.
//
//   3. FSE TABLES ARRIVE AS COUNTS, NOT CODES. The codes are canonical and
//      derived from how many entries each symbol occupies, so a table can be
//      over-subscribed — which the format requires a decoder to DETECT rather
//      than decode through. Decoding through produces bytes from an
//      uninitialised slot.
//
//   4. THE SEQUENCE ORDER IS NOT THE OBVIOUS ONE. The three FSE states are
//      initialised LL, OF, ML; the three codes are then decoded LL, ML, OF; and
//      the three sets of extra bits are read OF, ML, LL. Three different orders
//      inside one loop, none of them marked.
//
//   5. OFFSETS 1, 2 AND 3 ARE NOT OFFSETS. They reference the last three
//      distinct offsets, with one special case that only applies when the
//      literal length is zero.
//
// Every one of those has vectors aimed at it; see tools/pkgsynth.py.
//
// WHAT IS DELIBERATELY SIMPLE: both bit readers are naive — a bit at a time from
// an index, rather than the reference implementation's 64-bit register with
// reload logic. That version is several times faster and has several more ways
// to be subtly wrong. Slow and obviously correct is the right first version; the
// fast one is a rewrite with these vectors already in place to prove it.

#include "afpkg.h"

#ifdef ZSTD_TRACE
#include <stdio.h>
#define ZTRACE(...) do { printf("  [zstd] " __VA_ARGS__); printf("\n"); } while (0)
#else
#define ZTRACE(...) do { } while (0)
#endif

// Local, because afpkg.c keeps its helpers static to that file. Two one-line
// functions beat exporting a helper that has nothing to do with this file.
static void zd_memset(void *dst, int value, af_size n)
{
    af_u8 *d = (af_u8 *)dst;
    for (af_size i = 0; i < n; i++) {
        d[i] = (af_u8)value;
    }
}

#define ZSTD_MAGIC 0xFD2FB528u

#define ZSTD_MAX_OFFSET_CODE 31
#define ZSTD_MAX_LL_CODE 35
#define ZSTD_MAX_ML_CODE 52

#define ZSTD_FSE_MAX_TABLELOG 9
#define ZSTD_FSE_MIN_TABLELOG 5
#define ZSTD_FSE_MAX_SYMBOLS 256
#define ZSTD_FSE_MAX_TABLE (1u << ZSTD_FSE_MAX_TABLELOG)

#define ZSTD_HUF_MAX_TABLELOG 11
#define ZSTD_HUF_MAX_SYMBOLS 256

#define ZSTD_MAX_LITERALS (128 * 1024)

static af_u32 highbit32(af_u32 v)
{
    af_u32 n = 0;
    while (v >>= 1) {
        n++;
    }
    return n;
}

// =============================================================================
// A backward bit reader — most-significant bit first, from the end
// =============================================================================
typedef struct {
    const af_u8 *data;
    af_i64       bitpos;
    bool         overrun;
} zbits_t;

static af_status_t zb_init(zbits_t *z, const af_u8 *data, af_size len,
                           const char **why)
{
    z->data = data;
    z->overrun = false;
    z->bitpos = 0;

    if (len == 0) {
        *why = "empty Zstandard bitstream";
        return AF_ERR_FS_CORRUPT;
    }

    const af_u8 last = data[len - 1];
    if (last == 0) {
        *why = "Zstandard bitstream's final byte is zero — the end marker is "
               "missing";
        return AF_ERR_FS_CORRUPT;
    }

    z->bitpos = (af_i64)((len - 1) * 8) + (af_i64)highbit32(last);
    return AF_OK;
}

static af_u32 zb_read(zbits_t *z, af_u32 n)
{
    af_u32 value = 0;

    for (af_u32 i = 0; i < n; i++) {
        if (z->bitpos <= 0) {
            z->overrun = true;
            return 0;
        }
        z->bitpos--;
        const af_i64 bit = z->bitpos;
        value = (value << 1) | (af_u32)((z->data[bit >> 3] >> (bit & 7)) & 1);
    }

    return value;
}

static bool zb_empty(const zbits_t *z)
{
    return z->bitpos <= 0;
}

// =============================================================================
// A forward bit reader — least-significant bit first, from the start
// =============================================================================
typedef struct {
    const af_u8 *data;
    af_size      len;
    af_size      bitpos;
    bool         overrun;
} fbits_t;

static void fb_init(fbits_t *b, const af_u8 *data, af_size len)
{
    b->data = data;
    b->len = len;
    b->bitpos = 0;
    b->overrun = false;
}

static af_u32 fb_read(fbits_t *b, af_u32 n)
{
    af_u32 value = 0;

    for (af_u32 i = 0; i < n; i++) {
        const af_size byte = b->bitpos >> 3;
        if (byte >= b->len) {
            b->overrun = true;
            return value;
        }
        value |= (af_u32)((b->data[byte] >> (b->bitpos & 7)) & 1) << i;
        b->bitpos++;
    }

    return value;
}

static af_size fb_bytes_used(const fbits_t *b)
{
    return (b->bitpos + 7) / 8;
}

// EACH TABLE DESCRIPTION ENDS ON A BYTE BOUNDARY, AND THE NEXT ONE STARTS THERE.
//
// The forward stream is a bit stream and the descriptions look consecutive, so
// the natural implementation feeds one reader into all three. The reference
// implementation does not: FSE_readNCount reports how many BYTES it consumed and
// the caller advances a byte pointer. The two agree only when every description
// happens to end on a byte — and when two tables are present and the first ends
// mid-byte, the second is read one bit-shift out. Its normalized counts still sum
// to exactly the right total, so it looks like a valid table and decodes to
// symbols that are merely wrong.
static void fb_align(fbits_t *b)
{
    b->bitpos = (b->bitpos + 7u) & ~(af_size)7u;
}


// =============================================================================
// FSE
// =============================================================================
typedef struct {
    af_u8  nb_bits;
    af_u16 new_state;
    af_u8  symbol;
} fse_entry_t;

typedef struct {
    af_u32      table_log;
    fse_entry_t table[ZSTD_FSE_MAX_TABLE];
} fse_dt_t;

typedef struct {
    af_u32          state;
    const fse_dt_t *dt;
} fse_state_t;

// A negative count means "low probability": the symbol gets one state but no
// code is transmitted for it, so it lives at the TOP of the table where the
// unused codes are. The one piece of FSE that looks arbitrary and is not.
static int fse_build_dtable(const af_i16 *norm, af_u32 max_symbol,
                            af_u32 table_log, fse_dt_t *dt)
{
    if (table_log > ZSTD_FSE_MAX_TABLELOG || max_symbol >= ZSTD_FSE_MAX_SYMBOLS) {
        return -1;
    }

    const af_u32 table_size = 1u << table_log;
    af_u16 symbol_next[ZSTD_FSE_MAX_SYMBOLS];

    for (af_u32 i = 0; i < ZSTD_FSE_MAX_SYMBOLS; i++) {
        symbol_next[i] = 0;
    }

    dt->table_log = table_log;

    af_u32 high_threshold = table_size - 1;

    for (af_u32 s = 0; s <= max_symbol; s++) {
        if (norm[s] == -1) {
            dt->table[high_threshold].symbol = (af_u8)s;
            high_threshold--;
            symbol_next[s] = 1;
        } else {
            symbol_next[s] = (af_u16)norm[s];
        }
    }

    // A step coprime with the table size walks every slot, decorrelating
    // adjacent states. The constant is the reference implementation's; another
    // coprime step still walks every slot and decodes nothing.
    const af_u32 step = (table_size >> 1) + (table_size >> 3) + 3;
    const af_u32 mask = table_size - 1;

    af_u32 position = 0;
    for (af_u32 s = 0; s <= max_symbol; s++) {
        for (af_i32 i = 0; i < (af_i32)norm[s]; i++) {
            dt->table[position].symbol = (af_u8)s;
            position = (position + step) & mask;
            while (position > high_threshold) {
                position = (position + step) & mask;
            }
        }
    }

    if (position != 0) {
        return -1;
    }

    for (af_u32 u = 0; u < table_size; u++) {
        const af_u32 symbol = dt->table[u].symbol;
        const af_u32 next_state = symbol_next[symbol]++;

        const af_u32 nb = table_log - highbit32(next_state);
        dt->table[u].nb_bits = (af_u8)nb;
        dt->table[u].new_state = (af_u16)((next_state << nb) - table_size);
    }

    return 0;
}

static af_status_t fse_read_ncount(fbits_t *b, af_i16 *norm,
                                   af_u32 max_symbol, af_u32 *table_log_out,
                                   af_u32 *max_symbol_out, const char **why)
{
    for (af_u32 i = 0; i < ZSTD_FSE_MAX_SYMBOLS; i++) {
        norm[i] = 0;
    }

    const af_u32 table_log = fb_read(b, 4) + ZSTD_FSE_MIN_TABLELOG;
    *table_log_out = table_log;

    if (b->overrun || table_log > ZSTD_FSE_MAX_TABLELOG) {
        *why = "FSE table declares an accuracy log larger than the format allows";
        return AF_ERR_FS_CORRUPT;
    }

    af_i32 remaining = (af_i32)((1u << table_log) + 1);
    af_u32 threshold = 1u << table_log;
    af_u32 nb_bits = table_log + 1;

    af_u32 charnum = 0;
    bool previous0 = false;

    while (remaining > 1 && charnum <= max_symbol) {
        if (previous0) {
            af_u32 n0 = charnum;
            for (;;) {
                const af_u32 v = fb_read(b, 2);
                if (b->overrun) {
                    *why = "FSE count table ended inside a zero run";
                    return AF_ERR_FS_CORRUPT;
                }
                if (v == 3) {
                    n0 += 3;
                } else {
                    n0 += v;
                    break;
                }
            }

            if (n0 > max_symbol) {
                *why = "FSE zero run names a symbol past the end of the alphabet";
                return AF_ERR_FS_CORRUPT;
            }
            while (charnum < n0) {
                norm[charnum++] = 0;
            }
            if (charnum > max_symbol) {
                break;
            }
            previous0 = false;
        }

        const af_i32 max = (af_i32)((2 * threshold - 1) - (af_u32)remaining);

        // THE COUNT IS NOT ALWAYS nb_bits WIDE, AND THAT IS NOT AN OPTIMISATION
        // THE READER MAY IGNORE.
        //
        // The format encodes a count in nb_bits bits only when it needs them:
        // the low nb_bits-1 bits are read first, and the top bit is fetched ONLY
        // if those already guarantee the value is not below `max`. A decoder that
        // always reads nb_bits gets the same VALUE — the low bits are the same
        // either way — but advances the bit position by one too many, and every
        // symbol after it is decoded from the wrong offset.
        //
        // That is why the table still looked plausible: tableLog was right, the
        // symbol counts summed to the right total, and only the alphabet size and
        // the last few counts were wrong. It took a Huffman weight table with the
        // wrong number of symbols to make it visible.
        const af_u32 low = fb_read(b, nb_bits - 1);
        af_i32 count;

        if ((af_i32)low < max) {
            count = (af_i32)low;
        } else {
            const af_u32 ext = fb_read(b, 1);
            count = (af_i32)(low | (ext << (nb_bits - 1)));
            if (count >= (af_i32)threshold) {
                count -= max;
            }
        }

        if (b->overrun) {
            *why = "FSE count table ended inside a count";
            return AF_ERR_FS_CORRUPT;
        }

        count--;   // the transmitted value is offset by one

        remaining -= (count < 0) ? -count : count;
        norm[charnum++] = (af_i16)count;
        previous0 = (count == 0);

        while (remaining < (af_i32)threshold) {
            nb_bits--;
            threshold >>= 1;
        }
    }

    if (remaining != 1) {
        *why = "FSE count table does not sum to its declared table size";
        return AF_ERR_FS_CORRUPT;
    }

    *max_symbol_out = (charnum > 0) ? (charnum - 1) : 0;
    return AF_OK;
}

static void fse_init_state(fse_state_t *st, zbits_t *z, const fse_dt_t *dt)
{
    st->dt = dt;
    st->state = zb_read(z, dt->table_log);
}

static af_u32 fse_decode(fse_state_t *st, zbits_t *z)
{
    const fse_entry_t *entry = &st->dt->table[st->state];
    const af_u32 symbol = entry->symbol;
    const af_u32 low = zb_read(z, entry->nb_bits);
    st->state = (af_u32)entry->new_state + low;
    return symbol;
}

// A SEQUENCE DECODER CANNOT USE fse_decode, AND THAT IS THE POINT OF THESE TWO.
//
// fse_decode reads the symbol AND immediately consumes the bits that renormalise
// the state. For the Huffman weight tables that is exactly right. For sequences
// it is wrong, because the format interleaves the three symbol types in THREE
// DIFFERENT ORDERS and the renormalisation belongs to a different step from the
// symbol:
//
//     initial states   LL, OF, ML
//     extra bits       OF, ML, LL        <- read with the states AS THEY ARE
//     state updates    LL, ML, OF        <- and NOT for the last sequence
//
// Folding the update into the symbol lookup reads the LL renormalisation bits
// before the offset's extra bits, so every sequence after the first is decoded
// from the wrong position and the stream is exhausted early. That is a decoder
// that produces plausible rubbish and reports "the bitstream ended in the middle
// of a sequence" — which is exactly what this one did.
static af_u32 fse_peek(const fse_state_t *st)
{
    return st->dt->table[st->state].symbol;
}

static void fse_advance(fse_state_t *st, zbits_t *z)
{
    const fse_entry_t *entry = &st->dt->table[st->state];
    const af_u32 low = zb_read(z, entry->nb_bits);
    st->state = (af_u32)entry->new_state + low;
}

// =============================================================================
// Huffman, for literals
//
// Canonical codes transmitted as WEIGHTS: a weight of w means a code length of
// (maxBits + 1 - w). The symbol count is one more than the weight count, because
// the final weight is implied by the requirement that the code space be filled
// exactly.
// =============================================================================
// A DIRECT LOOKUP TABLE, because that is how this format's Huffman codes are
// defined: a symbol is identified by its table index in tableLog bits, not by a
// walk through code lengths.
typedef struct {
    af_u32 table_log;
    af_u8  nb_bits[ZSTD_HUF_MAX_SYMBOLS];
    af_u16 lookup[1u << ZSTD_HUF_MAX_TABLELOG];
} huf_dt_t;

// Static rather than a local: the lookup is 4 KiB and read_literals is reached
// from a block loop, so a stack copy per block is a real cost.
//
// NOT REENTRANT. One decode at a time, which is true today and is worth stating
// because the day it stops being true the symptom will be two decompressions
// corrupting each other's tables rather than anything obviously wrong.
static huf_dt_t s_huf;

static af_status_t huf_build(huf_dt_t *h, const af_u8 *weights,
                             af_u32 n_weights, const char **why)
{
    af_u8 lengths[ZSTD_HUF_MAX_SYMBOLS];
    const af_u32 nsym = n_weights + 1;

    if (nsym > ZSTD_HUF_MAX_SYMBOLS) {
        *why = "Huffman table declares too many symbols";
        return AF_ERR_FS_CORRUPT;
    }

    af_u32 weight_total = 0;
    for (af_u32 i = 0; i < n_weights; i++) {
        if (weights[i] > 0 && weights[i] <= 12) {
            weight_total += 1u << (weights[i] - 1);
        }
    }

    if (weight_total == 0) {
        *why = "Huffman table has no symbols";
        return AF_ERR_FS_CORRUPT;
    }

    const af_u32 table_log = highbit32(weight_total) + 1;
    if (table_log > ZSTD_HUF_MAX_TABLELOG) {
        *why = "Huffman table's codes are longer than the format allows";
        return AF_ERR_FS_CORRUPT;
    }

    // The implied last weight must make the total an exact power of two. If it
    // does not, the weights describe a code space that cannot be filled and
    // every decode after it would be off by an unknown amount.
    const af_u32 total = 1u << table_log;
    const af_u32 rest = total - weight_total;
    ZTRACE("huf: n_weights=%u wtotal=%u tablelog=%u rest=%u", n_weights,
           (unsigned)weight_total, (unsigned)table_log, (unsigned)rest);
    if (rest == 0 || (rest & (rest - 1)) != 0) {
        *why = "Huffman weights do not fill the code space exactly";
        return AF_ERR_FS_CORRUPT;
    }

    h->table_log = table_log;

    for (af_u32 i = 0; i < n_weights; i++) {
        lengths[i] = weights[i];
    }
    lengths[n_weights] = (af_u8)(highbit32(rest) + 1);

    // --- how many symbols carry each weight ----------------------------------
    af_u32 rank_stats[16];
    for (af_u32 w = 0; w <= 15; w++) {
        rank_stats[w] = 0;
    }
    for (af_u32 i = 0; i < nsym; i++) {
        if (lengths[i] > table_log) {
            *why = "Huffman weight exceeds the table's maximum code length";
            return AF_ERR_FS_CORRUPT;
        }
        if (lengths[i] > 0) {
            rank_stats[lengths[i]]++;
        }
    }

    // --- where each weight's block of the table begins ------------------------
    //
    // Accumulated in ENTRIES, not symbols: weight w occupies 2^(w-1) entries per
    // symbol. And accumulated for w ASCENDING, which is what puts the longest
    // codes at the bottom of the table — the opposite of canonical Huffman, and
    // the detail this whole function exists to get right.
    af_u32 rank_start[17];
    {
        af_u32 next = 0;
        for (af_u32 w = 1; w <= table_log; w++) {
            rank_start[w] = next;
            next += rank_stats[w] << (w - 1);
        }
        if (next != total) {
            *why = "Huffman weights cover the wrong number of table entries";
            return AF_ERR_FS_CORRUPT;
        }
    }

    // --- fill ----------------------------------------------------------------
    for (af_u32 i = 0; i < nsym; i++) {
        const af_u32 w = lengths[i];
        h->nb_bits[i] = (w == 0) ? 0 : (af_u8)(table_log + 1 - w);
    }

    for (af_u32 w = 1; w <= table_log; w++) {
        const af_u32 span = 1u << (w - 1);
        af_u32 at = rank_start[w];

        for (af_u32 i = 0; i < nsym; i++) {
            if (lengths[i] != w) {
                continue;
            }
            for (af_u32 k = 0; k < span; k++) {
                h->lookup[at + k] = (af_u16)i;
            }
            at += span;
        }
    }

    return AF_OK;
}

static af_status_t huf_read_weights(const af_u8 *data, af_size len,
                                    af_u8 *weights, af_u32 *n_out,
                                    af_size *consumed, const char **why)
{
    if (len == 0) {
        *why = "Huffman tree description is missing";
        return AF_ERR_FS_CORRUPT;
    }

    const af_u8 header = data[0];

    if (header < 128) {
        const af_size compressed = header;
        if (compressed == 0 || compressed + 1 > len) {
            *why = "Huffman weight description's size is out of range";
            return AF_ERR_FS_CORRUPT;
        }

        fbits_t fb;
        fb_init(&fb, data + 1, compressed);

        af_i16 norm[ZSTD_FSE_MAX_SYMBOLS];
        af_u32 table_log = 0;
        af_u32 max_symbol = 255;

        const af_status_t rc = fse_read_ncount(&fb, norm, 255, &table_log,
                                               &max_symbol, why);
        if (af_status_err(rc)) {
            return rc;
        }

        if (table_log > 6) {
            *why = "Huffman weight table has an accuracy log above the "
                   "format's limit of 6";
            return AF_ERR_FS_CORRUPT;
        }

        fse_dt_t dt;
        if (fse_build_dtable(norm, max_symbol, table_log, &dt) != 0) {
            *why = "Huffman weight table is malformed";
            return AF_ERR_FS_CORRUPT;
        }

        const af_size header_bytes = fb_bytes_used(&fb);
        if (header_bytes >= compressed) {
            *why = "Huffman weight table leaves no room for its bitstream";
            return AF_ERR_FS_CORRUPT;
        }

        zbits_t z;
        const af_status_t zrc = zb_init(&z, data + 1 + header_bytes,
                                        compressed - header_bytes, why);
        if (af_status_err(zrc)) {
            return zrc;
        }

        // Two interleaved states, so the weight count is whichever runs out of
        // bits first rather than anything transmitted.
        fse_state_t s1, s2;
        fse_init_state(&s1, &z, &dt);
        fse_init_state(&s2, &z, &dt);

        // ZSTD'S FSE DECODER DOES NOT STOP WHERE THE BITS STOP. It stops one
        // symbol LATER.
        //
        // Its bit reader refills a register a byte at a time and reports
        // "overflow" when a refill goes past the start of the stream; at that
        // point both live states still hold valid symbols and both are emitted.
        // An exact bit-at-a-time reader that stops the moment the last bit is
        // consumed therefore loses the final two weights — and loses them
        // invisibly, because the missing weights are the high symbols that happen
        // to be zero, so the table still looks like a table and only the symbol
        // COUNT is wrong. Everything downstream then fails at a place that has
        // nothing to do with the cause.
        af_u32 n = 0;
        for (;;) {
            if (n + 2 > ZSTD_HUF_MAX_SYMBOLS) {
                *why = "Huffman weight description is longer than the format allows";
                return AF_ERR_FS_CORRUPT;
            }

            weights[n++] = (af_u8)fse_decode(&s1, &z);
            if (z.overrun) {
                weights[n++] = (af_u8)fse_decode(&s2, &z);
                break;
            }

            weights[n++] = (af_u8)fse_decode(&s2, &z);
            if (z.overrun) {
                weights[n++] = (af_u8)fse_decode(&s1, &z);
                break;
            }
        }

        if (n < 2) {
            *why = "Huffman weight description decoded too few weights";
            return AF_ERR_FS_CORRUPT;
        }

#ifdef ZSTD_TRACE
        ZTRACE("weights: FSE n=%u tablelog=%u bits left %lld", n,
               (unsigned)table_log, (long long)z.bitpos);
        {
            char line[256];
            int at = 0;
            for (af_u32 q = 0; q < n && at < 200; q++) {
                at += snprintf(line + at, sizeof(line) - (size_t)at, "%u ",
                               (unsigned)weights[q]);
            }
            line[at] = '\0';
            ZTRACE("weights: %s", line);
        }
#endif

        *n_out = n;
        *consumed = 1 + compressed;
        return AF_OK;
    }

    // Weights written directly, four bits each, high nibble first.
    const af_u32 n = (af_u32)header - 127;
    const af_size need = (n + 1) / 2;

    if (n == 0 || need + 1 > len) {
        *why = "direct Huffman weights run past the end of the block";
        return AF_ERR_FS_CORRUPT;
    }

    for (af_u32 i = 0; i < n; i++) {
        const af_u8 byte = data[1 + (i / 2)];
        weights[i] = (af_u8)((i % 2 == 0) ? (byte >> 4) : (byte & 0x0F));
    }

    *n_out = n;
    *consumed = 1 + need;
    return AF_OK;
}

// A peek that pads with zeroes past the end of the stream.
//
// The final code of a stream is often shorter than tableLog bits and there is
// nothing after it, so reading tableLog bits strictly would fail on a stream
// that is perfectly well formed. zstd's own fast path loads a register and lets
// the high bits be whatever is there; padding with zeroes is the same idea made
// deterministic.
static af_u32 zb_peek_padded(const zbits_t *z, af_u32 n)
{
    af_u32 value = 0;
    af_i64 pos = z->bitpos;

    for (af_u32 i = 0; i < n; i++) {
        pos--;
        af_u32 b = 0;
        if (pos >= 0) {
            b = (af_u32)((z->data[pos >> 3] >> (pos & 7)) & 1);
        }
        value = (value << 1) | b;
    }

    return value;
}

// The symbol, or -1. A code matching nothing must be an error rather than a
// symbol, or a corrupt block decodes into plausible bytes.
static int huf_decode(zbits_t *z, const huf_dt_t *h)
{
    const af_u32 index = zb_peek_padded(z, h->table_log);
    const af_u32 symbol = h->lookup[index];
    const af_u32 bits = h->nb_bits[symbol];

    if (bits == 0 || bits > h->table_log) {
        return -1;
    }

    zb_read(z, bits);
    if (z->overrun) {
        return -1;
    }

    return (int)symbol;
}

// =============================================================================
// Sequence code tables
// =============================================================================
static const af_u32 s_ll_base[ZSTD_MAX_LL_CODE + 1] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096,
    8192, 16384, 32768, 65536
};
static const af_u8 s_ll_bits[ZSTD_MAX_LL_CODE + 1] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
};

static const af_u32 s_ml_base[ZSTD_MAX_ML_CODE + 1] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
    19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
    35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131,
    259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539
};
static const af_u8 s_ml_bits[ZSTD_MAX_ML_CODE + 1] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7,
    8, 9, 10, 11, 12, 13, 14, 15, 16
};

// An offset code of n means a base of 1<<n and n extra bits, so the smallest
// reachable value is 1 — and values 1, 2 and 3 are not offsets at all but
// references to the repeat offsets.
static af_u32 of_base(af_u32 code)
{
    return (code == 0) ? 1u : (1u << code);
}

// =============================================================================
// The predefined FSE tables (RFC 8878 §3.1.1.3.2.2)
//
// Densities, not codes. Literal lengths cluster on small values with a long
// tail; match lengths are near-uniform with a tail; offsets are deliberately
// flat, because they carry the most information.
// =============================================================================
static const af_i16 s_ll_default[36] = {
    4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1,
    -1, -1, -1, -1
};

// RFC 8878 §3.1.1.3.2.2.2. TRANSCRIBED, NOT DERIVED — and the tail is the part
// that matters and the part that was wrong.
//
// Symbols 46..52 are the seven low-probability ones. This array previously ended
// its run of 1s at index 47 and put only five -1s at 48..52, which still sums to
// exactly 64 and so passed every internal consistency check the builder has. It
// is a distribution that is self-consistent and wrong: states 57 and 56 decoded
// to match-length codes 45 and 46 instead of 52 and 51, so a match length of
// 39998 came back as 41. Nothing detects that except an answer compared against
// something outside this file.
static const af_i16 s_ml_default[53] = {
    1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1,
    -1, -1, -1, -1, -1
};

static const af_i16 s_of_default[29] = {
    1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1
};

// =============================================================================
// Output
// =============================================================================
typedef struct {
    af_u8  *base;
    af_size size;
    af_size used;
    bool    full;
} outbuf_t;

static void out_byte(outbuf_t *o, af_u8 v)
{
    if (o->used >= o->size) {
        o->full = true;
        return;
    }
    o->base[o->used++] = v;
}

// A back-reference. BYTE AT A TIME, deliberately: the source may overlap the
// destination, which is how run-length compression works here, and a block copy
// reads bytes this loop has not written yet — corrupting exactly the inputs that
// compress best.
static void out_copy(outbuf_t *o, af_size distance, af_size length)
{
    for (af_size i = 0; i < length; i++) {
        if (o->used < distance) {
            o->full = true;
            return;
        }
        out_byte(o, o->base[o->used - distance]);
    }
}

// =============================================================================
// The literals section
// =============================================================================
static af_status_t read_literals(const af_u8 *data, af_size len,
                                 af_u8 *out, af_size out_size,
                                 af_size *lit_len, af_size *consumed,
                                 const char **why)
{
    if (len < 1) {
        *why = "block has no literals section";
        return AF_ERR_FS_CORRUPT;
    }

    const af_u8 b0 = data[0];
    const af_u32 type = b0 & 3u;
    const af_u32 size_format = (b0 >> 2) & 3u;

    af_size header = 0;
    af_u32 regenerated = 0;
    af_u32 compressed = 0;
    bool four_streams = false;

    if (type == 0 || type == 1) {
        // Raw and RLE use the same 2-bit Size_Format field as everything else,
        // but only three of its four values exist and the two single-byte ones
        // are NOT adjacent:
        //
        //     00 or 10   1 byte,  Regenerated_Size = header[0] >> 3   (5 bits)
        //     01         2 bytes, = (header[0] >> 4) + (header[1] << 4)
        //     11         3 bytes, = (header[0] >> 4) + (header[1] << 4)
        //                                    + (header[2] << 12)
        //
        // Reading the field as one bit and then taking the size out of bits 3+
        // for the multi-byte forms gives a number that is plausible, large, and
        // wrong: this decoder read a 39-byte literal section as 78 bytes, so the
        // sequences section started 39 bytes late and the first thing it saw was
        // an offset table where a count belonged.
        const af_u32 fmt = (b0 >> 2) & 3u;

        if (fmt == 0 || fmt == 2) {
            regenerated = (af_u32)b0 >> 3;
            header = 1;
        } else if (fmt == 1) {
            if (len < 2) {
                *why = "literals header is truncated";
                return AF_ERR_FS_CORRUPT;
            }
            regenerated = ((af_u32)data[0] >> 4) | ((af_u32)data[1] << 4);
            header = 2;
        } else {
            if (len < 3) {
                *why = "literals header is truncated";
                return AF_ERR_FS_CORRUPT;
            }
            regenerated = ((af_u32)data[0] >> 4) | ((af_u32)data[1] << 4) |
                          ((af_u32)data[2] << 12);
            header = 3;
        }
        compressed = (type == 0) ? regenerated : 1;
    } else {
        if (size_format == 0 || size_format == 1) {
            if (len < 3) {
                *why = "literals header is truncated";
                return AF_ERR_FS_CORRUPT;
            }
            regenerated = ((af_u32)(b0 >> 4) & 0x0Fu) |
                          (((af_u32)data[1] & 0x3Fu) << 4);
            compressed = ((af_u32)data[1] >> 6) | ((af_u32)data[2] << 2);
            header = 3;
            four_streams = (size_format == 1);
        } else if (size_format == 2) {
            if (len < 4) {
                *why = "literals header is truncated";
                return AF_ERR_FS_CORRUPT;
            }
            regenerated = ((af_u32)(b0 >> 4) & 0x0Fu) |
                          (((af_u32)data[1] & 0x3Fu) << 4) |
                          (((af_u32)data[2] & 0x03u) << 10);
            compressed = ((af_u32)data[2] >> 2) | ((af_u32)data[3] << 6);
            header = 4;
            four_streams = true;
        } else {
            if (len < 5) {
                *why = "literals header is truncated";
                return AF_ERR_FS_CORRUPT;
            }
            regenerated = ((af_u32)(b0 >> 4) & 0x0Fu) |
                          (((af_u32)data[1] & 0x3Fu) << 4) |
                          (((af_u32)data[2] & 0x3Fu) << 10);
            compressed = ((af_u32)data[2] >> 6) |
                         ((af_u32)data[3] << 2) |
                         ((af_u32)data[4] << 10);
            header = 5;
            four_streams = true;
        }
    }

    ZTRACE("literals: type=%u fmt=%u regen=%u comp=%u header=%lu",
           type, size_format, regenerated, compressed, (unsigned long)header);

    if (regenerated > out_size) {
        *why = "literals section does not fit the literal buffer";
        return AF_ERR_TOOMANY;
    }

    if (type == 0) {
        if (header + regenerated > len) {
            *why = "raw literals run past the end of the block";
            return AF_ERR_FS_CORRUPT;
        }
        for (af_u32 i = 0; i < regenerated; i++) {
            out[i] = data[header + i];
        }
        *lit_len = regenerated;
        *consumed = header + regenerated;
        return AF_OK;
    }

    if (type == 1) {
        if (header + 1 > len) {
            *why = "RLE literals byte is missing";
            return AF_ERR_FS_CORRUPT;
        }
        for (af_u32 i = 0; i < regenerated; i++) {
            out[i] = data[header];
        }
        *lit_len = regenerated;
        *consumed = header + 1;
        return AF_OK;
    }

    if (header + compressed > len) {
        *why = "compressed literals run past the end of the block";
        return AF_ERR_FS_CORRUPT;
    }

    const af_u8 *stream = data + header;
    af_size stream_len = compressed;

    huf_dt_t *huf = &s_huf;

    if (type == 2) {
        af_u8 weights[ZSTD_HUF_MAX_SYMBOLS];
        af_u32 n_weights = 0;
        af_size tree_consumed = 0;

        const af_status_t rc = huf_read_weights(stream, stream_len, weights,
                                                &n_weights, &tree_consumed, why);
        if (af_status_err(rc)) {
            return rc;
        }
        const af_status_t brc = huf_build(huf, weights, n_weights, why);
        if (af_status_err(brc)) {
            return brc;
        }

        stream += tree_consumed;
        stream_len -= tree_consumed;
    } else {
        *why = "treeless literals need the previous block's Huffman table";
        return AF_ERR_NOTSUP;
    }

    if (!four_streams) {
        zbits_t z;
        const af_status_t zrc = zb_init(&z, stream, stream_len, why);
        if (af_status_err(zrc)) {
            return zrc;
        }

        for (af_u32 i = 0; i < regenerated; i++) {
            const int sym = huf_decode(&z, huf);
            if (sym < 0) {
                *why = "Huffman literal decoding failed";
                return AF_ERR_FS_CORRUPT;
            }
            out[i] = (af_u8)sym;
        }

        *lit_len = regenerated;
        *consumed = header + compressed;
        return AF_OK;
    }

    // Four streams, with a jump table of the first three sizes as 16-bit
    // little-endian values; the fourth is whatever remains.
    if (stream_len < 6) {
        *why = "literals jump table is missing";
        return AF_ERR_FS_CORRUPT;
    }

    const af_u32 s1 = (af_u32)stream[0] | ((af_u32)stream[1] << 8);
    const af_u32 s2 = (af_u32)stream[2] | ((af_u32)stream[3] << 8);
    const af_u32 s3 = (af_u32)stream[4] | ((af_u32)stream[5] << 8);
    const af_size body = stream_len - 6;

    if ((af_size)s1 + s2 + s3 > body) {
        *why = "literals jump table sizes exceed the section";
        return AF_ERR_FS_CORRUPT;
    }
    const af_size s4 = body - s1 - s2 - s3;

    // Each of the four streams decodes a QUARTER, with the remainder in the
    // last. Getting the split wrong produces the right COUNT of bytes in the
    // wrong ORDER — exactly what byte-exact vectors catch.
    const af_u32 chunk = (regenerated + 3) / 4;
    const af_u32 counts[4] = {
        (chunk <= regenerated) ? chunk : regenerated,
        (2 * chunk <= regenerated) ? chunk
                                   : ((regenerated > chunk) ? regenerated - chunk : 0),
        (3 * chunk <= regenerated) ? chunk
                                   : ((regenerated > 2 * chunk) ? regenerated - 2 * chunk : 0),
        (regenerated > 3 * chunk) ? regenerated - 3 * chunk : 0
    };

    const af_u8 *starts[4] = { stream + 6, stream + 6 + s1,
                               stream + 6 + s1 + s2,
                               stream + 6 + s1 + s2 + s3 };
    const af_size sizes[4] = { s1, s2, s3, s4 };

    af_u32 written = 0;
    for (int k = 0; k < 4; k++) {
        if (counts[k] == 0 || sizes[k] == 0) {
            continue;
        }

        zbits_t z;
        const af_status_t zrc = zb_init(&z, starts[k], sizes[k], why);
        if (af_status_err(zrc)) {
            return zrc;
        }

        for (af_u32 i = 0; i < counts[k]; i++) {
            const int sym = huf_decode(&z, huf);
            if (sym < 0) {
                *why = "Huffman literal decoding failed in a 4-stream block";
                return AF_ERR_FS_CORRUPT;
            }
            out[written++] = (af_u8)sym;
        }
    }

    if (written != regenerated) {
        *why = "the four literal streams did not produce the declared count";
        return AF_ERR_FS_CORRUPT;
    }

    *lit_len = regenerated;
    *consumed = header + compressed;
    return AF_OK;
}

// =============================================================================
// Sequence tables
// =============================================================================
typedef struct {
    bool     is_rle;
    af_u8    rle_symbol;
    fse_dt_t dt;
} seq_table_t;

static af_status_t seq_table_read(seq_table_t *tab, fbits_t *fb,
                                  af_u32 mode,
                                  const af_i16 *predefined, af_u32 predefined_len,
                                  af_u32 predefined_log, af_u32 max_symbol,
                                  const seq_table_t *previous, const char **why)
{
    tab->is_rle = false;
    tab->rle_symbol = 0;

    switch (mode) {
    case 0:
        if (fse_build_dtable(predefined, predefined_len - 1, predefined_log,
                             &tab->dt) != 0) {
            *why = "the predefined sequence table failed to build";
            return AF_ERR_FS_CORRUPT;
        }
        return AF_OK;

    case 1:
        tab->is_rle = true;
        tab->rle_symbol = (af_u8)fb_read(fb, 8);
        ZTRACE("       table rle symbol=%u (max %u) at bit %lu",
               (unsigned)tab->rle_symbol, (unsigned)max_symbol,
               (unsigned long)fb->bitpos);
        if (fb->overrun || tab->rle_symbol > max_symbol) {
            *why = "RLE sequence table names a symbol outside its alphabet";
            return AF_ERR_FS_CORRUPT;
        }
        fb_align(fb);
        return AF_OK;

    case 2: {
        af_i16 norm[ZSTD_FSE_MAX_SYMBOLS];
        af_u32 table_log = 0;
        af_u32 table_max = max_symbol;

        const af_status_t rc = fse_read_ncount(fb, norm, max_symbol, &table_log,
                                               &table_max, why);
        if (af_status_err(rc)) {
            return rc;
        }
        if (table_max > max_symbol) {
            *why = "FSE sequence table declares too many symbols";
            return AF_ERR_FS_CORRUPT;
        }
        if (fse_build_dtable(norm, table_max, table_log, &tab->dt) != 0) {
            *why = "FSE sequence table is malformed";
            return AF_ERR_FS_CORRUPT;
        }
#ifdef ZSTD_TRACE
        {
            char line[400];
            int at2 = 0;
            line[0] = '\0';
            for (af_u32 q = 0; q <= table_max && at2 < 340; q++) {
                if (norm[q] != 0) {
                    at2 += snprintf(line + at2, sizeof(line) - (size_t)at2,
                                    "%u:%d ", (unsigned)q, (int)norm[q]);
                }
            }
            ZTRACE("       table max=%u log=%u bits=%lu  %s", (unsigned)table_max,
                   (unsigned)table_log, (unsigned long)fb->bitpos, line);
        }
#endif
        fb_align(fb);
        return AF_OK;
    }

    default:
        if (previous == NULL) {
            *why = "a sequence table repeats a previous one, but there is none";
            return AF_ERR_FS_CORRUPT;
        }
        *tab = *previous;
        return AF_OK;
    }
}

// An RLE table produces the same symbol every time and uses no bits and no
// state. Treating it as an FSE table would read bits that are not there and
// desynchronise everything after it.
static af_u32 seq_peek(const seq_table_t *tab, const fse_state_t *st, bool have)
{
    return tab->is_rle ? tab->rle_symbol : (have ? fse_peek(st) : 0u);
}

static void seq_advance(const seq_table_t *tab, fse_state_t *st, zbits_t *z,
                        bool have)
{
    if (!tab->is_rle && have) {
        fse_advance(st, z);
    }
}

// =============================================================================
// One compressed block
// =============================================================================
static af_status_t decode_compressed_block(const af_u8 *data, af_size len,
                                           af_u8 *literal_buf,
                                           af_size literal_cap, outbuf_t *out,
                                           seq_table_t *ll_prev,
                                           seq_table_t *of_prev,
                                           seq_table_t *ml_prev,
                                           bool *have_previous,
                                           const char **why)
{
    af_size lit_len = 0;
    af_size consumed = 0;

    af_status_t rc = read_literals(data, len, literal_buf, literal_cap,
                                   &lit_len, &consumed, why);
    if (af_status_err(rc)) {
        return rc;
    }
    if (consumed > len) {
        *why = "literal section overruns the block";
        return AF_ERR_FS_CORRUPT;
    }

    const af_u8 *seq = data + consumed;
    af_size seq_len = len - consumed;

    if (seq_len == 0) {
        *why = "compressed block has no sequences section";
        return AF_ERR_FS_CORRUPT;
    }

    // --- how many sequences ---------------------------------------------------
    const af_u8 b0 = seq[0];
    af_u32 count = 0;
    af_size at = 1;

    if (b0 == 0) {
        count = 0;
    } else if (b0 < 128) {
        count = b0;
    } else if (b0 < 255) {
        if (seq_len < 2) {
            *why = "sequence count is truncated";
            return AF_ERR_FS_CORRUPT;
        }
        count = ((af_u32)(b0 - 128) << 8) + seq[1];
        at = 2;
    } else {
        if (seq_len < 3) {
            *why = "sequence count is truncated";
            return AF_ERR_FS_CORRUPT;
        }
        count = (af_u32)seq[1] + ((af_u32)seq[2] << 8) + 0x7F00u;
        at = 3;
    }

    // A block with no sequences is literals and nothing else. Legal, common,
    // and the case an implementation that always reads a modes byte gets wrong.
    if (count == 0) {
        for (af_size i = 0; i < lit_len; i++) {
            out_byte(out, literal_buf[i]);
        }
        if (out->full) {
            *why = "output buffer is full";
            return AF_ERR_TOOMANY;
        }
        return AF_OK;
    }

    if (at >= seq_len) {
        *why = "sequences section ends before the compression modes";
        return AF_ERR_FS_CORRUPT;
    }

    // THE MODES BYTE IS LAID OUT FROM THE BOTTOM, LL FIRST:
    //
    //     bits 1-0  reserved, must be zero
    //     bits 3-2  match length mode
    //     bits 5-4  offset mode
    //     bits 7-6  literal length mode
    //
    // The reserved bits are at the BOTTOM, which is the opposite of every other
    // field in this format — and assuming otherwise is exactly what made a first
    // block report "a sequence table repeats a previous one, but there is none":
    // the literal-length mode was being read out of the reserved bits.
    const af_u8 modes = seq[at++];
    ZTRACE("       modes byte 0x%02x at %lu (ll=%u of=%u ml=%u)", modes,
           (unsigned long)(at - 1), (unsigned)((modes >> 6) & 3u),
           (unsigned)((modes >> 4) & 3u), (unsigned)((modes >> 2) & 3u));
    if ((modes & 0x03u) != 0) {
        *why = "sequence symbol-compression modes has reserved bits set";
        return AF_ERR_FS_CORRUPT;
    }

    fbits_t fb;
    fb_init(&fb, seq + at, seq_len - at);

    seq_table_t ll_tab, of_tab, ml_tab;

    // The tables are read in the order LL, OF, ML, and each mode comes from its
    // own field in the byte rather than from its position in the stream.
    const af_u32 ll_mode = (af_u32)((modes >> 6) & 3u);
    const af_u32 of_mode = (af_u32)((modes >> 4) & 3u);
    const af_u32 ml_mode = (af_u32)((modes >> 2) & 3u);

    rc = seq_table_read(&ll_tab, &fb, ll_mode, s_ll_default, 36, 6,
                        ZSTD_MAX_LL_CODE, *have_previous ? ll_prev : NULL, why);
    if (af_status_err(rc)) {
        return rc;
    }
    rc = seq_table_read(&of_tab, &fb, of_mode, s_of_default, 29, 5,
                        ZSTD_MAX_OFFSET_CODE, *have_previous ? of_prev : NULL, why);
    if (af_status_err(rc)) {
        return rc;
    }
    rc = seq_table_read(&ml_tab, &fb, ml_mode, s_ml_default, 53, 6,
                        ZSTD_MAX_ML_CODE, *have_previous ? ml_prev : NULL, why);
    if (af_status_err(rc)) {
        return rc;
    }

    const af_size header_bytes = fb_bytes_used(&fb);

    ZTRACE("block: lit_len=%lu consumed=%lu seq_len=%lu count=%u modes=0x%02x "
           "tables=%lu",
           (unsigned long)lit_len, (unsigned long)consumed,
           (unsigned long)seq_len, count, modes, (unsigned long)header_bytes);
    ZTRACE("       ll_mode=%u of_mode=%u ml_mode=%u  fb bits=%lu bytes=%lu",
           (unsigned)((modes >> 6) & 3u), (unsigned)((modes >> 4) & 3u),
           (unsigned)((modes >> 2) & 3u),
           (unsigned long)fb.bitpos, (unsigned long)header_bytes);

    if (header_bytes >= seq_len - at) {
        *why = "sequences section has no bitstream after its tables";
        return AF_ERR_FS_CORRUPT;
    }

    zbits_t z;
    rc = zb_init(&z, seq + at + header_bytes, seq_len - at - header_bytes, why);
    ZTRACE("       stream %lu bytes, %lld valid bits, final byte 0x%02x",
           (unsigned long)(seq_len - at - header_bytes), (long long)z.bitpos,
           seq[seq_len - 1]);
    if (af_status_err(rc)) {
        return rc;
    }

    // --- states, initialised LL, OF, ML --------------------------------------
    //
    // RFC 8878 §3.1.1.3.2.1.3: "It starts with Literals_Length_State, followed
    // by Offset_State, and finally Match_Length_State." The order is fixed by the
    // format and is NOT the order the codes are read in, nor the order the extra
    // bits are read in, nor the order the states are updated in. Four different
    // orders in one loop, none of them marked.
    fse_state_t s_ll, s_of, s_ml;
    bool have_ll = false, have_of = false, have_ml = false;

    if (!ll_tab.is_rle) { fse_init_state(&s_ll, &z, &ll_tab.dt); have_ll = true; }
    if (!of_tab.is_rle) { fse_init_state(&s_of, &z, &of_tab.dt); have_of = true; }
    if (!ml_tab.is_rle) { fse_init_state(&s_ml, &z, &ml_tab.dt); have_ml = true; }

    ZTRACE("       states ll=%u/%u of=%u/%u ml=%u/%u rle=%d%d%d",
           s_ll.state, ll_tab.dt.table_log, s_of.state, of_tab.dt.table_log,
           s_ml.state, ml_tab.dt.table_log, (int)ll_tab.is_rle, (int)of_tab.is_rle,
           (int)ml_tab.is_rle);

    af_u32 rep0 = 1, rep1 = 4, rep2 = 8;
    af_size lit_at = 0;

    for (af_u32 i = 0; i < count; i++) {
        // The codes come from the states without touching the bitstream. Reading
        // the symbol and renormalising the state are separate steps; see the
        // comment on fse_peek.
        const af_u32 ll_code = seq_peek(&ll_tab, &s_ll, have_ll);
        const af_u32 ml_code = seq_peek(&ml_tab, &s_ml, have_ml);
        const af_u32 of_code = seq_peek(&of_tab, &s_of, have_of);

        if (ll_code > ZSTD_MAX_LL_CODE || ml_code > ZSTD_MAX_ML_CODE ||
            of_code > ZSTD_MAX_OFFSET_CODE) {
            *why = "a sequence code is outside its alphabet";
            return AF_ERR_FS_CORRUPT;
        }

        // "Decoding starts by reading the Number_of_Bits required to decode
        // offset. It does the same for Match_Length and then for
        // Literals_Length." — OF, ML, LL, which is the reverse of the state
        // order above and matches neither the code order nor the update order.
        const af_u32 offset_value = of_base(of_code) + zb_read(&z, of_code);
        const af_u32 match_length =
            s_ml_base[ml_code] + zb_read(&z, s_ml_bits[ml_code]);
        const af_u32 literal_length =
            s_ll_base[ll_code] + zb_read(&z, s_ll_bits[ll_code]);

        ZTRACE("  seq %u: ll=%u ml=%u of=%u -> llv=%u mlv=%u ofv=%u  bits left %lld",
               i, ll_code, ml_code, of_code, literal_length, match_length,
               offset_value, (long long)z.bitpos);

        if (z.overrun) {
            *why = "sequences bitstream ended in the middle of a sequence";
            return AF_ERR_FS_CORRUPT;
        }

        // --- the offset, including the repeat scheme --------------------------
        //
        // RFC 8878 §3.1.1.5. THREE rules, and the third is the one everybody
        // gets wrong:
        //
        //   1. The history is rep0 = most recent, rep1, rep2.
        //   2. "when the current sequence's literals_length is 0, repeated
        //      offsets are shifted by 1" — so offset_value 1 means rep1 rather
        //      than rep0, 2 means rep2, and 3 means rep0 - 1.
        //   3. The history is updated after EVERY sequence. When the offset came
        //      out of the list the list is rotated so the used entry becomes the
        //      most recent; when it did not — offset_value > 3, or offset_value 3
        //      with no literals, where the value is derived rather than reused —
        //      the history is shifted and the resolved offset is inserted.
        //
        // Treating offset_value 3 with no literals as a read that leaves the
        // history alone is the natural reading and it is wrong: the offset after
        // it is a value that has never been seen, and the next sequence that
        // refers to the history must find it in first place.
        af_u32 offset;

        if (offset_value > 3) {
            offset = offset_value - 3;
            rep2 = rep1;
            rep1 = rep0;
            rep0 = offset;
        } else if (literal_length == 0) {
            if (offset_value == 1) {
                offset = rep1;
                rep1 = rep0;
                rep0 = offset;
            } else if (offset_value == 2) {
                offset = rep2;
                rep2 = rep1;
                rep1 = rep0;
                rep0 = offset;
            } else {
                offset = rep0 - 1;             // rep0 is never 0
                rep2 = rep1;
                rep1 = rep0;
                rep0 = offset;
            }
        } else if (offset_value == 1) {
            offset = rep0;                     // already the most recent: no change
        } else if (offset_value == 2) {
            offset = rep1;
            rep1 = rep0;
            rep0 = offset;
        } else {
            offset = rep2;
            rep2 = rep1;
            rep1 = rep0;
            rep0 = offset;
        }

        if (offset == 0) {
            *why = "a sequence resolves to an offset of zero";
            return AF_ERR_FS_CORRUPT;
        }

        // --- execute ----------------------------------------------------------
        if (lit_at + literal_length > lit_len) {
            *why = "a sequence asks for more literals than the block has";
            return AF_ERR_FS_CORRUPT;
        }
        for (af_u32 k = 0; k < literal_length; k++) {
            out_byte(out, literal_buf[lit_at++]);
        }
        if (out->full) {
            *why = "output buffer is full";
            return AF_ERR_TOOMANY;
        }

        if (out->used < offset) {
            *why = "a sequence's offset reaches before the start of the output";
            return AF_ERR_FS_CORRUPT;
        }
        out_copy(out, offset, match_length);
        if (out->full) {
            *why = "output buffer is full";
            return AF_ERR_TOOMANY;
        }

        // "If it is not the last sequence in the block, the next operation is to
        // update states... Literals_Length_State is updated, followed by
        // Match_Length_State, and then Offset_State."
        //
        // SKIPPING THE LAST UPDATE IS NOT AN OPTIMISATION. The final states are
        // never used again, so the compressor never writes their renormalisation
        // bits; updating them would consume bits that do not exist and truncate
        // the stream by up to 20 bits — which is the whole remaining bitstream in
        // a one-sequence block.
        if (i + 1 < count) {
            seq_advance(&ll_tab, &s_ll, &z, have_ll);
            seq_advance(&ml_tab, &s_ml, &z, have_ml);
            seq_advance(&of_tab, &s_of, &z, have_of);

            if (z.overrun) {
                *why = "sequences bitstream ended while updating FSE states";
                return AF_ERR_FS_CORRUPT;
            }
        }
    }

    // RFC 8878 §3.1.1.3.2.1.3: "At the end, the bitstream shall be entirely
    // consumed; otherwise, the bitstream is considered corrupted." Checking it
    // turns every remaining desynchronisation into a named failure instead of
    // output that merely looks wrong.
    if (!zb_empty(&z)) {
        ZTRACE("       %lld bits left unconsumed at the end of the block",
               (long long)z.bitpos);
        *why = "sequences bitstream was not fully consumed";
        return AF_ERR_FS_CORRUPT;
    }

    while (lit_at < lit_len) {
        out_byte(out, literal_buf[lit_at++]);
    }
    if (out->full) {
        *why = "output buffer is full";
        return AF_ERR_TOOMANY;
    }

    // Keep these tables for a "repeat" mode in the next block.
    *ll_prev = ll_tab;
    *of_prev = of_tab;
    *ml_prev = ml_tab;
    *have_previous = true;

    return AF_OK;
}

// =============================================================================
// XXH64, for the frame's optional content checksum
//
// A Zstandard frame's Content_Checksum is the low 32 bits of XXH64 of the
// DECOMPRESSED data with seed 0. It is optional, and it is the only thing in the
// format that detects corruption which still decodes — a flipped bit inside
// literal data usually produces a perfectly valid frame, a perfectly valid tar,
// and a package that installs with one wrong byte in it.
//
// The frame header says whether the checksum is there, so a frame without one is
// not a failure; a frame WITH one that disagrees is. That is the whole contract,
// and it is worth having: it is the same guarantee the gzip reader already gives
// through CRC32, and without it the two readers would offer different protection
// for the same class of bug.
//
// The algorithm is XXH64 as published, and it is verified here against frames the
// reference implementation produced with its checksum flag on — which is the only
// reason to trust it.
// =============================================================================
#define XXH_PRIME64_1 0x9E3779B185EBCA87ull
#define XXH_PRIME64_2 0xC2B2AE3D27D4EB4Full
#define XXH_PRIME64_3 0x165667B19E3779F9ull
#define XXH_PRIME64_4 0x85EBCA77C2B2AE63ull
#define XXH_PRIME64_5 0x27D4EB2F165667C5ull

static af_u64 rotl64(af_u64 x, af_u32 r)
{
    return (x << r) | (x >> (64u - r));
}

static af_u64 rd64le(const af_u8 *p)
{
    af_u64 v = 0;
    for (af_u32 i = 0; i < 8; i++) {
        v |= (af_u64)p[i] << (8u * i);
    }
    return v;
}

static af_u64 rd32le(const af_u8 *p)
{
    return (af_u64)p[0] | ((af_u64)p[1] << 8) | ((af_u64)p[2] << 16) |
           ((af_u64)p[3] << 24);
}

static af_u64 xxh64_round(af_u64 acc, af_u64 input)
{
    acc += input * XXH_PRIME64_2;
    acc = rotl64(acc, 31);
    acc *= XXH_PRIME64_1;
    return acc;
}

static af_u64 xxh64_merge(af_u64 acc, af_u64 val)
{
    acc ^= xxh64_round(0, val);
    return acc * XXH_PRIME64_1 + XXH_PRIME64_4;
}

static af_u64 xxh64(const af_u8 *p, af_size len)
{
    const af_u8 *const end = p + len;
    af_u64 h;

    if (len >= 32) {
        const af_u8 *const limit = end - 32;
        af_u64 v1 = XXH_PRIME64_1 + XXH_PRIME64_2;
        af_u64 v2 = XXH_PRIME64_2;
        af_u64 v3 = 0;
        af_u64 v4 = (af_u64)0 - XXH_PRIME64_1;

        do {
            v1 = xxh64_round(v1, rd64le(p)); p += 8;
            v2 = xxh64_round(v2, rd64le(p)); p += 8;
            v3 = xxh64_round(v3, rd64le(p)); p += 8;
            v4 = xxh64_round(v4, rd64le(p)); p += 8;
        } while (p <= limit);

        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = xxh64_merge(h, v1);
        h = xxh64_merge(h, v2);
        h = xxh64_merge(h, v3);
        h = xxh64_merge(h, v4);
    } else {
        h = XXH_PRIME64_5;
    }

    h += (af_u64)len;

    while ((af_size)(end - p) >= 8) {
        h ^= xxh64_round(0, rd64le(p));
        h = rotl64(h, 27) * XXH_PRIME64_1 + XXH_PRIME64_4;
        p += 8;
    }

    if ((af_size)(end - p) >= 4) {
        h ^= rd32le(p) * XXH_PRIME64_1;
        h = rotl64(h, 23) * XXH_PRIME64_2 + XXH_PRIME64_3;
        p += 4;
    }

    while (p < end) {
        h ^= (af_u64)(*p) * XXH_PRIME64_5;
        h = rotl64(h, 11) * XXH_PRIME64_1;
        p++;
    }

    h ^= h >> 33;
    h *= XXH_PRIME64_2;
    h ^= h >> 29;
    h *= XXH_PRIME64_3;
    h ^= h >> 32;
    return h;
}

// =============================================================================
// The frame
// =============================================================================
af_status_t afpkg_zstd(const af_u8 *data, af_size len,
                       af_u8 *out_base, af_size out_size,
                       af_size *out_len, const char **why)
{
    if (out_len != NULL) {
        *out_len = 0;
    }

    if (data == NULL || out_base == NULL || out_len == NULL || len < 4) {
        *why = "no input or output buffer, or the frame is too short";
        return AF_ERR_INVAL;
    }

    const af_u32 magic = (af_u32)data[0] | ((af_u32)data[1] << 8) |
                         ((af_u32)data[2] << 16) | ((af_u32)data[3] << 24);
    if (magic != ZSTD_MAGIC) {
        *why = "not a Zstandard frame: the magic number is wrong";
        return AF_ERR_INVAL;
    }

    af_size at = 4;

    if (at >= len) {
        *why = "Zstandard frame ends before its header";
        return AF_ERR_FS_CORRUPT;
    }

    const af_u8 fhd = data[at++];
    const af_u32 fcs_flag = (af_u32)(fhd >> 6) & 3u;
    const bool single_segment = (fhd & 0x20u) != 0;
    const bool has_checksum = (fhd & 0x04u) != 0;
    const af_u32 did_flag = fhd & 3u;

    if ((fhd & 0x08u) != 0) {
        *why = "Zstandard frame header has its reserved bit set";
        return AF_ERR_FS_CORRUPT;
    }

    if (!single_segment) {
        if (at >= len) {
            *why = "Zstandard frame header is truncated";
            return AF_ERR_FS_CORRUPT;
        }
        at++;   // window descriptor; the window is bounded by the buffer here
    }

    const af_u32 did_size = (did_flag == 0) ? 0 : (1u << (did_flag - 1));
    if (did_size > 0) {
        *why = "this Zstandard frame requires a dictionary, which is not "
               "supported";
        return AF_ERR_NOTSUP;
    }

    af_u32 fcs_size = (fcs_flag == 0) ? (single_segment ? 1u : 0u)
                                      : (1u << fcs_flag);

    af_u64 content_size = 0;
    if (fcs_size > 0) {
        if (at + fcs_size > len) {
            *why = "Zstandard frame header is truncated";
            return AF_ERR_FS_CORRUPT;
        }
        for (af_u32 i = 0; i < fcs_size; i++) {
            content_size |= (af_u64)data[at + i] << (8 * i);
        }
        if (fcs_size == 2) {
            content_size += 256;    // the format's oddity for a 2-byte size
        }
        at += fcs_size;
    }

    outbuf_t out;
    out.base = out_base;
    out.size = out_size;
    out.used = 0;
    out.full = false;

    static af_u8 s_literals[ZSTD_MAX_LITERALS];

    seq_table_t ll_prev, of_prev, ml_prev;
    zd_memset(&ll_prev, 0, sizeof(ll_prev));
    zd_memset(&of_prev, 0, sizeof(of_prev));
    zd_memset(&ml_prev, 0, sizeof(ml_prev));
    bool have_previous = false;

    af_u32 blocks = 0;

    while (true) {
        if (at + 3 > len) {
            *why = "Zstandard frame ends before a block header";
            return AF_ERR_FS_CORRUPT;
        }

        const af_u32 header = (af_u32)data[at] |
                              ((af_u32)data[at + 1] << 8) |
                              ((af_u32)data[at + 2] << 16);
        at += 3;

        const bool last_block = (header & 1u) != 0;
        const af_u32 block_type = (header >> 1) & 3u;
        const af_u32 block_size = header >> 3;

        if (block_type == 3) {
            *why = "Zstandard block type 3 is reserved and must not appear";
            return AF_ERR_FS_CORRUPT;
        }

        ZTRACE("block header: last=%d type=%u size=%u at=%lu of %lu",
               (int)last_block, block_type, block_size, (unsigned long)at,
               (unsigned long)len);

        if (block_type == 0) {          // Raw
            if (at + block_size > len) {
                *why = "a raw block runs past the end of the frame";
                return AF_ERR_FS_CORRUPT;
            }
            for (af_u32 i = 0; i < block_size; i++) {
                out_byte(&out, data[at + i]);
            }
            at += block_size;
        } else if (block_type == 1) {    // RLE
            if (at + 1 > len) {
                *why = "an RLE block has no byte";
                return AF_ERR_FS_CORRUPT;
            }
            for (af_u32 i = 0; i < block_size; i++) {
                out_byte(&out, data[at]);
            }
            at += 1;
        } else {                         // Compressed
            if (at + block_size > len) {
                *why = "a compressed block runs past the end of the frame";
                return AF_ERR_FS_CORRUPT;
            }
            const af_status_t rc = decode_compressed_block(
                data + at, block_size, s_literals, sizeof(s_literals), &out,
                &ll_prev, &of_prev, &ml_prev, &have_previous, why);
            if (af_status_err(rc)) {
                return rc;
            }
            at += block_size;
        }

        if (out.full) {
            *why = "the decompressed frame does not fit the output buffer";
            return AF_ERR_TOOMANY;
        }

        if (last_block) {
            break;
        }

        if (++blocks > 0x100000u) {
            *why = "the frame contains an implausible number of blocks";
            return AF_ERR_FS_CORRUPT;
        }
    }

    // The content checksum follows the last block, when the header said it would.
    if (has_checksum) {
        if (at + 4 > len) {
            *why = "the frame claims a checksum but has no room for one";
            return AF_ERR_FS_CORRUPT;
        }

        // The stored value is the LOW 32 bits of XXH64, little-endian. A full
        // 64-bit compare would be wrong: the high half is not in the file.
        const af_u32 stored = (af_u32)data[at] |
                              ((af_u32)data[at + 1] << 8) |
                              ((af_u32)data[at + 2] << 16) |
                              ((af_u32)data[at + 3] << 24);
        const af_u32 computed = (af_u32)xxh64(out_base, out.used);

        if (stored != computed) {
            *why = "the frame's content checksum does not match the decompressed "
                   "bytes — the frame is corrupt";
            return AF_ERR_FS_CORRUPT;
        }
    }

    if (fcs_size > 0 && content_size != out.used) {
        *why = "the decompressed length does not match the frame header's";
        return AF_ERR_FS_CORRUPT;
    }

    *out_len = out.used;
    *why = "decompressed";
    return AF_OK;
}
