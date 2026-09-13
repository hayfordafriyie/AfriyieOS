// SPDX-License-Identifier: MIT
// AfriyieOS — binary format identification
//
// Implementation notes for the decisions that are not obvious from the header.
//
// NOTHING HERE ALLOCATES, LOGS OR LOOKS AT THE DISK. Every check is a comparison
// against bytes the caller already has. That is what makes the whole module
// testable on the host in milliseconds, and what lets it run in a context where
// nothing else works yet.
//
// BYTE ORDER IS HANDLED BY HAND, not by casting a struct over the buffer. The
// formats disagree with each other — PE is little-endian, Mach-O fat headers are
// big-endian, ELF declares its own — and a struct cast would be both unaligned
// and wrong for whichever ones differ. Reading fields explicitly costs a few
// lines and cannot be subtly wrong on a different architecture.

#include "afriyie/binfmt.h"
#include "afriyie/kstring.h"

// -----------------------------------------------------------------------------
// Little/big-endian readers
//
// Separate functions rather than a `bool big_endian` parameter, because a
// parameter means every call site has to be read carefully to know which it is,
// and getting it wrong produces a plausible number rather than a crash.
// -----------------------------------------------------------------------------
static af_u16 rd16le(const af_u8 *p)
{
    return (af_u16)((af_u16)p[0] | ((af_u16)p[1] << 8));
}

static af_u32 rd32le(const af_u8 *p)
{
    return (af_u32)p[0] | ((af_u32)p[1] << 8) |
           ((af_u32)p[2] << 16) | ((af_u32)p[3] << 24);
}

static af_u32 rd32be(const af_u8 *p)
{
    return ((af_u32)p[0] << 24) | ((af_u32)p[1] << 16) |
           ((af_u32)p[2] << 8) | (af_u32)p[3];
}

// Reads 8 bytes big-endian. Used only for the Mach-O fat magic, which is a
// 4-byte magic in an 8-byte field.
static af_u32 rd32be_at(const af_u8 *p, af_size i)
{
    return rd32be(p + i);
}

// Bounds check. `data` is not read — the parameter is kept so every call site
// reads the same way as match() below, where the buffer IS used.
static bool has(AF_UNUSED_PARAM const af_u8 *data, af_size len, af_size at,
                af_size n)
{
    AF_UNUSED(data);
    return at <= len && (len - at) >= n;
}

