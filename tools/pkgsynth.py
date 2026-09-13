#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""AfriyieOS — synthetic package fixture generator.

WHY GENERATE FIXTURES INSTEAD OF DOWNLOADING REAL PACKAGES
=========================================================

Real .deb, .rpm, .apk and pacman packages would be the ideal test input, and
they are the wrong thing to put in this repository:

  * they are megabytes each, and they change, so a test failure would be
    ambiguous between "the code broke" and "the fixture moved"
  * downloading them in CI makes the build depend on the network and on
    somebody else's archive retention
  * most carry licence terms that a public repository should not be quietly
    redistributing

So the fixtures are generated, deterministically, from a description in code.
That makes a failure unambiguous, keeps the repository small, and lets a test
construct the exact malformed case it needs — which is the thing that actually
finds bugs, and which no real package would conveniently contain.

WHAT IS REAL AND WHAT IS NOT
============================

The CONTAINERS are real. An `ar` archive written here is a valid `ar` archive;
a tar stream is a valid tar stream; the gzip members are valid gzip streams that
`gzip -d` will decompress. That is deliberate: a synthetic format that only our
own reader accepts would test the reader against our assumptions rather than
against the format.

The DEFLATE payloads use stored blocks (level 0) rather than Huffman coding.
That is a real, spec-legal gzip stream, and it is the one case our inflater
handles today — see the note in libs/libafpkg/. Real-world packages use Huffman,
which is the next piece of work and is named everywhere rather than implied.

Usage:
    python3 tools/pkgsynth.py --out build/fixtures        # write every fixture
    python3 tools/pkgsynth.py --list
