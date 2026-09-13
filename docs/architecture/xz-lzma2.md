# xz and LZMA2 — what is done, and what the rest costs

**Status at v0.10a:** the `.xz` **container is complete** and in the test gate.
The **LZMA2 chunk layer** is complete for *uncompressed* chunks. **LZMA-compressed
chunks are refused**, with a message naming the chunk type.

This document exists because the remaining work has a property none of the other
decoders in this project had, and it changes how the work has to be done.

---

## 1. The property that changes everything: there is no specification

DEFLATE has RFC 1951. Zstandard has RFC 8878. Both were read, both were quoted
in the source, and both caught mistakes.

**LZMA2 has no published prose specification.** There is:

* `xz-file-format.txt`, which specifies the **container** — the stream header,
  the blocks, the filter chain, the index, the footer, both CRCs. This decoder's
  container layer was written from it, including its reference CRC code.
* The **LZMA SDK** and **xz-embedded**, which *are* the definition of the LZMA2
  chunk format and of LZMA itself. Both are public domain / 0BSD.

So the chunk format below is transcribed from `xz_dec_lzma2.c`, and that has a
consequence worth stating plainly:

> **ADR-015's byte-exact-vector rule is not a good practice here. It is the only
> verification that exists.** There is no prose to check a reading against, no
> second description to catch a misread. A decoder written from the reference
> implementation and tested against fixtures from the same reference
> implementation is checked by exactly one thing: whether the bytes come out
> right.

That is a weaker position than DEFLATE and Zstandard were in, and it is recorded
rather than glossed. It is also why the vectors come from the *encoder* — a
different code path in the same library, and the only independent artefact
available.

---

## 2. What is implemented, and what it is verified against

| Layer | State | Verified by |
| --- | --- | --- |
| Stream header, flags, CRC32 | **done** | 12 reference streams, byte-exact |
| Block header, flags, filter list, CRC32 | **done** | same |
| LZMA2 chunk framing + uncompressed chunks | **done** | same |
| Block padding, Check (CRC32 *and* CRC64) | **done** | same |
| Index, records, CRC32, footer, padding | **done** | same, plus 4 corruption cases |
| LZMA-compressed chunks | **refused** | 6 reference streams asserted to be refused |
| Delta and BCJ filters | **refused by name** | — |
| Multi-stream concatenation | **refused** | — |
| SHA-256 check | **refused** | — |

Everything in the *done* rows is checked by `tests/native/test_afpkg.c` and runs
in `tools/native_test.sh`, which is a step in the CI gate.

---

## 3. The container, in one page

Confirmed field by field against a real file rather than only against the
document:

```
Stream Header    6 bytes magic  FD 37 7A 58 5A 00
                 2 bytes flags  [0x00, check-id]
                 4 bytes CRC32  of the two flag bytes

Block            Block Header   size byte: real size = (value + 1) * 4
                                flags: bits 0-1 filters-1, 2-5 reserved,
                                       0x40 Compressed Size present,
                                       0x80 Uncompressed Size present
                                [Compressed Size]    VLI
                                [Uncompressed Size]  VLI
                                for each filter: VLI id, VLI props-size, props
                                padding to the header size, then CRC32 of
                                everything before it
                 Compressed Data   LZMA2 chunk stream (filter 0x21)
                 Block Padding     0-3 zero bytes, to a multiple of four
                 Check             0, 4 (CRC32) or 8 (CRC64) bytes

Index            0x00 indicator
                 VLI number of records
                 records: VLI Unpadded Size, VLI Uncompressed Size
                 padding to a multiple of four, then CRC32

Stream Footer    CRC32 of the next 6 bytes, Backward Size (u32le),
                 flags (must equal the header's), "YZ"
```

**Unpadded Size excludes Block Padding.** It is the one field whose name says
what it is and whose arithmetic was still wrong on the first attempt — the first
version added the padding, which made every stream with a non-multiple-of-four
compressed size fail its own index check.

**The Index is a claim, and it is checkable.** A decoder that skips it accepts a
stream whose index disagrees with its contents, which is the shape of a truncated
or spliced file that still decompresses. It is verified.

**CRC64 is not optional in practice.** The reference tools write check id 4 by
default, so a decoder that handled only CRC32 would refuse essentially every
`.xz` file in existence.

**Magic decides, names do not** (ADR-013, applied to package members): `.gz`,
`.xz`, `.zst` and `bz2` are recognised by their bytes, and `bz2` is refused *by
name* because being told "bzip2 is not supported" is more useful than being told
the tar header at offset 0 is bad.

---

## 4. The LZMA2 chunk layer

Transcribed from `xz-embedded`'s `xz_dec_lzma2.c`. Five groups of control byte:

```
0x00        end of the LZMA2 stream
0x01        dictionary reset, then an UNCOMPRESSED chunk
0x02        an UNCOMPRESSED chunk, no reset
0x03..0x7F  reserved and invalid
0x80..0xFF  an LZMA-compressed chunk
```

For a compressed chunk, the **low five bits are the top five bits of the
uncompressed size** (bits 16-20), and the **top three bits say which resets
happen first**:

```
0x80  nothing is reset
0xA0  state reset, old properties
0xC0  state reset, new properties follow
0xE0  dictionary reset, state reset, new properties
```

Then: two bytes of uncompressed size (the low 16 bits), two bytes of compressed
size (minus one), and — for 0xC0 and 0xE0 — a properties byte.

Rules a decoder must enforce, each of which is a rejection rather than a guess:

* the first chunk of each **block** (not file) must reset the dictionary;
* the first LZMA chunk must carry properties;
* `lc + lp <= 4`, and the properties byte is at most `(4*5+4)*9+8`;
* at the end of a chunk, the compressed size must be exhausted, no pending match
  may remain, and the range decoder must be finished (`code == 0`) — all three,
  or the chunk is corrupt.

An **uncompressed chunk** is a three-byte header — control, then size minus one
as a big-endian 16-bit value — followed by that many bytes copied verbatim. It is
what makes incompressible data not expand, which is why it is reachable from a
real file and why it was the right first slice.

*Completed 2026-09-13. The fixture generator classifies every stream it writes by
walking these control bytes (`has_lzma_chunk` in `tools/pkgsynth.py`), so the
decodable vectors and the refusal vectors are separated by the format's own
structure rather than by a hand-written list.*

---

## 5. What LZMA-compressed chunks cost

The remaining work is one function and its support:

1. **Range decoder** — `range`, `code`, five initialisation bytes of which the
   first is discarded, `RC_TOP_VALUE = 1 << 24`, normalisation before every bit.
2. **Bit model** — 11-bit probabilities, `RC_MOVE_BITS = 5`, adapt up on 0 and
   down on 1. About 15 lines, and every one of them is load-bearing.
3. **State machine** — 12 states, `is_match[state][pos_state]`, `is_rep`,
   `is_rep0/1/2`, `is_rep0_long`, with the exact transition table.
4. **Literal coder** — 0x300-entry trees per context, 16 contexts
   (`lc + lp` based), plus the **matched literal** path used when the previous
   symbol was a match/rep, which decodes against the byte at `rep0`.
5. **Length coder** — low/mid/high trees, `LEN_LOW_SYMBOLS = 8`,
   `LEN_MID = 8`, `LEN_HIGH = 256`, `MATCH_LEN_MIN = 2`.
6. **Distance coder** — `DIST_SLOTS = 64`, `DIST_MODEL_START = 4`,
   `DIST_MODEL_END = 14`, `ALIGN_BITS = 4`, the `dist_special` reverse trees and
   `dist_align`.
7. **Repeat distances** — `rep0..rep3`, with `lzma_rep_match`'s four-way
   decision and its state transitions.

The probability arrays total roughly **12 KiB** and are all initialised to
`2048 / 2`.

**Measured expectation, from this project's own history:** DEFLATE took one
milestone and two bugs. Zstandard took two milestones and **nine** bugs, six of
which produced output of the right length with the wrong bytes. LZMA is a state
machine with four interleaved symbol types and a matched-literal path that only
one branch of the input reaches — and unlike Zstandard, **there is no
specification to re-read when something is wrong.**

So the honest estimate is: a milestone of its own, and the debugging loop will be
"compare against a reference vector, bisect the symbol stream, read the reference
implementation again" rather than "read the spec again".

---

## 6. How to do it, in order

1. **Fixtures first, and the ones with *uncompressed* chunks kept.** They already
   exist. When a bug appears, the split between "streams this build already
   decoded" and "streams it now fails" is what localises it.
2. **The range decoder alone, verified on its own.** Add a test that decodes a
   known chunk and asserts the first N symbols. A range decoder that is wrong
   produces a stream of plausible, wrong bits — the same failure mode as the
   zstd backward reader, and the same remedy.
3. **Literals only.** A stream of LZMA chunks with `is_match` always 0 is a legal
   LZMA2 stream and exercises items 1, 2 and 4 without the match path.
4. **Then matches, one length class at a time** — 2, 3, 4, then 5+ — because
   `dist_slot` uses a different probability tree for each.
5. **Then repeats.** `is_rep`, and the `rep0_long` length-of-one case.
6. **Then the matched literal.** It is the branch that is unreachable until
   matches work, and therefore the one that will be written last and tested
   least.
7. **Then the filters** — delta (0x03) and x86 BCJ (0x04) between them cover most
   real streams.

At every step the assertion is the same: byte-exact output against a stream the
reference encoder produced. Nothing weaker can see a bug in this file.