static bool match(const af_u8 *data, af_size len, af_size at,
                  const char *sig, af_size n)
{
    if (!has(data, len, at, n)) {
        return false;
    }
    for (af_size i = 0; i < n; i++) {
        if (data[at + i] != (af_u8)sig[i]) {
            return false;
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// Small helpers for filling the result
// -----------------------------------------------------------------------------
static void set(af_binfmt_info_t *out, af_binfmt_t fmt, af_personality_t who,
                af_u32 flags, af_u32 at, const char *reason)
{
    out->format      = fmt;
    out->personality = who;
    out->flags       = flags;
    out->outer_format = AF_BINFMT_UNKNOWN;
    out->decided_at  = at;
    out->reason      = reason;
}

static void unknown(af_binfmt_info_t *out, af_u32 at, const char *reason)
{
    set(out, AF_BINFMT_UNKNOWN, AF_PERSONALITY_NONE, 0, at, reason);
}

// -----------------------------------------------------------------------------
// ZIP
//
// A ZIP is identified by its first local file header. That is necessary but not
// sufficient to say anything useful: APK, AAB, JAR, Flatpak and a plain archive
// all begin this way. The local header carries the entry's NAME, which is what
// distinguishes them — looking for `AndroidManifest.xml`, `classes.dex`,
// `META-INF/MANIFEST.MF`, `base/manifest/AndroidManifest.xml` and so on.
//
// Only the FIRST entry name is inspected. Scanning every entry would need the
// whole central directory, which is at the END of the file and behind the API's
// prefix-only boundary. It is also unnecessary in practice: the formats that
// matter put their marker first, and a ZIP that buries it is one we report as a
// plain ZIP rather than guessing.
// -----------------------------------------------------------------------------
static bool zip_first_name(const af_u8 *data, af_size len, af_size hdr_at,
                           char *name, af_size name_size)
{
    // Local file header: 30 bytes, then the name, then extra.
    if (!has(data, len, hdr_at, 30)) {
        return false;
    }
    if (rd32le(data + hdr_at) != 0x04034b50u) {
        return false;
    }

    const af_u16 name_len = rd16le(data + hdr_at + 26);
    if (!has(data, len, hdr_at + 30, name_len) || name_len >= name_size) {
        return false;
    }

    for (af_u16 i = 0; i < name_len; i++) {
        name[i] = (char)data[hdr_at + 30 + i];
    }
    name[name_len] = '\0';
    return true;
}

static bool name_startswith(const char *name, const char *prefix)
{
    af_size i = 0;
    while (prefix[i] != '\0') {
        if (name[i] != prefix[i]) {
            return false;
        }
        i++;
    }
    return true;
}

static void classify_zip(const af_u8 *data, af_size len, af_binfmt_info_t *out)
{
    char name[128];

    if (!zip_first_name(data, len, 0, name, sizeof(name))) {
        // A ZIP with no readable first name is still a ZIP. Saying so is more
        // useful than saying "unknown", because the caller can look further.
        set(out, AF_BINFMT_ZIP, AF_PERSONALITY_NONE, AF_BINFMT_F_NOEXEC, 0,
            "PK local file header, but the first entry name is unreadable");
        return;
    }

    // Order matters. An AAB also contains an AndroidManifest.xml, but inside
    // base/manifest/ and in protobuf form, so the base/ test has to come first
    // or every AAB would be reported as an APK — and be installed as one, and
    // fail, because an AAB has no classes.dex at its root.
    if (name_startswith(name, "base/manifest/")) {
        set(out, AF_BINFMT_AAB, AF_PERSONALITY_ANDROID,
            AF_BINFMT_F_CONTAINER | AF_BINFMT_F_NOEXEC, 30,
            "ZIP whose first entry is base/manifest/ — an Android App Bundle, "
            "which is a publishing format and must be split into APKs before "
            "anything can install it");
        return;
    }

    if (name_startswith(name, "AndroidManifest.xml") ||
        name_startswith(name, "classes.dex")) {
        set(out, AF_BINFMT_APK_ANDROID, AF_PERSONALITY_ANDROID,
            AF_BINFMT_F_CONTAINER, 30,
            "ZIP with an Android manifest or DEX at its root — an Android APK");
        return;
    }

    if (name_startswith(name, "META-INF/")) {
        set(out, AF_BINFMT_JAR, AF_PERSONALITY_NONE,
            AF_BINFMT_F_CONTAINER | AF_BINFMT_F_NOEXEC, 30,
            "ZIP with META-INF first — a JAR or a Java archive; no JVM "
            "personality exists yet");
        return;
    }

    if (name_startswith(name, "metadata") ||
        name_startswith(name, "export")) {
        set(out, AF_BINFMT_FLATPAK, AF_PERSONALITY_PACKAGE,
            AF_BINFMT_F_CONTAINER | AF_BINFMT_F_NOEXEC, 30,
            "ZIP with OSTree metadata — a Flatpak bundle");
        return;
    }

    set(out, AF_BINFMT_ZIP, AF_PERSONALITY_NONE, AF_BINFMT_F_NOEXEC, 30,
        "ZIP archive; nothing about it is executable");
}

// -----------------------------------------------------------------------------
// ELF
//
// Three different things arrive as ELF and they need three different
// personalities:
//
//   * an AfriyieOS binary — our own ABI
//   * a Linux binary    — Linux syscall ABI
//   * an Android .so    — Linux-ish, but bionic, and reached through JNI
//
// They are told apart by their interpreter (PT_INTERP) and by note sections.
// This detector does not read the program headers — that is the loader's job and
// needs the whole file — so it reports the format and marks it ambiguous, which
// is the honest answer at this level. Refining it needs the ELF loader, and
// guessing here would be worse than admitting the limit.
// -----------------------------------------------------------------------------
static void classify_elf(const af_u8 *data, af_size len, af_binfmt_info_t *out)
{
    if (!has(data, len, 0, 20)) {
        unknown(out, 0, "ELF magic but the header is truncated");
        out->flags |= AF_BINFMT_F_TRUNCATED;
        return;
    }

    const af_u8 ei_class = data[4];   // 1 = 32-bit, 2 = 64-bit
    const af_u8 ei_data  = data[5];   // 1 = LE, 2 = BE
    const af_u16 e_type  = rd16le(data + 16);

    if (ei_class != 1 && ei_class != 2) {
        unknown(out, 4, "ELF magic with an unknown class byte");
        return;
    }
    if (ei_data != 1 && ei_data != 2) {
        unknown(out, 5, "ELF magic with an unknown data-encoding byte");
        return;
    }

    af_u32 flags = 0;
    if (ei_class == 2) {
        flags |= AF_BINFMT_F_64BIT;
    }

    // ET_DYN (3) is a shared object OR a PIE executable — the same value, told
    // apart by whether the file has an entry point and an interpreter. Say so
    // rather than picking one.
    const char *reason;
    if (e_type == 2) {
        reason = "ELF executable";
    } else if (e_type == 3) {
        reason = "ELF shared object or PIE executable";
        flags |= AF_BINFMT_F_AMBIGUOUS;
    } else if (e_type == 1) {
        reason = "ELF relocatable object — not executable, must be linked first";
        flags |= AF_BINFMT_F_NOEXEC;
    } else {
        reason = "ELF with an unrecognised e_type";
        flags |= AF_BINFMT_F_AMBIGUOUS;
    }

    // WHICH PERSONALITY IS NOT DECIDABLE FROM THE PREFIX, and pretending
    // otherwise would be the single most damaging lie this module could tell.
    // An AfriyieOS binary and a Linux binary are both `ELF 64-bit LSB
    // executable`. What separates them is the interpreter recorded in PT_INTERP
    // (a path under /lib/ld-… for Linux, /system/afr/… for us) and that lives
    // past the prefix. Until the loader resolves it, the honest answer is
    // "ELF, personality unknown", and the loader refines it.
    set(out, AF_BINFMT_ELF, AF_PERSONALITY_NONE, flags, 0, reason);
    out->flags |= AF_BINFMT_F_AMBIGUOUS;
}

// -----------------------------------------------------------------------------
// PE
//
// `MZ` alone means nothing useful: DOS executables, OS/2 binaries, some
// self-extracting archives and every modern Windows image start with it. The
// real signature is `PE\0\0` at the offset given by e_lfanew (0x3C).
//
// A .NET assembly is also a PE, and is distinguished by the CLI data directory
// in the optional header. It matters because .NET is a different personality
// problem — the runtime is a managed VM, not a Win32 API translator.
// -----------------------------------------------------------------------------
static void classify_pe(const af_u8 *data, af_size len, af_binfmt_info_t *out)
{
    if (!has(data, len, 0, 0x40)) {
        unknown(out, 0, "MZ header but the DOS stub is truncated");
        out->flags |= AF_BINFMT_F_TRUNCATED;
        return;
    }

    const af_u32 lfanew = rd32le(data + 0x3C);
    if (!has(data, len, lfanew, 24)) {
        // The PE header is past the prefix. This is a real limit of prefix-only
        // detection and it is reported rather than guessed: an image with an
        // unusually large DOS stub is legitimate and not rare.
        set(out, AF_BINFMT_PE, AF_PERSONALITY_WINDOWS,
            AF_BINFMT_F_TRUNCATED | AF_BINFMT_F_AMBIGUOUS, 0x3C,
            "MZ header; the PE header is past the supplied prefix "
            "(e_lfanew points beyond what was read)");
        return;
    }

    if (!match(data, len, lfanew, "PE\0\0", 4)) {
        unknown(out, lfanew,
                "MZ header with no PE signature — a DOS or OS/2 executable, or "
                "a self-extracting archive");
        return;
    }

    const af_u16 machine = rd16le(data + lfanew + 4);
    const af_u16 magic   = rd16le(data + lfanew + 24);

    af_u32 flags = 0;
    if (magic == 0x20B) {
        flags |= AF_BINFMT_F_64BIT;         // PE32+
    } else if (magic != 0x10B) {
        unknown(out, lfanew + 24, "PE with an unknown optional-header magic");
        return;
    }

    // 0x8664 = x86_64, 0xAA64 = ARM64, 0x014C = i386, 0x01C4 = ARMNT.
    const char *machine_name;
    switch (machine) {
    case 0x8664: machine_name = "x86-64"; break;
    case 0xAA64: machine_name = "ARM64";  break;
    case 0x014C: machine_name = "i386";   break;
    case 0x01C4: machine_name = "ARM";    break;
    default:     machine_name = "an unknown machine"; break;
    }

    set(out, AF_BINFMT_PE, AF_PERSONALITY_WINDOWS, flags, lfanew, machine_name);

    // The caller wants a sentence, not a machine name in a field named "reason".
    // The switch above sets the machine; the reason is composed from context.
    out->reason = "PE image — the Windows personality";
}

// -----------------------------------------------------------------------------
// Mach-O
//
// Four magics, and the fat ones are byte-swapped relative to the thin ones in a
// way that has bitten every implementation of this. A fat header is
// BIG-endian; a thin header's magic is whatever the file says.
//
// Detected, and no personality can run it — see universal-compat.md §1. Reporting
// the format is still correct and still useful: a user asking "why won't this
// .dmg open" deserves "it is a Mach-O for arm64, and there is no macOS
// personality" rather than silence.
// -----------------------------------------------------------------------------
static void classify_macho(const af_u8 *data, af_size len, af_binfmt_info_t *out)
{
    if (!has(data, len, 0, 8)) {
        unknown(out, 0, "Mach-O magic but the header is truncated");
        out->flags |= AF_BINFMT_F_TRUNCATED;
        return;
    }

    const af_u32 magic_be = rd32be(data);

    // Fat / universal binaries. 0xCAFEBABE reads the same either way, which is
    // exactly why the following byte order matters: the architecture table is
    // big-endian, and the two fat magics differ only in their low byte.
    if (magic_be == 0xCAFEBABEu || magic_be == 0xCAFEBABFu) {
        const af_u32 nfat = rd32be_at(data, 4);
        const bool is64 = (magic_be == 0xCAFEBABFu);

        af_u32 flags = AF_BINFMT_F_AMBIGUOUS;
        if (is64) {
            flags |= AF_BINFMT_F_64BIT;
        }

        // The architecture count is a claim by the file about itself. A
        // zero or absurd count means the header is not what it says, and saying
        // so is more useful than reporting a clean detection of a broken file.
        if (nfat == 0 || nfat > 64) {
            flags |= AF_BINFMT_F_TRUNCATED;
        }

        set(out, AF_BINFMT_MACHO, AF_PERSONALITY_DARWIN, flags, 4,
            is64 ? "Mach-O 64-bit universal binary"
                 : "Mach-O universal (fat) binary");
        return;
    }

    const af_u32 magic_le = rd32le(data);

    // Thin, little-endian host (the modern case).
    if (magic_le == 0xFEEDFACFu || magic_le == 0xFEEDFACEu) {
        af_u32 flags = (magic_le == 0xFEEDFACFu) ? AF_BINFMT_F_64BIT : 0;
        set(out, AF_BINFMT_MACHO, AF_PERSONALITY_DARWIN, flags, 0,
            (magic_le == 0xFEEDFACFu) ? "Mach-O 64-bit executable"
                                      : "Mach-O 32-bit executable");
        return;
    }

    // Thin, big-endian host — PowerPC and pre-Intel Macs, kept because getting
    // this branch wrong by omission would misreport old binaries as unknown.
    if (magic_be == 0xFEEDFACFu || magic_be == 0xFEEDFACEu) {
        af_u32 flags = (magic_be == 0xFEEDFACFu) ? AF_BINFMT_F_64BIT : 0;
        set(out, AF_BINFMT_MACHO, AF_PERSONALITY_DARWIN, flags, 0,
            "Mach-O big-endian (PowerPC) executable");
        return;
    }
}

// -----------------------------------------------------------------------------
// The main entry point
// -----------------------------------------------------------------------------
void af_binfmt_detect(const af_u8 *data, af_size len, af_binfmt_info_t *out)
{
    if (out == NULL) {
        return;
    }

    if (data == NULL || len == 0) {
        unknown(out, 0, "no data supplied");
        return;
    }

    // --- 1. Formats with a distinctive leading signature -------------------
    //
    // Checked before anything that unwraps, so a container is only unwrapped
    // when it really is one.

    // ELF: 0x7F 'E' 'L' 'F'
    if (match(data, len, 0, "\x7F" "ELF", 4)) {
        classify_elf(data, len, out);
        return;
    }

    // PE: 'MZ'. Deliberately narrow — 'ZM' is also a valid DOS signature and
    // means something different (a DOS device driver), so only 'MZ'.
    if (match(data, len, 0, "MZ", 2)) {
        classify_pe(data, len, out);
        return;
    }

    // Mach-O.
    if (has(data, len, 0, 4)) {
        const af_u32 be = rd32be(data);
        const af_u32 le = rd32le(data);
        if (be == 0xCAFEBABEu || be == 0xCAFEBABFu ||
            le == 0xFEEDFACFu || le == 0xFEEDFACEu ||
            be == 0xFEEDFACFu || be == 0xFEEDFACEu) {
            classify_macho(data, len, out);
            return;
        }
    }

    // DEX: "dex\n035\0" and other versions.
    if (match(data, len, 0, "dex\n", 4)) {
        set(out, AF_BINFMT_DEX, AF_PERSONALITY_ANDROID, 0, 0,
            "Dalvik executable");
        return;
    }

    // Zstandard — a compression frame, not something executable. Recognised so
    // that a .pkg.tar.zst is not reported as "unknown".
    if (has(data, len, 0, 4) && data[0] == 0x28 && data[1] == 0xB5 &&
        data[2] == 0x2F && data[3] == 0xFD) {
        set(out, AF_BINFMT_ZSTD, AF_PERSONALITY_NONE, AF_BINFMT_F_NOEXEC, 0,
            "Zstandard frame");
        return;
    }

    // RPM: 0xED 0xAB 0xEE 0xDB, then a version byte.
    if (has(data, len, 0, 4) && data[0] == 0xED && data[1] == 0xAB &&
        data[2] == 0xEE && data[3] == 0xDB) {
        set(out, AF_BINFMT_RPM, AF_PERSONALITY_PACKAGE, 0, 0,
            "RPM package");
        return;
    }

    // Debian: an ar archive whose first member is `debian-binary`. The ar magic
    // alone is not enough — an ar archive can hold anything.
    if (match(data, len, 0, "!<arch>\n", 8)) {
        if (match(data, len, 8, "debian-binary", 13)) {
            set(out, AF_BINFMT_DEB, AF_PERSONALITY_PACKAGE, 0, 8,
                "Debian package");
            return;
        }
        // Alpine's .apk is a gzipped tar, not an ar archive, so it is not here.
        unknown(out, 8, "ar archive, but not a Debian package");
        return;
    }

    // Squashfs: 'hsqs' little-endian, 'sqsh' big-endian.
    if (match(data, len, 0, "hsqs", 4) || match(data, len, 0, "sqsh", 4)) {
        set(out, AF_BINFMT_SQUASHFS, AF_PERSONALITY_PACKAGE, AF_BINFMT_F_NOEXEC,
            0, "Squashfs image — the payload of a Snap, AppImage or live system");
        return;
    }

    // gzip, xz, tar.
    if (has(data, len, 0, 2) && data[0] == 0x1F && data[1] == 0x8B) {
        set(out, AF_BINFMT_GZIP, AF_PERSONALITY_NONE, AF_BINFMT_F_NOEXEC, 0,
            "gzip stream");
        return;
    }
    if (match(data, len, 0, "\xFD" "7zXZ", 5)) {
        set(out, AF_BINFMT_XZ, AF_PERSONALITY_NONE, AF_BINFMT_F_NOEXEC, 0,
            "xz stream");
        return;
    }
    if (match(data, len, 0, "ustar", 5) || match(data, len, 257, "ustar", 5)) {
        set(out, AF_BINFMT_TAR, AF_PERSONALITY_NONE, AF_BINFMT_F_NOEXEC, 0,
            "tar archive");
        return;
    }

    // --- 2. Scripts ---------------------------------------------------------
    if (match(data, len, 0, "#!", 2)) {
        set(out, AF_BINFMT_SCRIPT, AF_PERSONALITY_SCRIPT, 0, 0,
            "script — the interpreter is named on the first line");
        return;
    }

    // --- 3. WebAssembly -----------------------------------------------------
    if (match(data, len, 0, "\0asm", 4)) {
        set(out, AF_BINFMT_WASM, AF_PERSONALITY_WASM, 0, 0,
            "WebAssembly module");
        return;
    }

    // --- 4. ZIP and everything that is secretly a ZIP -----------------------
    if (match(data, len, 0, "PK\x03\x04", 4)) {
        classify_zip(data, len, out);
        return;
    }

    // --- 5. MSI and other OLE compound files --------------------------------
    if (has(data, len, 0, 8) && data[0] == 0xD0 && data[1] == 0xCF &&
        data[2] == 0x11 && data[3] == 0xE0) {
        set(out, AF_BINFMT_MSI, AF_PERSONALITY_PACKAGE, AF_BINFMT_F_NOEXEC, 0,
            "OLE compound file — an MSI installer or an Office document");
        return;
    }

    // --- 6. Nothing matched -------------------------------------------------
    //
    // The AppImage case is worth naming here because it is the one that looks
    // like a failure and is not: an AppImage IS an ELF, so it is caught by the
    // ELF branch above and reported as ELF. The squashfs payload glued to its
    // end is found by af_binfmt_detect_tail. That split is why there are two
    // entry points.
    unknown(out, 0, "no known signature in the first bytes");
}

// -----------------------------------------------------------------------------
// Trailer-based detection
//
// DMG, AppImage and ZIP all keep their identifying structure at the END:
//
//   DMG      'koly' in the last 512 bytes — the whole point being that the
//            beginning is indistinguishable from any other disk image
//   AppImage a squashfs superblock, then a magic and an offset, in the last
//            64 KiB
//   ZIP      the end-of-central-directory record
//
// A prefix-only detector cannot see any of these, which is why this exists
// rather than being folded into the function above.
// -----------------------------------------------------------------------------
void af_binfmt_detect_tail(const af_u8 *tail, af_size len, af_binfmt_info_t *out)
{
    if (out == NULL) {
        return;
    }
    if (tail == NULL || len == 0) {
        unknown(out, 0, "no trailer supplied");
        return;
    }

    // --- DMG: a 512-byte 'koly' block at the very end ------------------------
    if (len >= 512) {
        const af_size at = len - 512;
        if (match(tail, len, at, "koly", 4)) {
            set(out, AF_BINFMT_DMG, AF_PERSONALITY_DARWIN,
                AF_BINFMT_F_CONTAINER | AF_BINFMT_F_NOEXEC, (af_u32)at,
                "UDIF disk image — a macOS .dmg, detected by its trailer "
                "because its beginning is filesystem data");
            return;
        }
    }

    // --- AppImage: magic near the end, after the squashfs payload ------------
    //
    // The AppImage type-2 trailer is at a fixed offset from the end: 8 bytes of
    // magic, then the squashfs offset and length. Scanning backwards for it
    // rather than computing the offset means a file truncated or padded by a
    // download is still recognised.
    for (af_size i = len; i >= 8; i--) {
        if (match(tail, len, i - 8, "AI\x02", 3) ||
            match(tail, len, i - 8, "AI\x01", 3)) {
            set(out, AF_BINFMT_APPIMAGE, AF_PERSONALITY_LINUX,
                AF_BINFMT_F_CONTAINER, (af_u32)(i - 8),
                "ELF with an AppImage trailer — the squashfs payload at the end "
                "is mounted and the ELF inside it is run");
            return;
        }
        if ((i & 0x3FFu) == 0 && i < 0x10000u) {
            // The magic is within the last 64 KiB; not finding it by then means
            // it is not there.
            break;
        }
    }

    // --- ZIP end-of-central-directory ---------------------------------------
    //
    // 0x06054b50, then a comment of up to 64 KiB may follow. Scanning back over
    // the comment is what makes an APK with a ZIP comment still identifiable.
    for (af_size i = len; i >= 22; i--) {
        if (rd32le(tail + (i - 22)) == 0x06054b50u) {
            set(out, AF_BINFMT_ZIP, AF_PERSONALITY_NONE, AF_BINFMT_F_NOEXEC,
                (af_u32)(i - 22), "ZIP end-of-central-directory record");
            return;
        }
        if (len - i > 65535u) {
            break;      // past the largest possible archive comment
        }
    }

    unknown(out, 0, "no known trailer signature");
}

// -----------------------------------------------------------------------------
// Names
// -----------------------------------------------------------------------------
const char *af_binfmt_name(af_binfmt_t format)
{
    switch (format) {
    case AF_BINFMT_UNKNOWN:      return "unknown";
    case AF_BINFMT_ELF:          return "ELF";
    case AF_BINFMT_PE:           return "PE";
    case AF_BINFMT_MACHO:        return "Mach-O";
    case AF_BINFMT_DEX:          return "DEX";
    case AF_BINFMT_WASM:         return "WebAssembly";
    case AF_BINFMT_SCRIPT:       return "script";
    case AF_BINFMT_DEB:          return "deb";
    case AF_BINFMT_RPM:          return "rpm";
    case AF_BINFMT_PACMAN:       return "pacman";
    case AF_BINFMT_APK_PKG:      return "Alpine apk";
    case AF_BINFMT_APK_ANDROID:  return "Android apk";
    case AF_BINFMT_AAB:          return "Android App Bundle";
    case AF_BINFMT_FLATPAK:      return "Flatpak";
    case AF_BINFMT_SNAP:         return "Snap";
    case AF_BINFMT_MSI:          return "MSI";
    case AF_BINFMT_DMG:          return "DMG";
    case AF_BINFMT_APPIMAGE:     return "AppImage";
    case AF_BINFMT_JAR:          return "JAR";
    case AF_BINFMT_ZIP:          return "ZIP";
    case AF_BINFMT_TAR:          return "tar";
    case AF_BINFMT_SQUASHFS:     return "Squashfs";
    case AF_BINFMT_ZSTD:         return "Zstandard";
    case AF_BINFMT_GZIP:         return "gzip";
    case AF_BINFMT_XZ:           return "xz";
    }
    return "?";
}

const char *af_personality_name(af_personality_t personality)
{
    switch (personality) {
    case AF_PERSONALITY_NONE:    return "none";
    case AF_PERSONALITY_NATIVE:  return "native";
    case AF_PERSONALITY_LINUX:   return "linux";
    case AF_PERSONALITY_WINDOWS: return "windows";
    case AF_PERSONALITY_ANDROID: return "android";
    case AF_PERSONALITY_DARWIN:  return "darwin";
    case AF_PERSONALITY_WASM:    return "wasm";
    case AF_PERSONALITY_SCRIPT:  return "script";
    case AF_PERSONALITY_PACKAGE: return "package";
    }
    return "?";
}

bool af_binfmt_is_executable(af_binfmt_t format)
{
    switch (format) {
    case AF_BINFMT_ELF:
    case AF_BINFMT_PE:
    case AF_BINFMT_MACHO:
    case AF_BINFMT_DEX:
    case AF_BINFMT_WASM:
    case AF_BINFMT_SCRIPT:
        return true;
    default:
        return false;
    }
}

bool af_binfmt_can_run(const af_binfmt_info_t *info)
{
    if (info == NULL) {
        return false;
    }

    // The format has to be one a loader can enter at all...
    if (!af_binfmt_is_executable(info->format)) {
        return false;
    }

    // ...and the file must not have said otherwise. AF_BINFMT_F_NOEXEC is set
    // for the cases that are the right format and the wrong thing: a relocatable
    // object, an AAB, a package.
    if ((info->flags & AF_BINFMT_F_NOEXEC) != 0) {
        return false;
    }

    // An identification that could not be completed is not an identification.
    // A PE whose header lies past the prefix might be perfectly good, and
    // returning "yes, runnable" on the strength of two magic bytes is how a
    // loader ends up handing a malformed image to the CPU.
    if ((info->flags & AF_BINFMT_F_TRUNCATED) != 0) {
        return false;
    }

    // Ambiguous means the format is known but the personality is not — an ELF
    // that could be ours or Linux's. It is runnable *in principle* once the
    // loader resolves it, so this is not disqualifying on its own.
    return true;
}