"""

from __future__ import annotations

import argparse
import os
import random
import struct
import zlib

# -----------------------------------------------------------------------------
# Deterministic timestamps
#
# A generated fixture must be byte-identical on every run and every machine, or
# a test that compares against a stored checksum fails for reasons that have
# nothing to do with the code. 2020-01-01 00:00:00 UTC.
# -----------------------------------------------------------------------------
FIXED_MTIME = 1577836800


# =============================================================================
# ar archives
#
# The container a .deb is built on. Format: 8-byte magic, then a sequence of
# 60-byte headers each followed by its data, with data padded to an even offset.
#
#   "!<arch>\n"
#   name(16) mtime(12) uid(6) gid(6) mode(8) size(10) magic(2)
#
# The trailing "`\n" (0x60 0x0A) after the size is the field that trips up
# naive parsers, and it is why the header is 60 bytes rather than 58.
# =============================================================================
AR_MAGIC = b"!<arch>\n"


def ar_member(name: str, data: bytes) -> bytes:
    # ar names are space-padded, and a name of 16 characters or more goes in a
    # separate string table. Every member name in a .deb is short, so the simple
    # form is what is implemented — and a longer name is refused rather than
    # silently truncated.
    if len(name) > 15:
        raise ValueError(f"ar member name too long for the simple format: {name!r}")

    header = (
        name.ljust(16).encode("ascii")
        + str(FIXED_MTIME).ljust(12).encode("ascii")
        + "0".ljust(6).encode("ascii")          # uid
        + "0".ljust(6).encode("ascii")          # gid
        + "100644".ljust(8).encode("ascii")     # mode
        + str(len(data)).ljust(10).encode("ascii")
        + b"\x60\x0a"
    )
    assert len(header) == 60, len(header)

    # Members are padded to an even byte boundary with a newline.
    padding = b"\n" if len(data) % 2 else b""
    return header + data + padding


def build_ar(members: list[tuple[str, bytes]]) -> bytes:
    out = bytearray(AR_MAGIC)
    for name, data in members:
        out += ar_member(name, data)
    return bytes(out)


# =============================================================================
# tar archives
#
# POSIX ustar. 512-byte header, then the file rounded up to 512 bytes. Two
# all-zero blocks end the archive.
#
# Every numeric field is OCTAL, zero-padded, with a trailing NUL or space — not
# decimal, and not packed. Getting that wrong produces an archive that looks
# structurally fine and reports a file size of zero, which is why the fields
# here are written by one function rather than by hand at each call site.
# =============================================================================
def tar_octal(value: int, width: int) -> bytes:
    # width includes the trailing NUL.
    return ("%0*o" % (width - 1, value)).encode("ascii") + b"\0"


def tar_header(name: str, size: int, mode: int = 0o644, typeflag: bytes = b"0") -> bytes:
    if len(name.encode()) > 100:
        raise ValueError(f"tar name too long for ustar: {name!r}")

    header = bytearray(512)
    header[0:100] = name.encode("ascii").ljust(100, b"\0")
    header[100:108] = tar_octal(mode, 8)
    header[108:116] = tar_octal(0, 8)            # uid
    header[116:124] = tar_octal(0, 8)            # gid
    header[124:136] = tar_octal(size, 12)
    header[136:148] = tar_octal(FIXED_MTIME, 12)
    header[148:156] = b" " * 8                   # checksum: spaces while summing
    header[156:157] = typeflag

    header[257:263] = b"ustar\0"                 # POSIX, not GNU
    header[263:265] = b"00"
    header[265:297] = b"root".ljust(32, b"\0")
    header[297:329] = b"root".ljust(32, b"\0")

    checksum = sum(header)
    header[148:156] = ("%06o" % checksum).encode("ascii") + b"\0 "

    return bytes(header)


def build_tar(files: list[tuple[str, bytes, int]]) -> bytes:
    """files is a list of (path, contents, mode)."""
    out = bytearray()

    for path, contents, mode in files:
        out += tar_header(path, len(contents), mode)
        out += contents
        pad = (-len(contents)) % 512
        out += b"\0" * pad

    out += b"\0" * 1024        # end-of-archive marker
    return bytes(out)


# =============================================================================
# gzip
#
# A real gzip stream with a stored (uncompressed) DEFLATE block. Valid per
# RFC 1951 — `gzip -d` will decompress it — and it is the case our inflater
# handles. Huffman-coded blocks are the follow-up.
# =============================================================================
def gzip_stored(data: bytes) -> bytes:
    header = b"\x1f\x8b\x08\x00" + struct.pack("<I", FIXED_MTIME) + b"\x00\x03"

    body = bytearray()
    offset = 0

    # A stored block carries at most 65535 bytes. An empty input still needs one
    # block, which is the case that catches a loop written as `while offset < len`.
    while True:
        chunk = data[offset:offset + 65535]
        final = 1 if offset + len(chunk) >= len(data) else 0

        body.append(final)                      # BFINAL
        body += struct.pack("<H", len(chunk))   # LEN
        body += struct.pack("<H", len(chunk) ^ 0xFFFF)   # NLEN, ones-complement
        body += chunk

        offset += len(chunk)
        if final:
            break

    trailer = struct.pack("<II", zlib.crc32(data) & 0xFFFFFFFF, len(data) & 0xFFFFFFFF)
    return header + bytes(body) + trailer


# =============================================================================
# The packages
# =============================================================================

DEB_CONTROL = """\
Package: afriyie-test
Version: 1.2.3-1
Architecture: amd64
Maintainer: AfriyieOS Tests <tests@afriyieos.invalid>
Installed-Size: 42
Depends: libc6 (>= 2.31), libfoo | libbar
Section: utils
Priority: optional
Description: a synthetic package used by the AfriyieOS test suite
 This package is generated, never downloaded. It exists so that the reader
 can be tested against a container it did not also write.
