// SPDX-License-Identifier: MIT
// AfriyieOS — binary format detection self test
//
// WHY THIS IS IN THE KERNEL AND NOT IN tests/host
//
// Everything in tools/ is tested from Python because it IS Python. This module
// is kernel C, and the alternative — reimplementing the detector in Python to
// test it — would mean testing the reimplementation. That is the classic way a
// test suite ends up agreeing with a bug: two implementations written from the
// same misunderstanding, both correct and both wrong.
//
// Every case is a synthetic header built in a local array. No disk, no image, no
// firmware, no allocation. It runs in microseconds during boot, which is cheap
// enough that there is no reason not to.
//
// THE ASSERTIONS ARE POSITIVE. A detector written badly returns AF_BINFMT_UNKNOWN
// for everything and passes any test that only checks "it did not crash". So
// each case names the format AND the personality it must produce, and a mismatch
// reports what was expected, what came back, and the detector's own reason
// string — the field that makes a wrong answer debuggable without a rebuild.

#include "afriyie/binfmt.h"
#include "afriyie/kstring.h"
#include "afriyie/log.h"

static af_u32 s_checks;
static af_u32 s_failures;

// -----------------------------------------------------------------------------
// Assertions
// -----------------------------------------------------------------------------
static void expect(bool condition, const char *what,
                   const af_binfmt_info_t *got, af_binfmt_t want)
{
    s_checks++;

    if (condition) {
        return;
    }

    s_failures++;
    af_error("binfmt", "%s: expected %s, got %s (%s)",
             what, af_binfmt_name(want), af_binfmt_name(got->format),
             got->reason != NULL ? got->reason : "no reason given");
}

static void expect_personality(bool condition, const char *what,
                               const af_binfmt_info_t *got,
                               af_personality_t want)
{
    s_checks++;

    if (condition) {
        return;
    }

    s_failures++;
    af_error("binfmt", "%s: expected personality %s, got %s (%s)",
             what, af_personality_name(want),
             af_personality_name(got->personality), got->reason);
}

// -----------------------------------------------------------------------------
// Builds a ZIP local file header naming one entry, and classifies it.
//
// The entry NAME is the whole point: APK, AAB, JAR, Flatpak and a plain archive
// all begin with the same four bytes, and the name is what separates them.
// -----------------------------------------------------------------------------
static void check_zip_named(const char *name, af_binfmt_t want,
                            af_personality_t want_who, const char *what)
{
    af_u8 buf[160];
    af_memset(buf, 0, sizeof(buf));

    buf[0] = 'P';
    buf[1] = 'K';
    buf[2] = 0x03;
    buf[3] = 0x04;

    const af_size name_len = af_strlen(name);
    if (name_len > 120) {
        s_failures++;
        af_error("binfmt", "%s: test entry name too long", what);
        return;
    }

    // Local file header: the name length lives at offset 26, little-endian.
    buf[26] = (af_u8)(name_len & 0xFFu);
    buf[27] = (af_u8)((name_len >> 8) & 0xFFu);
    af_memcpy(buf + 30, name, name_len);

    af_binfmt_info_t info;
    af_binfmt_detect(buf, 30 + name_len, &info);

    s_checks++;
    if (info.format != want || info.personality != want_who) {
        s_failures++;
        af_error("binfmt", "%s: expected %s/%s, got %s/%s (%s)",
                 what, af_binfmt_name(want), af_personality_name(want_who),
                 af_binfmt_name(info.format),
                 af_personality_name(info.personality), info.reason);
    }
}

