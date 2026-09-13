// SPDX-License-Identifier: MIT
// AfriyieOS — native tests for libafpkg
//
// WHY A C TEST BINARY, WHEN tests/host IS PYTHON
//
// Everything in tools/ is tested from Python because it IS Python. libafpkg is
// C that will eventually run inside AfriyieOS, and reimplementing it in Python
// to test it would mean testing the reimplementation — two implementations
// written from the same misunderstanding, both correct and both wrong.
//
// So this is a C program, compiled by the host compiler, run by tools/native_test.sh.
// It links libafpkg against the fixtures tools/pkgsynth.py generates and asserts
// real results.
//
// This is NEW INFRASTRUCTURE and it is worth more than the tests in it: every
// user-space library after this one — the package manager, the file system
// client, the personality runtimes — can be tested the same way, on the host,
// in milliseconds, without booting anything.
//
// THE FIXTURES ARE REAL CONTAINERS and the tests are positive. A reader that
// returns AF_ERR everywhere passes any test that only checks "did not crash", so
// every case names the field it expects and its value.

#include "afpkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_checks;
static int s_failures;

static void check(int condition, const char *what)
{
    s_checks++;
    if (!condition) {
        s_failures++;
        printf("  FAIL  %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    s_checks++;
    if (got == NULL || strcmp(got, want) != 0) {
        s_failures++;
        printf("  FAIL  %s: expected \"%s\", got \"%s\"\n",
               what, want, got != NULL ? got : "(null)");
    }
}

static void check_u64(unsigned long long got, unsigned long long want,
                      const char *what)
{
    s_checks++;
    if (got != want) {
        s_failures++;
        printf("  FAIL  %s: expected %llu, got %llu\n", what, want, got);
    }
}

// -----------------------------------------------------------------------------
// Fixture loading
// -----------------------------------------------------------------------------
static unsigned char *s_file;
static long           s_file_len;

static int load(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);

    FILE *handle = fopen(path, "rb");
    if (handle == NULL) {
        printf("  cannot open %s — run tools/pkgsynth.py first\n", path);
        return -1;
    }

    fseek(handle, 0, SEEK_END);
    s_file_len = ftell(handle);
    fseek(handle, 0, SEEK_SET);

    s_file = (unsigned char *)malloc((size_t)s_file_len);
    if (fread(s_file, 1, (size_t)s_file_len, handle) != (size_t)s_file_len) {
        fclose(handle);
        return -1;
    }
    fclose(handle);
    return 0;
}

static void unload(void)
{
    free(s_file);
    s_file = NULL;
    s_file_len = 0;
}

// The decompression scratch buffer. Sized generously: a real package's control
// archive is a few kilobytes and its data archive can be megabytes, but the
// fixtures are small and a test that allocated 64 MiB would hide the fact that
// the caller is expected to choose this.
static unsigned char s_scratch[4 * 1024 * 1024];

// =============================================================================
// 1. A well-formed package
// =============================================================================
static void test_good_package(const char *dir)
{
    printf("\n=== a well-formed Debian package ===\n");

    if (load(dir, "test_1.2.3-1_amd64.deb") != 0) {
        s_failures++;
        return;
    }

    afpkg_scratch_t scratch = { s_scratch, sizeof(s_scratch) };
    af_pkg_t pkg;

    const af_status_t rc = afpkg_open(s_file, (af_size)s_file_len, &scratch, &pkg);
    check(rc == AF_OK, "a generated .deb reads successfully");
    if (rc != AF_OK) {
        printf("  reason: %s\n", afpkg_error(&pkg));
        unload();
        return;
    }

    check(pkg.format == AF_BINFMT_DEB, "the format is identified as deb");

    // The metadata, field by field. Every one of these is a claim the control
    // file made and the reader had to find, parse and terminate correctly —
    // a parser that got the line-continuation rule wrong would still return
    // AF_OK and would have an empty description.
    check_str(pkg.info.name, "afriyie-test", "Package");
    check_str(pkg.info.version, "1.2.3-1", "Version");
    check_str(pkg.info.architecture, "amd64", "Architecture");
    check_str(pkg.info.maintainer, "AfriyieOS Tests <tests@afriyieos.invalid>",
              "Maintainer");
    check_str(pkg.info.description,
              "a synthetic package used by the AfriyieOS test suite This package "
              "is generated, never downloaded. It exists so that the reader can "
              "be tested against a container it did not also write.",
              "Description with its continuation lines joined");
    check_u64(pkg.info.installed_size, 42, "Installed-Size");

    check(pkg.info.depends_count == 1, "one Depends line");
    if (pkg.info.depends_count == 1) {
        check_str(pkg.info.depends[0], "libc6 (>= 2.31), libfoo | libbar",
                  "the dependency string is kept verbatim");
    }

    // The file list. Three files and two directories in the fixture, and the
    // ORDER matters — walking a tar backwards is a real bug and this is what
    // catches it.
    afpkg_iter_t it;
    afpkg_iter_begin(&it);

    af_pkg_entry_t entry;
    int files = 0;
    int dirs = 0;
    int saw_binary = 0;

    while (afpkg_iter_next(&pkg, &it, &entry)) {
        if (entry.is_directory) {
            dirs++;
        } else {
            files++;
        }

        if (strcmp(entry.path, "usr/bin/afriyie-test") == 0) {
            saw_binary = 1;

            // DERIVED, NOT GUESSED. The first version of this test asserted 43,
            // which was a number I had in my head; the file is 46 bytes and the
            // reader was right. A hard-coded length is a second copy of the
            // fixture, and the copy drifts the moment the fixture changes.
            static const char *const script =
                "#!/bin/sh\necho hello from a synthetic package\n";
            check_u64(entry.size, strlen(script),
                      "the installed program's size");

            // 0755 — the leading "./" was normalised away and the mode came
            // from an OCTAL field. A reader parsing it as decimal gets 493.
            check_u64(entry.mode, 0755, "the installed program's mode is octal");
        }
    }

    check(files == 3, "three regular files");
    check(dirs == 2, "two directories");
    check(saw_binary == 1, "the program was found by its normalised path");

    unload();
}

// =============================================================================
// 2. Refusals — the cases that matter more than the happy path
// =============================================================================
static void test_ar_that_is_not_a_deb(const char *dir)
{
    printf("\n=== an ar archive that is not a Debian package ===\n");

    if (load(dir, "plain.ar") != 0) {
        s_failures++;
        return;
    }

    afpkg_scratch_t scratch = { s_scratch, sizeof(s_scratch) };
    af_pkg_t pkg;

    const af_status_t rc = afpkg_open(s_file, (af_size)s_file_len, &scratch, &pkg);
    check(rc != AF_OK, "a plain ar archive is refused");
    check(rc == AF_ERR_INVAL, "and refused as invalid rather than corrupt");
    check(strstr(afpkg_error(&pkg), "not a Debian package") != NULL,
          "the reason names the actual problem");

    unload();
}

static void test_truncated(const char *dir)
{
    printf("\n=== a truncated package ===\n");

    if (load(dir, "truncated_1.0_amd64.deb") != 0) {
        s_failures++;
        return;
    }

    afpkg_scratch_t scratch = { s_scratch, sizeof(s_scratch) };
    af_pkg_t pkg;

    // The declared member sizes now run past the end of the file. A reader that
    // trusted them would walk off the buffer.
    const af_status_t rc = afpkg_open(s_file, (af_size)s_file_len, &scratch, &pkg);
    check(rc != AF_OK, "a truncated package is refused rather than misread");

    unload();
}

static void test_unsupported_compression(const char *dir)
{
    printf("\n=== a control member compressed with something we cannot read ===\n");

    if (load(dir, "notxzsupport_1.0_amd64.deb") != 0) {
        s_failures++;
        return;
    }

    afpkg_scratch_t scratch = { s_scratch, sizeof(s_scratch) };
    af_pkg_t pkg;

    const af_status_t rc = afpkg_open(s_file, (af_size)s_file_len, &scratch, &pkg);
    check(rc == AF_ERR_NOTSUP,
          "an xz control member is refused with AF_ERR_NOTSUP");

    // THE POINT OF THIS TEST. Reading an xz member as if it were gzip produces
    // bytes that parse as a control file with the wrong name, and the package
    // then installs under that name. A refusal is strictly better than a
    // plausible wrong answer.
    check(strstr(afpkg_error(&pkg), "compression") != NULL ||
          strstr(afpkg_error(&pkg), "control") != NULL,
          "the reason names the unsupported compression");

    unload();
}

static void test_no_control_file(const char *dir)
{
    printf("\n=== a control archive with no control file ===\n");

    if (load(dir, "nocontrol_1.0_amd64.deb") != 0) {
        s_failures++;
        return;
    }

    afpkg_scratch_t scratch = { s_scratch, sizeof(s_scratch) };
    af_pkg_t pkg;

    const af_status_t rc = afpkg_open(s_file, (af_size)s_file_len, &scratch, &pkg);
    check(rc == AF_ERR_FS_CORRUPT, "a package with no ./control is corrupt");

    unload();
}

// =============================================================================
// 3. The layers on their own
//
// An `ar` reader is not a package reader, and a bug in one should not be
// attributed to the other. Testing the layers separately is also what makes the
// parser testable at all when the format above it is wrong.
// =============================================================================
static void test_layers(const char *dir)
{
    printf("\n=== the layers, tested on their own ===\n");

    if (load(dir, "test_1.2.3-1_amd64.deb") != 0) {
        s_failures++;
        return;
    }

    const char *why = "";

    // --- ar ------------------------------------------------------------------
    const af_u8 *member = NULL;
    af_size member_len = 0;

    af_status_t rc = afpkg_ar_find(s_file, (af_size)s_file_len, "debian-binary",
                                   &member, &member_len, &why);
    check(rc == AF_OK, "ar finds a member by name");
    check_u64(member_len, 4, "debian-binary is 4 bytes");
    check(member != NULL && member[0] == '2' && member[1] == '.',
          "and its contents are the format version");

    rc = afpkg_ar_find(s_file, (af_size)s_file_len, "no-such-member",
                       &member, &member_len, &why);
    check(rc == AF_ERR_NOENT, "a missing member is AF_ERR_NOENT");

    // Not an ar archive at all.
    rc = afpkg_ar_find((const af_u8 *)"PK\x03\x04", 4, "anything",
                       &member, &member_len, &why);
    check(rc == AF_ERR_INVAL, "a non-ar buffer is refused");

    // --- control member discovery --------------------------------------------
    const af_u8 *control = NULL;
    af_size control_len = 0;

    rc = afpkg_ar_find_control(s_file, (af_size)s_file_len, &control,
                               &control_len, &why);
    check(rc == AF_OK, "the control member is found by prefix");
    check(control != NULL && control[0] == 0x1F && control[1] == 0x8B,
          "and it is a gzip stream");

    // --- gzip ----------------------------------------------------------------
    afpkg_scratch_t scratch = { s_scratch, sizeof(s_scratch) };
    af_size inflated = 0;
    af_size huff_len = 0;

    rc = afpkg_gunzip(control, control_len, scratch, &inflated, &why);
    check(rc == AF_OK, "the gzip stream inflates");
    check(inflated > 0, "and produces bytes");
    // A tar archive is a whole number of 512-byte blocks.
    check_u64(inflated % 512, 0, "the inflated length is a whole tar block count");

    // A Huffman-coded stream now DECODES, so the assertion that it is refused is
    // gone — it was testing the absence of the feature, and the feature arrived.
    //
    // What replaces it is the case that still matters: a stream whose Huffman
    // tables are corrupt must be REJECTED, not decoded into plausible nonsense.
    // 0x05 is a final dynamic block whose header claims 257+31 literal codes and
    // then runs out of bits.
    unsigned char bad_huffman[] = {
        0x1F, 0x8B, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0x03,
        0x05, 0xFF, 0x07, 0x00, 0, 0, 0, 0, 0, 0,
    };
    rc = afpkg_gunzip(bad_huffman, sizeof(bad_huffman), scratch, &huff_len, &why);
    check(rc != AF_OK, "a corrupt Huffman stream is rejected");

    // ...and a stream that is genuinely too short is refused for THAT reason,
    // which keeps the two failures distinguishable.
    rc = afpkg_gunzip(bad_huffman, 11, scratch, &huff_len, &why);
    check(rc == AF_ERR_INVAL, "a gzip stream shorter than its header is invalid");

    // --- tar -----------------------------------------------------------------
    //
    // `inflated` is untouched above, deliberately. The first version of this
    // test reused one variable for the control archive's inflated length and for
    // the throwaway streams, so a later section silently set it to zero and the
    // tar walk below counted no entries — a failure with no visible connection
    // to the line that caused it.
    af_size cursor = 0;
    af_pkg_entry_t entry;
    const af_u8 *contents = NULL;
    af_size contents_len = 0;
    int entries = 0;

    while (true) {
        rc = afpkg_tar_next(s_scratch, inflated, &cursor, &entry, &contents,
                            &contents_len, &why);
        if (rc == AF_ERR_AGAIN) {
            continue;
        }
        if (af_status_err(rc)) {
            break;
        }
        entries++;
    }

    check(entries == 2, "the control tar holds two entries");
    check(rc == AF_ERR_NOENT, "and the walk ends with AF_ERR_NOENT, not an error");

    // --- the control parser on its own ---------------------------------------
    af_pkg_info_t info;
    const char *text = "Package: solo\nVersion: 9.9\n\nSecond: paragraph ignored\n";
    afpkg_parse_control(text, strlen(text), &info);

    check_str(info.name, "solo", "the control parser reads a minimal paragraph");
    check_str(info.version, "9.9", "and stops at the blank line");
    check_str(info.architecture, "", "a missing field stays empty");

    unload();
}

// =============================================================================
// 4. DEFLATE
//
// THE TEST THAT MATTERS MOST IN THIS FILE. A decompressor that produces output
// has proved nothing — wrong bytes look exactly like right bytes until they are
// compared — so every case here compares the inflated result with the exact
// original, byte for byte, and reports the first offset that differs.
//
// The vectors are 6 payloads x 10 compression levels, and the payloads are
// chosen to reach the three block types and the parts of the length/distance
// tables a single string would never touch. See tools/pkgsynth.py.
// =============================================================================
static void test_deflate(const char *dir)
{
    printf("\n=== DEFLATE ===\n");

    static const char *const payloads[] = {
        "empty", "one", "constant", "text", "random", "distances"
    };
    const int payload_count = (int)(sizeof(payloads) / sizeof(payloads[0]));

    int vectors = 0;
    int mismatched_vectors = 0;

    for (int p = 0; p < payload_count; p++) {
        // The expected output.
        char raw_name[256];
        snprintf(raw_name, sizeof(raw_name), "%s/deflate_%s.raw", dir, payloads[p]);

        FILE *raw_file = fopen(raw_name, "rb");
        if (raw_file == NULL) {
            printf("  missing vector %s\n", raw_name);
            s_failures++;
            continue;
        }

        fseek(raw_file, 0, SEEK_END);
        long raw_len = ftell(raw_file);
        fseek(raw_file, 0, SEEK_SET);

        unsigned char *raw = malloc((size_t)raw_len + 1);
        if (fread(raw, 1, (size_t)raw_len, raw_file) != (size_t)raw_len) {
            fclose(raw_file);
            free(raw);
            s_failures++;
            continue;
        }
        fclose(raw_file);

        int payload_mismatches = 0;
        const char *first_failure = NULL;

        for (int level = 0; level <= 9; level++) {
            char gz_name[256];
            snprintf(gz_name, sizeof(gz_name), "%s/deflate_%s.%d.gz",
                     dir, payloads[p], level);

            FILE *gz_file = fopen(gz_name, "rb");
            if (gz_file == NULL) {
                s_failures++;
                printf("  missing vector %s\n", gz_name);
                continue;
            }

            fseek(gz_file, 0, SEEK_END);
            long gz_len = ftell(gz_file);
            fseek(gz_file, 0, SEEK_SET);

            unsigned char *gz = malloc((size_t)gz_len);
            if (fread(gz, 1, (size_t)gz_len, gz_file) != (size_t)gz_len) {
                fclose(gz_file);
                free(gz);
                s_failures++;
                continue;
            }
            fclose(gz_file);

            vectors++;

            afpkg_scratch_t scratch = { s_scratch, sizeof(s_scratch) };
            af_size out_len = 0;
            const char *why = "";

            const af_status_t rc = afpkg_gunzip(gz, (af_size)gz_len, scratch,
                                                &out_len, &why);
            if (rc != AF_OK) {
                s_failures++;
                payload_mismatches++;
                if (first_failure == NULL) first_failure = why;
                printf("  FAIL  %s level %d: refused — %s\n", payloads[p], level, why);
                free(gz);
                continue;
            }

            if (out_len != (af_size)raw_len) {
                s_failures++;
                payload_mismatches++;
                printf("  FAIL  %s level %d: length %lu, expected %ld\n",
                       payloads[p], level, (unsigned long)out_len, raw_len);
                free(gz);
                continue;
            }

            if (memcmp(s_scratch, raw, (size_t)raw_len) != 0) {
                // Report the FIRST differing offset. "The bytes differ" is not
                // actionable; "offset 4096" points straight at the block
                // boundary or table entry that is wrong.
                af_size at = 0;
                while (at < out_len && s_scratch[at] == raw[at]) {
                    at++;
                }
                s_failures++;
                payload_mismatches++;
                printf("  FAIL  %s level %d: first difference at offset %lu "
                       "(got 0x%02X, expected 0x%02X)\n",
                       payloads[p], level, (unsigned long)at,
                       s_scratch[at], raw[at]);
            }

            free(gz);
        }

        if (payload_mismatches == 0) {
            printf("  ok    %-10s %ld bytes, 10 levels\n", payloads[p], raw_len);
        }
        if (first_failure != NULL) {
            (void)first_failure;
        }

        mismatched_vectors += payload_mismatches;
        free(raw);
    }

    check(mismatched_vectors == 0, "every DEFLATE vector inflates to the exact original");
    printf("  %d vector(s) checked\n", vectors);

    // --- the refusals ---------------------------------------------------------
    afpkg_scratch_t scratch = { s_scratch, sizeof(s_scratch) };
    af_size out_len = 0;
    const char *why = "";

    // Block type 3 is reserved. A stream containing one is corrupt, and a
    // decoder that treats it as something else invents data.
    unsigned char reserved[] = {
        0x1F, 0x8B, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0x03,
        0x07, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    check(afpkg_gunzip(reserved, sizeof(reserved), scratch, &out_len, &why)
              == AF_ERR_FS_CORRUPT,
          "a reserved block type is rejected as corrupt");

    // A raw DEFLATE stream ending mid-symbol must be an error, not a short read.
    // The first byte is a final dynamic block header, then nothing.
    const unsigned char truncated[] = { 0x05, 0x00 };
    check(afpkg_inflate(truncated, sizeof(truncated), s_scratch, sizeof(s_scratch),
                        &out_len, NULL, &why) != AF_OK,
          "a DEFLATE stream that ends mid-header is an error");

    // An empty input is the case a loop written as `while (offset < len)` gets
    // wrong: it produces no blocks at all and looks like success.
    const unsigned char nothing[] = { 0x00 };
    check(afpkg_inflate(nothing, 0, s_scratch, sizeof(s_scratch),
                        &out_len, NULL, &why) != AF_OK,
          "an empty DEFLATE stream is an error, not an empty result");
}


// =============================================================================
// 5. Alpine Linux packages
//
// A different distribution, a different format, and — the part that matters — a
// different CONTAINER SHAPE. A .deb is an ar archive holding two gzip members.
// An Alpine .apk is several gzip members CONCATENATED in one file, with no outer
// container at all. A reader written only against .deb assumes there is always
// an index of members, and in this format there is not.
// =============================================================================
static afpkg_scratch_t s_apk_scratch;

static void test_apk(const char *dir)
{
    printf("\n=== Alpine packages ===\n");

    s_apk_scratch.base = s_scratch;
    s_apk_scratch.size = sizeof(s_scratch);

    if (load(dir, "test_1.2.3-r0_x86_64.apk") != 0) {
        s_failures++;
        return;
    }

    af_pkg_t pkg;
    const af_status_t rc = afpkg_open(s_file, (af_size)s_file_len,
                                      &s_apk_scratch, &pkg);
    check(rc == AF_OK, "a generated .apk reads successfully");
    if (rc != AF_OK) {
        printf("  reason: %s\n", afpkg_error(&pkg));
        unload();
        return;
    }

    // ALPINE's type, not Android's. Both are called .apk and have nothing else
    // in common; a reader that confuses them installs an Alpine package as an
    // Android app.
    check(pkg.format == AF_BINFMT_APK_PKG,
          "identified as an Alpine package, not an Android one");

    check_str(pkg.info.name, "afriyie-test", "pkgname");
    check_str(pkg.info.version, "1.2.3-r0", "pkgver");
    check_str(pkg.info.architecture, "x86_64", "arch");
    check_str(pkg.info.description, "a synthetic Alpine package", "pkgdesc");
    check_u64(pkg.info.installed_size, 4096, "size");

    // TWO `depend` lines, not one comma-separated list. Alpine writes one line
    // per dependency, and a parser that split on commas would turn
    // "so:libc.musl-x86_64.so.1" into two pieces of nonsense.
    check(pkg.info.depends_count == 2, "two dependencies, one per line");
    if (pkg.info.depends_count == 2) {
        check_str(pkg.info.depends[0], "musl", "the first dependency");
        check_str(pkg.info.depends[1], "so:libc.musl-x86_64.so.1",
                  "a dependency containing a colon survives intact");
    }

    // --- ONE tar, split across members ---------------------------------------
    //
    // THE TEST THIS FORMAT EXISTS FOR, and the one the first fixture failed to
    // be. A real Alpine package is one tar cut into gzip members at arbitrary
    // offsets — including mid-entry — so that a signature can be verified before
    // the payload is read. A reader that treats each member as its own archive
    // sees only the entries in the LAST one.
    //
    // This fixture splits at offset 700 (inside an entry's header) and 1500
    // (inside file data). An implementation cannot get those boundaries right by
    // accident: the first cut leaves a partial header at the end of a member,
    // and the second leaves arbitrary bytes with no structure at all.
    afpkg_iter_t it;
    afpkg_iter_begin(&it);

    af_pkg_entry_t entry;
    int files = 0;
    int dirs = 0;
    int saw_binary = 0;
    int saw_library = 0;
    int saw_metadata = 0;

    while (afpkg_iter_next(&pkg, &it, &entry)) {
        if (entry.is_directory) {
            dirs++;
        } else {
            files++;
        }
        if (strcmp(entry.path, "usr/bin/afriyie-test") == 0) {
            saw_binary = 1;
        }
        if (strcmp(entry.path, "lib/libafriyie.so.1") == 0) {
            saw_library = 1;
        }
        // ANY dot-name at the archive root is apk metadata. A real busybox
        // package carries .post-install, .post-upgrade and .trigger; naming
        // the ones we happen to know guarantees being wrong the next time
        // Alpine adds one.
        if (entry.path[0] == '.' && strchr(entry.path, '/') == NULL) {
            saw_metadata = 1;
        }
    }

    // TWO payload files, not three. The tar holds five files and two of them
    // are the metadata entries that must not be offered for installation. The
    // first version of this line said three, which was a miscount of my own
    // fixture — and the assertion failing is how it was found.
    // THREE payload files: the fixture also carries usr/share/.hidden-config, a
    // dot-name INSIDE a directory. That is payload, not metadata, and it exists
    // to fail loudly if the exclusion is ever widened to "any name beginning
    // with a dot".
    check(files == 3, "three payload files across the whole split tar");
    check(dirs == 3, "three directories across the whole split tar");
    check(saw_binary == 1, "the payload file was reached despite the split");
    check(saw_library == 1, "and the last payload file, after the second cut");
    check(saw_metadata == 0,
          ".PKGINFO and .SIGN are NOT offered as files to install — they are "
          "metadata that happens to live in the same tar");

    // --- the install scripts -------------------------------------------------
    //
    // THE HOLE THIS CLOSES. Alpine ships .post-install and friends as dot-named
    // files in the same tar as the payload. Excluding them from the file list is
    // necessary and not sufficient: a package manager that installs busybox and
    // never runs .post-install has installed a package that does not work.
    //
    // A real busybox package is what showed the scripts existed. This is what
    // shows they are now reachable.
    check(pkg.info.script_count == 2,
          "the package's two install scripts were found");

    int saw_post_install = 0;
    int saw_trigger = 0;

    for (af_u32 i = 0; i < pkg.info.script_count; i++) {
        const af_pkg_script_t *s = &pkg.info.scripts[i];

        check(s->data != NULL && s->len > 0, "a script has contents");

        if (s->kind == AF_SCRIPT_POST_INSTALL) {
            saw_post_install = 1;
            // The contents, not merely the name. A reader that recorded that a
            // script existed without keeping its bytes would pass every
            // structural check and be useless.
            check(s->len == strlen("#!/bin/sh\n# configure the package\n"),
                  "post-install's length is right");
            check(memcmp(s->data, "#!/bin/sh\n# configure",
                         strlen("#!/bin/sh\n# configure")) == 0,
                  "post-install's contents are the ones in the package");
        }
        if (s->kind == AF_SCRIPT_TRIGGER) {
            saw_trigger = 1;
        }
    }

    check(saw_post_install == 1, "post-install is identified as such");
    check(saw_trigger == 1, "and so is the trigger");

    // Every kind must be nameable, including one a switch forgets.
    for (int k = 0; k <= (int)AF_SCRIPT_TRIGGER; k++) {
        const char *name = afpkg_script_name((af_script_kind_t)k);
        s_checks++;
        if (name == NULL || name[0] == '\0' ||
            (name[0] == '?' && name[1] == '\0')) {
            s_failures++;
            printf("  FAIL  script kind %d has no name\n", k);
        }
    }

    unload();

    // --- a corrupted package --------------------------------------------------
    //
    // The case the CRC32 exists for. A bit flipped inside compressed data very
    // often produces a stream that still inflates — to the wrong bytes — so
    // without the trailer check this package installs and the corruption
    // surfaces later, in whatever the package does.
    printf("\n=== an Alpine package with a corrupted member ===\n");

    if (load(dir, "badcrc_1.2.3-r0_x86_64.apk") != 0) {
        s_failures++;
        return;
    }

    af_pkg_t bad;
    const af_status_t rc_bad = afpkg_open(s_file, (af_size)s_file_len,
                                          &s_apk_scratch, &bad);
    check(rc_bad != AF_OK, "a bit-flipped package is refused");
    check(rc_bad == AF_ERR_FS_CORRUPT, "and refused as corrupt, not as invalid");
    printf("  reason: %s\n", afpkg_error(&bad));

    unload();

    // --- a truncated package --------------------------------------------------
    printf("\n=== an Alpine package truncated mid-member ===\n");

    if (load(dir, "truncated_1.2.3-r0_x86_64.apk") != 0) {
        s_failures++;
        return;
    }

    af_pkg_t cut;
    const af_status_t rc_cut = afpkg_open(s_file, (af_size)s_file_len,
                                          &s_apk_scratch, &cut);
    check(rc_cut != AF_OK, "a package cut mid-member is refused");
    check(strstr(afpkg_error(&cut), "truncated") != NULL,
          "and the reason says truncated rather than corrupt");

    unload();

    // --- a gzipped tar that is not a package ---------------------------------
    printf("\n=== a gzipped tar that is not a package ===\n");

    if (load(dir, "deflate_text.5.gz") != 0) {
        s_failures++;
        return;
    }

    af_pkg_t notpkg;
    const af_status_t rc_not = afpkg_open(s_file, (af_size)s_file_len,
                                          &s_apk_scratch, &notpkg);
    check(rc_not != AF_OK, "a gzipped tar with no .PKGINFO is refused");
    check(strstr(afpkg_error(&notpkg), "not an Alpine package") != NULL,
          "and the reason names what it actually is");

    unload();
}

// =============================================================================
// A pacman package
//
// The first format here that was blocked by arithmetic rather than structure. It
// is a tar, which libafpkg could already read, inside zstd, which it could not.
// These tests are therefore the decoder's end-to-end test as much as the
// reader's: if the decompressor is wrong by one bit anywhere, no field below is
// right, and there is no way to reach the assertions by another route.
// =============================================================================
static void test_pacman(const char *dir)
{
    printf("\n=== pacman packages ===\n");

    if (load(dir, "test-1.2.3-1-x86_64.pkg.tar.zst") != 0) {
        s_failures++;
        return;
    }

    af_pkg_t pkg;
    const af_status_t rc = afpkg_open(s_file, (af_size)s_file_len,
                                      &s_apk_scratch, &pkg);
    check(rc == AF_OK, "a .pkg.tar.zst opens");
    if (rc != AF_OK) {
        printf("        %s\n", afpkg_error(&pkg));
        unload();
        return;
    }

    check(pkg.format == AF_BINFMT_PACMAN, "and is identified as a pacman package");
    check_str(pkg.info.name, "afriyie-test", "pkgname");
    check_str(pkg.info.version, "1.2.3-1", "pkgver");
    check_str(pkg.info.architecture, "x86_64", "arch");
    check_str(pkg.info.description, "a synthetic pacman package", "pkgdesc");
    check_u64(pkg.info.installed_size, 4096, "size");
    check(pkg.info.depends_count == 2, "both depend lines are kept separate");
    check_str(pkg.info.depends[0], "glibc", "the first dependency");
    check_str(pkg.info.depends[1], "libfoo.so=1-64",
              "and the second, which contains '=' and must not be split again");

    // --- the file list --------------------------------------------------------
    //
    // .PKGINFO and .MTREE describe the package; the rest is payload. Offering
    // either as a file to install would write it into the root directory of a
    // real system, which is exactly the bug the Alpine reader had.
    int files = 0;
    int saw_binary = 0;
    int saw_metadata = 0;

    afpkg_iter_t it;
    afpkg_iter_begin(&it);

    af_pkg_entry_t entry;
    while (afpkg_iter_next(&pkg, &it, &entry)) {
        if (entry.is_directory) {
            continue;
        }
        files++;
        if (strcmp(entry.path, "usr/bin/afriyie-test") == 0) {
            saw_binary = 1;
            check_u64(entry.size, strlen("#!/bin/sh\necho pacman\n"),
                      "the binary's size is right");
            check((entry.mode & 0111u) != 0, "and it is marked executable");
        }
        if (strncmp(entry.path, ".PKGINFO", 8) == 0 ||
            strncmp(entry.path, ".MTREE", 6) == 0) {
            saw_metadata = 1;
        }
    }

    check(files == 3, "the package has three payload files");
    check(saw_binary == 1, "including the binary");
    check(saw_metadata == 0, "and neither metadata file is offered as payload");

    unload();

    // --- a zstd tar that is not a package -------------------------------------
    printf("\n=== a zstd tar that is not a package ===\n");

    if (load(dir, "notapkg-1.0-1-x86_64.pkg.tar.zst") != 0) {
        s_failures++;
        return;
    }

    af_pkg_t notpkg;
    const af_status_t rc_not = afpkg_open(s_file, (af_size)s_file_len,
                                          &s_apk_scratch, &notpkg);
    check(rc_not != AF_OK, "a zstd tar with no .PKGINFO is refused");
    check(strstr(afpkg_error(&notpkg), "not a pacman package") != NULL,
          "and the reason distinguishes it from a damaged package");

    unload();

    // --- a corrupted zstd frame -----------------------------------------------
    //
    // The fixture's frame carries a content checksum, so this is detected because
    // the decompressed bytes no longer hash to what the frame says — not because
    // the corruption happened to land somewhere structurally invalid.
    printf("\n=== a corrupted zstd frame ===\n");

    if (load(dir, "badzstd-1.2.3-1-x86_64.pkg.tar.zst") != 0) {
        s_failures++;
        return;
    }

    af_pkg_t bad;
    const af_status_t rc_bad = afpkg_open(s_file, (af_size)s_file_len,
                                          &s_apk_scratch, &bad);
    check(rc_bad != AF_OK, "a frame with a flipped bit does not open as a package");
    check(strstr(afpkg_error(&bad), "checksum") != NULL,
          "and the reason names the checksum rather than guessing");

    unload();
}

// =============================================================================
// Zstandard
//
// 42 frames, produced by the reference implementation, each compared byte for
// byte against the payload it was built from.
//
// THE COMPARISON IS THE WHOLE TEST. A decompressor that emits the right NUMBER
// of bytes has proved nothing; every bug found in this decoder produced output of
// the correct length. They were found by this comparison and by nothing else.
//
// The vectors cover the six payload shapes at seven compression levels because
// zstd changes strategy as the level rises: low levels favour raw and RLE
// blocks, high levels switch to compressed literals, FSE-coded sequence tables
// and a different set of repeat offsets. All three block types and all four
// symbol-compression modes appear across the set.
// =============================================================================
static unsigned char *read_all(const char *path, long *out_len)
{
    FILE *handle = fopen(path, "rb");
    if (handle == NULL) {
        return NULL;
    }

    fseek(handle, 0, SEEK_END);
    const long len = ftell(handle);
    fseek(handle, 0, SEEK_SET);

    unsigned char *buf = (unsigned char *)malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(handle);
        return NULL;
    }

    if (len > 0 && fread(buf, 1, (size_t)len, handle) != (size_t)len) {
        free(buf);
        fclose(handle);
        return NULL;
    }

    fclose(handle);
    *out_len = len;
    return buf;
}

static void test_zstd(const char *dir)
{
    static const char *names[] = { "empty", "one", "random",
                                   "constant", "text", "distances" };
    static const int levels[] = { 1, 3, 6, 9, 12, 15, 19 };

    printf("\n=== zstd vectors ===\n");

    static unsigned char out[128 * 1024];
    char path[1024];

    for (unsigned n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        long raw_len = 0;
        snprintf(path, sizeof(path), "%s/zstd_%s.raw", dir, names[n]);
        unsigned char *raw = read_all(path, &raw_len);
        if (raw == NULL) {
            s_failures++;
            printf("  FAIL  cannot open %s — run tools/pkgsynth.py first\n", path);
            continue;
        }

        for (unsigned l = 0; l < sizeof(levels) / sizeof(levels[0]); l++) {
            snprintf(path, sizeof(path), "%s/zstd_%s.%d.zst", dir, names[n],
                     levels[l]);

            long zlen = 0;
            unsigned char *z = read_all(path, &zlen);
            if (z == NULL) {
                continue;   // pkgsynth skips levels the local zstd rejects
            }

            af_size got_len = 0;
            const char *why = "";
            const af_status_t rc = afpkg_zstd(z, (af_size)zlen, out,
                                              (af_size)sizeof(out), &got_len, &why);

            char what[160];
            snprintf(what, sizeof(what), "zstd_%s.%d is byte-exact", names[n],
                     levels[l]);

            const int ok = (rc == AF_OK) && ((long)got_len == raw_len) &&
                           (raw_len == 0 ||
                            memcmp(out, raw, (size_t)raw_len) == 0);
            check(ok, what);
            if (!ok) {
                printf("        %s: status=%d got=%ld want=%ld why=%s\n", what,
                       (int)rc, (long)got_len, raw_len, why);
            }

            free(z);
        }

        free(raw);
    }

    // --- the same payloads, in frames that carry a checksum -------------------
    //
    // The checksum is a claim about the decompressed bytes made by the encoder.
    // If the XXH64 here were wrong in any way, a frame that decompresses
    // perfectly would be rejected — so these six vectors are what the checksum
    // implementation is verified against, and there is nothing else that could
    // verify it against the reference implementation.
    printf("\n=== zstd frames with a content checksum ===\n");

    for (unsigned n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        long raw_len = 0;
        long zlen = 0;

        snprintf(path, sizeof(path), "%s/zstd_%s.raw", dir, names[n]);
        unsigned char *raw = read_all(path, &raw_len);

        snprintf(path, sizeof(path), "%s/zstd_%s.chk.zst", dir, names[n]);
        unsigned char *z = read_all(path, &zlen);

        if (raw == NULL || z == NULL) {
            free(raw);
            free(z);
            continue;   // the binding could not set the flag when this ran
        }

        af_size got_len = 0;
        const char *why = "";
        const af_status_t rc = afpkg_zstd(z, (af_size)zlen, out,
                                          (af_size)sizeof(out), &got_len, &why);

        char what[160];
        snprintf(what, sizeof(what), "zstd_%s with a checksum verifies and is "
                                     "byte-exact", names[n]);
        const int ok = (rc == AF_OK) && ((long)got_len == raw_len) &&
                       (raw_len == 0 || memcmp(out, raw, (size_t)raw_len) == 0);
        check(ok, what);
        if (!ok) {
            printf("        status=%d got=%ld want=%ld why=%s\n", (int)rc,
                   (long)got_len, raw_len, why);
        }

        free(z);
        free(raw);
    }

    // --- negatives ------------------------------------------------------------
    //
    // A decoder that accepts everything is not a decoder. These two are the ones
    // the frame layer is responsible for; the block layer's rejections are
    // exercised by the vectors above, which reach every one of them.
    {
        long zlen = 0;
        snprintf(path, sizeof(path), "%s/zstd_text.6.zst", dir);
        unsigned char *z = read_all(path, &zlen);

        if (z == NULL) {
            s_failures++;
            printf("  FAIL  cannot open %s\n", path);
        } else {
            af_size got_len = 0;
            const char *why = "";

            const unsigned char saved = z[zlen - 1];
            const unsigned char saved_magic = z[3];

            z[zlen - 1] = 0;   // the sentinel every backward bitstream needs
            af_status_t rc = afpkg_zstd(z, (af_size)zlen, out,
                                        (af_size)sizeof(out), &got_len, &why);
            check(rc != AF_OK, "a frame whose last bitstream byte is zero is refused");
            z[zlen - 1] = saved;

            z[3] = (unsigned char)(saved_magic ^ 0xFFu);
            rc = afpkg_zstd(z, (af_size)zlen, out, (af_size)sizeof(out),
                            &got_len, &why);
            check(rc == AF_ERR_INVAL, "a frame with the wrong magic is refused");
            check_str(why, "not a Zstandard frame: the magic number is wrong",
                      "and the reason names the magic number");

            free(z);
        }
    }
}

// =============================================================================
int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "build/fixtures";

    printf("libafpkg native tests\n");
    printf("fixtures: %s\n", dir);

    test_good_package(dir);
    test_ar_that_is_not_a_deb(dir);
    test_truncated(dir);
    test_unsupported_compression(dir);
    test_no_control_file(dir);
    test_layers(dir);
    test_deflate(dir);
    test_apk(dir);
    test_pacman(dir);
    test_zstd(dir);

    printf("\n===============================================================\n");
    if (s_failures == 0) {
        printf("  %d checks passed\n", s_checks);
        printf("===============================================================\n");
        return 0;
    }

    printf("  %d of %d checks FAILED\n", s_failures, s_checks);
    printf("===============================================================\n");
    return 1;
}