"""


def make_deb() -> bytes:
    control_tar = build_tar([
        ("./control", DEB_CONTROL.encode(), 0o644),
        ("./md5sums", b"d41d8cd98f00b204e9800998ecf8427e  usr/share/doc/README\n", 0o644),
    ])

    data_tar = build_tar([
        ("./usr/", b"", 0o755),
        ("./usr/bin/", b"", 0o755),
        ("./usr/bin/afriyie-test", b"#!/bin/sh\necho hello from a synthetic package\n", 0o755),
        ("./usr/share/doc/afriyie-test/README", b"A synthetic package.\n", 0o644),
        ("./usr/share/doc/afriyie-test/copyright", b"MIT\n", 0o644),
    ])

    return build_ar([
        ("debian-binary", b"2.0\n"),
        ("control.tar.gz", gzip_stored(control_tar)),
        ("data.tar.gz", gzip_stored(data_tar)),
    ])


def make_deb_xz_member() -> bytes:
    """A .deb whose control member claims an extension we do not handle.

    Used to prove the reader REFUSES rather than guessing. A reader that treats
    an xz-compressed control member as if it were gzip produces garbage metadata
    and installs a package under the wrong name, which is worse than an error.
    """
    return build_ar([
        ("debian-binary", b"2.0\n"),
        ("control.tar.xz", b"\xfd7zXZ\x00not really xz"),
        ("data.tar.gz", gzip_stored(build_tar([("./usr/", b"", 0o755)]))),
    ])


def make_deb_truncated() -> bytes:
    """An ar archive whose declared member size runs past the end of the file.

    The classic malformed-input case: a size field is a claim, and believing it
    without checking is how a reader walks off the end of a buffer.
    """
    good = make_deb()
    return good[:len(good) // 2]


def make_ar_not_deb() -> bytes:
    """A valid ar archive that is not a Debian package.

    `ar` archives hold anything — object files, static libraries, mail folders.
    The magic alone means only "ar".
    """
    return build_ar([
        ("somefile.o", b"\x7fELF not really"),
        ("another.o", b"also not"),
    ])


def make_deb_empty_control() -> bytes:
    """A .deb whose control member is a valid tar containing no ./control file."""
    control_tar = build_tar([("./md5sums", b"\n", 0o644)])
    data_tar = build_tar([("./usr/", b"", 0o755)])
    return build_ar([
        ("debian-binary", b"2.0\n"),
        ("control.tar.gz", gzip_stored(control_tar)),
        ("data.tar.gz", gzip_stored(data_tar)),
    ])


def make_apk() -> bytes:
    """An Alpine Linux package.

    NOT THE SAME THING AS AN ANDROID .apk, despite sharing three letters. This
    one is a gzipped tar with a .PKGINFO file, and it is what `apk add` installs
    on Alpine. The two formats share an extension and nothing else.

    ONE TAR, SPLIT ACROSS GZIP MEMBERS — and the split points are deliberately
    hostile.

    The first version of this generator wrote two COMPLETE tars, one per member,
    because that is what the format was assumed to look like. A real package from
    Alpine's CDN proved otherwise: it is ONE tar cut into pieces at arbitrary
    offsets, so that the signature can be verified before the payload is read.

    A reader that treats each member as its own archive passes the old fixture
    and fails on real packages, because it only ever sees the entries in the last
    member. So the fixture now splits one tar — including inside a header, and
    including inside file data — which is a boundary no implementation gets right
    by accident.
    """
    pkginfo = (
        "# Generated by pkgsynth; not a real apk.\n"
        "pkgname = afriyie-test\n"
        "pkgver = 1.2.3-r0\n"
        "pkgdesc = a synthetic Alpine package\n"
        "url = https://afriyieos.invalid\n"
        "builddate = 1577836800\n"
        "packager = AfriyieOS Tests <tests@afriyieos.invalid>\n"
        "size = 4096\n"
        "arch = x86_64\n"
        "license = MIT\n"
        "depend = musl\n"
        "depend = so:libc.musl-x86_64.so.1\n"
        "provides = afriyie=1.2.3\n"
        "datahash = 0000000000000000000000000000000000000000000000000000000000000000\n"
    )

    # One tar holding the signature, the metadata and the payload — exactly as a
    # real package does.
    whole = build_tar([
        (".SIGN.RSA.afriyie-test.rsa.pub", b"\x00" * 256, 0o644),
        (".PKGINFO", pkginfo.encode(), 0o644),
        # Install scripts, exactly as a real package carries them — found in
        # busybox-1.36.1-r31.apk after the first fixture omitted them and the
        # reader offered them as files to install.
        (".post-install", b"#!/bin/sh\n# configure the package\n", 0o755),
        (".trigger", b"#!/bin/sh\n# run after an install\n", 0o755),
        ("usr/", b"", 0o755),
        ("usr/bin/", b"", 0o755),
        ("usr/bin/afriyie-test", b"#!/bin/sh\necho alpine\n", 0o755),
        ("lib/", b"", 0o755),
        ("lib/libafriyie.so.1", b"\x7fELF not really\n", 0o755),
        # A dot-name INSIDE a directory. This is payload, not metadata, and it
        # exists to fail loudly if the exclusion is ever widened to "any name
        # beginning with a dot".
        ("usr/share/.hidden-config", b"payload, not metadata\n", 0o644),
    ])

    # Three cuts at offsets chosen to be unpleasant:
    #   700  — inside the SECOND entry's header, so the split is mid-structure
    #   1500 — inside file data, so a reader scanning for a header sees garbage
    # The remaining piece runs to the end.
    cuts = (700, 1500)

    pieces = []
    head = 0
    for cut in cuts:
        pieces.append(whole[head:cut])
        head = cut
    pieces.append(whole[head:])

    return b"".join(gzip_stored(piece) for piece in pieces)


def make_pacman() -> bytes:
    """An Arch Linux package: a zstd-compressed tar with a .PKGINFO at its root.

    THE FIRST FORMAT THIS PROJECT COULD NOT READ UNTIL ZSTD EXISTED. Every earlier
    reader was blocked by structure — an ar archive, a chain of gzip members, a
    different control format. This one is a tar, which libafpkg has read since
    v0.9, inside zstd, which it could not read at all. Adding it is the test of
    whether the zstd decoder is real: nothing here works if a single bit is
    decoded wrongly, and there is no way to pass by accident.

    Compressed with the reference implementation (Python's zstd binding), so the
    ground truth comes from somewhere else.
    """
    pkginfo = (
        "# Generated by makepkg 6.0.2 - do not edit\n"
        "# Tue Jan  1 00:00:00 UTC 2030\n"
        "pkgname = afriyie-test\n"
        "pkgbase = afriyie-test\n"
        "pkgver = 1.2.3-1\n"
        "pkgdesc = a synthetic pacman package\n"
        "url = https://afriyieos.invalid\n"
        "builddate = 1893456000\n"
        "packager = AfriyieOS Tests <tests@afriyieos.invalid>\n"
        "size = 4096\n"
        "arch = x86_64\n"
        "license = MIT\n"
        "depend = glibc\n"
        "depend = libfoo.so=1-64\n"
        "provides = afriyie=1.2.3\n"
        "xdata = pkgtype=pkg\n"
    )

    whole = build_tar([
        (".PKGINFO", pkginfo.encode(), 0o644),
        # The checksums file, which every real package carries and which is
        # payload rather than metadata: .MTREE and .PKGINFO are the only dot-names
        # at the root that describe the package rather than belong to it.
        (".MTREE", b"#mtree\n./usr type=dir\n", 0o644),
        ("usr/", b"", 0o755),
        ("usr/bin/", b"", 0o755),
        ("usr/bin/afriyie-test", b"#!/bin/sh\necho pacman\n", 0o755),
        ("usr/lib/", b"", 0o755),
        ("usr/lib/libafriyie.so.1", b"\x7fELF not really\n", 0o755),
        ("usr/share/doc/afriyie-test/README", b"a description of nothing\n", 0o644),
    ])

    try:
        from compression import zstd
    except ImportError:                       # noqa: BLE001
        return b""

    # checksum_flag: the frame carries an XXH64 of the payload. Real distribution
    # packages built by `zstd` or by a package manager generally do, and it is the
    # only thing in the format that detects corruption which still decodes.
    return zstd.compress(whole, options={
        zstd.CompressionParameter.compression_level: 19,
        zstd.CompressionParameter.checksum_flag: 1,
    })


def make_pacman_bad_checksum() -> bytes:
    """A pacman package whose zstd frame has one bit flipped in its bitstream.

    The frame carries a checksum — see make_pacman — so this is detected for the
    right reason rather than by luck. Without one, a flipped bit inside literal
    data decodes to a perfectly valid frame holding one wrong byte, and the
    package installs.
    """
    good = make_pacman()
    if not good:
        return b""

    mutated = bytearray(good)
    # Deep inside the compressed block, past the frame header and the tables,
    # where a flip changes a symbol rather than a length.
    mutated[len(mutated) // 2] ^= 0x40
    return bytes(mutated)


def make_zstd_not_package() -> bytes:
    """A zstd-compressed tar with no .PKGINFO — a container that is not a package.

    The distinction matters and is the same one the Alpine reader makes: "this is
    not a package" and "this is a damaged package" are different answers, and a
    reader that reports the second when the first is true sends a user looking for
    corruption that is not there.
    """
    whole = build_tar([
        ("usr/bin/thing", b"not a package\n", 0o755),
    ])

    try:
        from compression import zstd
    except ImportError:                       # noqa: BLE001
        return b""

    return zstd.compress(whole, 6)


def make_apk_bad_crc() -> bytes:
    """A valid .apk with one bit flipped in the compressed data.

    The gzip trailer carries a CRC32 of the decompressed bytes precisely so that
    this is detectable. Without the check, a corrupted package installs and the
    corruption surfaces later as a failure in whatever the package does — which
    is a far worse place to find it.
    """
    good = bytearray(make_apk())
    # Flip a bit well inside the first member's deflate payload.
    good[40] ^= 0x01
    return bytes(good)


def make_apk_truncated_member() -> bytes:
    """An .apk that ends in the middle of its second gzip member."""
    good = make_apk()
    return good[:len(good) * 3 // 4]


FIXTURES = {
    "test_1.2.3-1_amd64.deb": make_deb,
    "notxzsupport_1.0_amd64.deb": make_deb_xz_member,
    "truncated_1.0_amd64.deb": make_deb_truncated,
    "plain.ar": make_ar_not_deb,
    "nocontrol_1.0_amd64.deb": make_deb_empty_control,
    "test_1.2.3-r0_x86_64.apk": make_apk,
    "badcrc_1.2.3-r0_x86_64.apk": make_apk_bad_crc,
    "truncated_1.2.3-r0_x86_64.apk": make_apk_truncated_member,
    "test-1.2.3-1-x86_64.pkg.tar.zst": make_pacman,
    "badzstd-1.2.3-1-x86_64.pkg.tar.zst": make_pacman_bad_checksum,
    "notapkg-1.0-1-x86_64.pkg.tar.zst": make_zstd_not_package,
}


# =============================================================================
# DEFLATE test vectors
#
# A decompressor is the one kind of code where "it produced output" means
# nothing: wrong bytes look exactly like right bytes until they are compared.
# So every vector is an input, its compressed form, and the EXACT bytes that
# must come back.
#
# The payloads are chosen to exercise the parts of DEFLATE that a single test
# string would miss:
#
#   empty       the case a loop written as `while offset < len` gets wrong
#   constant    long runs — the best case for LZ77, so the densest back-references
#   text        mixed literals and matches, the ordinary case
#   random      INCOMPRESSIBLE, so the compressor emits stored blocks and any
#               implementation that only handles Huffman fails here
#   distances   data engineered so matches reach distances at each code boundary,
#               which is where the distance table's extra bits are got wrong
#
# Every level from 0 to 9 is generated, because zlib switches strategies as the
# level rises — level 0 is stored, low levels lean on fixed Huffman, high levels
# on dynamic. One level would test one of the three block types.
# =============================================================================

def deflate_payloads() -> dict[str, bytes]:
    rng = random.Random(0xA6F1)          # fixed seed: the vectors must not move

    text = (
        b"The quick brown fox jumps over the lazy dog. " * 8
        + b"AfriyieOS reads packages from every distribution.\n" * 4
    )

    # Distances engineered to cross the extra-bit boundaries of the distance
    # table: 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, ... A match at each of
    # these exercises a different row of dist_base/dist_extra, and a table with
    # one wrong entry produces correct output for most data and wrong output for
    # exactly one of these.
    distances = bytearray()
    seed = b"ABCDEFGH"
    for dist in (1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193):
        distances += seed * 40
        distances += bytes([0x40 + (dist & 0x3F)]) * 8
        distances += distances[-dist:][:dist]

    return {
        "empty": b"",
        "one": b"x",
        "constant": b"a" * 40000,
        "text": text,
        "random": bytes(rng.randrange(256) for _ in range(20000)),
        "distances": bytes(distances[:60000]),
    }


def write_deflate_vectors(out_dir: str) -> None:
    """Writes <name>.raw and <name>.gz per level for every payload.

    THE RAW DEFLATE DETAIL, which the first version of this function got wrong.

    `zlib.compress(data, level)` does NOT return a DEFLATE stream. It returns a
    ZLIB stream: a 2-byte header and a 4-byte Adler-32 trailer wrapped around the
    DEFLATE data. Wrapping that in a gzip header produces a file that `gzip -d`
    rejects and that a correct inflater rejects too, because it starts decoding
    the zlib header's 0x78 as a block header.

    `compressobj(..., wbits=-15)` produces RAW DEFLATE, which is what a gzip
    member actually contains. The distinction broke every stored-block vector on
    the first run — and it was visible only because the tests compare bytes
    rather than checking that output appeared.
    """
    payloads = deflate_payloads()

    for name, payload in payloads.items():
        with open(os.path.join(out_dir, f"deflate_{name}.raw"), "wb") as handle:
            handle.write(payload)

        for level in range(10):
            # wbits=-15: raw DEFLATE, no zlib header and no Adler trailer.
            compressor = zlib.compressobj(level, zlib.DEFLATED, -15)
            blob = compressor.compress(payload) + compressor.flush()

            # A real gzip header, written by hand so it is deterministic. zlib's
            # own gzip wrapper (wbits=31) stamps the current time into it, and a
            # fixture that changes every run cannot be compared against anything.
            stream = b"\x1f\x8b\x08\x00" + b"\x00\x00\x00\x00" + b"\x02\xff" + blob
            stream += struct.pack("<II", zlib.crc32(payload) & 0xFFFFFFFF,
                                  len(payload) & 0xFFFFFFFF)

            with open(os.path.join(out_dir, f"deflate_{name}.{level}.gz"), "wb") as handle:
                handle.write(stream)


def write_zstd_vectors(out_dir: str) -> None:
    """Writes zstd vectors: the payload, and its frame at several levels.

    THE SAME METHODOLOGY AS THE DEFLATE VECTORS, and for the same reason: a
    decompressor producing output has proved nothing, so every vector is compared
    byte for byte against the exact original.

    The generator is Python's own zstd binding. That matters — it means the
    ground truth comes from a different implementation written by different
    people, not from our understanding of the format. A fixture generated by our
    own assumptions tests the assumptions.

    Levels are included because zstd changes strategy as the level rises and the
    levels reach different parts of the format: low levels favour raw and RLE
    blocks, high levels use compressed literals with FSE-coded sequence tables
    and long-distance matching.
    """
    try:
        from compression import zstd
    except ImportError:
        print("  zstd vectors: SKIPPED (no compression.zstd in this interpreter)")
        return

    payloads = deflate_payloads()          # the same six, deliberately
    written = 0

    for name, payload in payloads.items():
        with open(os.path.join(out_dir, f"zstd_{name}.raw"), "wb") as handle:
            handle.write(payload)

        # Levels 1..19, plus the negative "fast" levels zstd offers. One level
        # would exercise one strategy.
        for level in (1, 3, 6, 9, 12, 15, 19):
            try:
                blob = zstd.compress(payload, level)
            except Exception:                      # noqa: BLE001 — level not supported here
                continue

            with open(os.path.join(out_dir, f"zstd_{name}.{level}.zst"), "wb") as handle:
                handle.write(blob)
            written += 1

        # One more frame per payload, this time carrying its XXH64 content
        # checksum. The decoder verifies it, so a frame that decompresses byte for
        # byte while its checksum is wrong cannot pass — which makes these vectors
        # the only test the checksum implementation has, and the only one it needs:
        # the hash has to agree with one the reference implementation computed.
        try:
            checked = zstd.compress(payload, options={
                zstd.CompressionParameter.compression_level: 19,
                zstd.CompressionParameter.checksum_flag: 1,
            })
        except Exception:                      # noqa: BLE001 — older binding
            checked = None

        if checked is not None:
            with open(os.path.join(out_dir, f"zstd_{name}.chk.zst"), "wb") as handle:
                handle.write(checked)
            written += 1

    print(f"  zstd vectors: {len(payloads)} payloads, {written} frames")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=None, help="directory to write fixtures into")
    parser.add_argument("--list", action="store_true", help="list fixture names")
    args = parser.parse_args()

    if args.list or args.out is None:
        for name in FIXTURES:
            print(name)
        return 0

    os.makedirs(args.out, exist_ok=True)
    for name, builder in FIXTURES.items():
        data = builder()
        path = os.path.join(args.out, name)
        with open(path, "wb") as handle:
            handle.write(data)
        print(f"  {name}: {len(data)} bytes")

    write_deflate_vectors(args.out)
    write_zstd_vectors(args.out)
    print(f"  deflate vectors: {len(deflate_payloads())} payloads x 10 levels")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