// -----------------------------------------------------------------------------
// The test
// -----------------------------------------------------------------------------
void af_binfmt_selftest(void)
{
    s_checks = 0;
    s_failures = 0;

    af_binfmt_info_t info;

    // =========================================================================
    // ELF
    // =========================================================================
    af_u8 elf[64];
    af_memset(elf, 0, sizeof(elf));
    elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    elf[4] = 2;      // ELFCLASS64
    elf[5] = 1;      // ELFDATA2LSB
    elf[16] = 2;     // e_type = ET_EXEC

    af_binfmt_detect(elf, sizeof(elf), &info);
    expect(info.format == AF_BINFMT_ELF, "ELF64 executable", &info, AF_BINFMT_ELF);
    expect((info.flags & AF_BINFMT_F_64BIT) != 0,
           "ELF64 sets the 64-bit flag", &info, AF_BINFMT_ELF);
    expect(af_binfmt_can_run(&info), "a well-formed ELF64 can be run", &info,
           AF_BINFMT_ELF);

    // The PERSONALITY is deliberately not asserted here, and that is the most
    // important decision in this file. An AfriyieOS binary and a Linux binary
    // are byte-identical at this level — same class, same type, same machine.
    // What separates them is PT_INTERP, which is past the prefix. Asserting a
    // personality here would be asserting a guess, and a test that encodes a
    // guess is worse than no test. What must be true is that the detector SAYS
    // it cannot tell.
    expect((info.flags & AF_BINFMT_F_AMBIGUOUS) != 0,
           "ELF reports that its personality needs the loader", &info,
           AF_BINFMT_ELF);

    // 32-bit big-endian, ET_REL: a relocatable object, which is not runnable.
    af_memset(elf, 0, sizeof(elf));
    elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    elf[4] = 1; elf[5] = 2; elf[16] = 1;

    af_binfmt_detect(elf, sizeof(elf), &info);
    expect(info.format == AF_BINFMT_ELF, "ELF32 relocatable", &info, AF_BINFMT_ELF);
    expect(!af_binfmt_can_run(&info),
           "a relocatable object cannot be run", &info, AF_BINFMT_ELF);
    expect(af_binfmt_is_executable(info.format),
           "but ELF is still an executable FORMAT", &info, AF_BINFMT_ELF);

    // A four-byte ELF prefix must be refused, not accepted on its magic alone.
    af_binfmt_detect(elf, 4, &info);
    expect((info.flags & AF_BINFMT_F_TRUNCATED) != 0,
           "a 4-byte ELF prefix is truncated, not valid", &info, AF_BINFMT_ELF);

    // =========================================================================
    // PE
    // =========================================================================
    af_u8 pe[256];
    af_memset(pe, 0, sizeof(pe));
    pe[0] = 'M'; pe[1] = 'Z';
    pe[0x3C] = 0x80;                                // e_lfanew = 0x80
    pe[0x80] = 'P'; pe[0x81] = 'E';
    pe[0x84] = 0x64; pe[0x85] = 0x86;               // machine = 0x8664 (x86-64)
    pe[0x80 + 24] = 0x0B; pe[0x80 + 25] = 0x02;     // magic = 0x20B (PE32+)

    af_binfmt_detect(pe, sizeof(pe), &info);
    expect(info.format == AF_BINFMT_PE, "PE32+ for x86-64", &info, AF_BINFMT_PE);
    expect_personality(info.personality == AF_PERSONALITY_WINDOWS,
                       "PE needs the Windows personality", &info,
                       AF_PERSONALITY_WINDOWS);
    expect((info.flags & AF_BINFMT_F_64BIT) != 0,
           "PE32+ sets the 64-bit flag", &info, AF_BINFMT_PE);
    expect(af_binfmt_can_run(&info), "a well-formed PE can be run", &info,
           AF_BINFMT_PE);

    // 'MZ' with no PE signature. This is the case that makes magic-number-only
    // detection wrong: DOS executables, OS/2 binaries and self-extracting
    // archives all begin with the same two bytes.
    af_u8 dos[64];
    af_memset(dos, 0, sizeof(dos));
    dos[0] = 'M'; dos[1] = 'Z';

    af_binfmt_detect(dos, sizeof(dos), &info);
    expect(info.format == AF_BINFMT_UNKNOWN,
           "MZ with no PE signature is not a PE", &info, AF_BINFMT_UNKNOWN);

    // A PE whose header lies past the supplied prefix is reported as truncated
    // and ambiguous rather than guessed at. A large DOS stub is legitimate.
    af_memset(pe, 0, sizeof(pe));
    pe[0] = 'M'; pe[1] = 'Z';
    pe[0x3C] = 0xF0; pe[0x3D] = 0xFF; pe[0x3E] = 0x00; pe[0x3F] = 0x00;

    af_binfmt_detect(pe, 128, &info);
    expect((info.flags & AF_BINFMT_F_TRUNCATED) != 0,
           "a PE header past the prefix is reported as truncated", &info,
           AF_BINFMT_PE);

    // =========================================================================
    // Mach-O
    // =========================================================================
    af_u8 macho[32];
    af_memset(macho, 0, sizeof(macho));
    macho[0] = 0xCF; macho[1] = 0xFA; macho[2] = 0xED; macho[3] = 0xFE;

    af_binfmt_detect(macho, sizeof(macho), &info);
    expect(info.format == AF_BINFMT_MACHO, "Mach-O 64-bit", &info, AF_BINFMT_MACHO);
    expect_personality(info.personality == AF_PERSONALITY_DARWIN,
                       "Mach-O names the Darwin personality", &info,
                       AF_PERSONALITY_DARWIN);

    // Fat binaries store their architecture table BIG-endian while thin headers
    // are host-endian. Reading one in the wrong byte order is the mistake this
    // case exists to catch, and it produces a plausible wrong number rather than
    // an error.
    af_memset(macho, 0, sizeof(macho));
    macho[0] = 0xCA; macho[1] = 0xFE; macho[2] = 0xBA; macho[3] = 0xBE;
    macho[7] = 2;                                   // two architectures

    af_binfmt_detect(macho, sizeof(macho), &info);
    expect(info.format == AF_BINFMT_MACHO, "Mach-O fat binary", &info,
           AF_BINFMT_MACHO);

    // =========================================================================
    // DEX
    // =========================================================================
    af_u8 dex[32];
    af_memset(dex, 0, sizeof(dex));
    af_memcpy(dex, "dex\n035", 7);

    af_binfmt_detect(dex, sizeof(dex), &info);
    expect(info.format == AF_BINFMT_DEX, "DEX", &info, AF_BINFMT_DEX);
    expect_personality(info.personality == AF_PERSONALITY_ANDROID,
                       "DEX needs the Android personality", &info,
                       AF_PERSONALITY_ANDROID);

    // =========================================================================
    // ZIP, and the formats that are secretly a ZIP
    // =========================================================================
    check_zip_named("AndroidManifest.xml", AF_BINFMT_APK_ANDROID,
                    AF_PERSONALITY_ANDROID, "APK identified by its manifest");
    check_zip_named("classes.dex", AF_BINFMT_APK_ANDROID,
                    AF_PERSONALITY_ANDROID, "APK identified by its dex");
    check_zip_named("META-INF/MANIFEST.MF", AF_BINFMT_JAR,
                    AF_PERSONALITY_NONE, "JAR identified by META-INF");
    check_zip_named("ordinary.txt", AF_BINFMT_ZIP,
                    AF_PERSONALITY_NONE, "plain ZIP");

    // THE ORDERING CASE, and the reason the checks are ordered the way they are.
    //
    // An Android App Bundle ALSO contains an AndroidManifest.xml, but under
    // base/manifest/ and in protobuf form. If the APK test ran first, every AAB
    // would be reported as an APK — and then fail to install, because an AAB has
    // no classes.dex at its root and is not installable at all. It is a
    // publishing format, not an application.
    check_zip_named("base/manifest/AndroidManifest.xml", AF_BINFMT_AAB,
                    AF_PERSONALITY_ANDROID, "AAB is not mistaken for an APK");

    // =========================================================================
    // Packages
    // =========================================================================
    af_u8 deb[32];
    af_memset(deb, 0, sizeof(deb));
    af_memcpy(deb, "!<arch>\n", 8);
    af_memcpy(deb + 8, "debian-binary", 13);

    af_binfmt_detect(deb, sizeof(deb), &info);
    expect(info.format == AF_BINFMT_DEB, "Debian package", &info, AF_BINFMT_DEB);
    expect_personality(info.personality == AF_PERSONALITY_PACKAGE,
                       "a package goes to the package manager, not a loader",
                       &info, AF_PERSONALITY_PACKAGE);
    expect(!af_binfmt_can_run(&info),
           "a package is not directly runnable", &info, AF_BINFMT_DEB);

    // A bare ar archive is not a .deb. The first member has to say so — and an
    // ar archive can hold anything, so the magic alone means nothing.
    af_u8 ar[32];
    af_memset(ar, 0, sizeof(ar));
    af_memcpy(ar, "!<arch>\n", 8);
    af_memcpy(ar + 8, "somefile.o", 10);

    af_binfmt_detect(ar, sizeof(ar), &info);
    expect(info.format == AF_BINFMT_UNKNOWN,
           "a plain ar archive is not a deb", &info, AF_BINFMT_UNKNOWN);

    af_u8 rpm[16];
    af_memset(rpm, 0, sizeof(rpm));
    rpm[0] = 0xED; rpm[1] = 0xAB; rpm[2] = 0xEE; rpm[3] = 0xDB;

    af_binfmt_detect(rpm, sizeof(rpm), &info);
    expect(info.format == AF_BINFMT_RPM, "RPM package", &info, AF_BINFMT_RPM);

    // =========================================================================
    // Scripts and WebAssembly
    // =========================================================================
    af_u8 script[32];
    af_memset(script, 0, sizeof(script));
    af_memcpy(script, "#!/bin/sh\n", 10);

    af_binfmt_detect(script, sizeof(script), &info);
    expect(info.format == AF_BINFMT_SCRIPT, "shebang script", &info,
           AF_BINFMT_SCRIPT);
    expect_personality(info.personality == AF_PERSONALITY_SCRIPT,
                       "a script names its own interpreter", &info,
                       AF_PERSONALITY_SCRIPT);

    af_u8 wasm[16];
    af_memset(wasm, 0, sizeof(wasm));
    af_memcpy(wasm, "\0asm", 4);

    af_binfmt_detect(wasm, sizeof(wasm), &info);
    expect(info.format == AF_BINFMT_WASM, "WebAssembly module", &info,
           AF_BINFMT_WASM);

    // =========================================================================
    // Trailer detection
    //
    // A DMG's signature is the 'koly' block in its LAST 512 bytes. Its beginning
    // is filesystem data indistinguishable from any other disk image, which is
    // precisely why prefix-only detection cannot find it — and why the API has
    // two entry points instead of one.
    //
    // 'koly' is the FIRST field of that 512-byte trailer, not the last. The
    // first version of this test put it at dmg+508, four bytes before the block
    // it was supposed to be inside, and the detector correctly reported nothing:
    // the test was wrong and the code was right, which is the outcome a positive
    // assertion is supposed to produce.
    // =========================================================================
    af_u8 dmg[1024];
    af_memset(dmg, 0, sizeof(dmg));
    af_memcpy(dmg + 512, "koly", 4);

    af_binfmt_detect(dmg, 512, &info);
    expect(info.format == AF_BINFMT_UNKNOWN,
           "a DMG's prefix is unidentifiable", &info, AF_BINFMT_UNKNOWN);

    af_binfmt_detect_tail(dmg + 512, 512, &info);
    expect(info.format == AF_BINFMT_DMG, "DMG by its trailer", &info,
           AF_BINFMT_DMG);
    expect((info.flags & AF_BINFMT_F_CONTAINER) != 0,
           "DMG is marked as a container", &info, AF_BINFMT_DMG);
    expect_personality(info.personality == AF_PERSONALITY_DARWIN,
                       "DMG names the Darwin personality", &info,
                       AF_PERSONALITY_DARWIN);
    expect(!af_binfmt_can_run(&info),
           "a DMG is an image, not something to run", &info, AF_BINFMT_DMG);

    // =========================================================================
    // Negative controls
    //
    // A detector that recognises everything is as broken as one that recognises
    // nothing, and BOTH pass a suite made only of positive cases. These are the
    // cases that fail if the detector starts saying yes too easily.
    // =========================================================================
    af_u8 junk[64];
    af_memset(junk, 0xA5, sizeof(junk));

    af_binfmt_detect(junk, sizeof(junk), &info);
    expect(info.format == AF_BINFMT_UNKNOWN, "random bytes are unknown", &info,
           AF_BINFMT_UNKNOWN);
    expect(info.reason != NULL && info.reason[0] != '\0',
           "even an unknown file gets an explanation", &info, AF_BINFMT_UNKNOWN);

    af_binfmt_detect(NULL, 0, &info);
    expect(info.format == AF_BINFMT_UNKNOWN, "NULL input is handled", &info,
           AF_BINFMT_UNKNOWN);

    // Every name lookup must answer for every value in the enum, including the
    // one a switch forgets. A "?" reaching a user-facing message is a bug that
    // is invisible until someone sees it.
    for (af_u32 f = 0; f <= (af_u32)AF_BINFMT_XZ; f++) {
        const char *name = af_binfmt_name((af_binfmt_t)f);
        s_checks++;
        if (name == NULL || name[0] == '\0' ||
            (name[0] == '?' && name[1] == '\0')) {
            s_failures++;
            af_error("binfmt", "format %u has no name", f);
        }
    }
    for (af_u32 p = 0; p <= (af_u32)AF_PERSONALITY_PACKAGE; p++) {
        const char *name = af_personality_name((af_personality_t)p);
        s_checks++;
        if (name == NULL || name[0] == '\0' ||
            (name[0] == '?' && name[1] == '\0')) {
            s_failures++;
            af_error("binfmt", "personality %u has no name", p);
        }
    }

    // =========================================================================
    // Report
    // =========================================================================
    if (s_failures != 0) {
        af_error("binfmt", "%u of %u format detection checks FAILED",
                 s_failures, s_checks);
        af_marker("AF_TEST_FAIL");
        return;
    }

    af_info("binfmt", "%u format detection checks passed", s_checks);
    af_marker("AF_BINFMT_OK");
}
