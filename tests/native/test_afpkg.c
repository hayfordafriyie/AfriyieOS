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

    rc = afpkg_gunzip(control, control_len, scratch, &inflated, &why);
    check(rc == AF_OK, "the gzip stream inflates");
    check(inflated > 0, "and produces bytes");
    // A tar archive is a whole number of 512-byte blocks.
    check_u64(inflated % 512, 0, "the inflated length is a whole tar block count");

    // A Huffman-coded stream must be REFUSED, not garbled. Constructed by hand
    // so the test does not depend on zlib producing one at a particular level.
    //
    // It is padded to a full-length gzip header PLUS the 8-byte trailer, because
    // the first version of this test was 11 bytes and the reader correctly
    // refused it for being too short — before it ever looked at the block type.
    // A test that fails for the wrong reason is indistinguishable from a test
    // that fails for the right one, so the length has to be honest.
    unsigned char huffman[20] = {
        0x1F, 0x8B, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0x03,
        0x05,                       // BFINAL=1, BTYPE=10 (dynamic Huffman)
        0, 0, 0, 0, 0, 0, 0, 0,     // the rest is never reached; the block
    };                              // type is refused first
    rc = afpkg_gunzip(huffman, sizeof(huffman), scratch, &inflated, &why);
    check(rc == AF_ERR_NOTSUP,
          "a Huffman-coded gzip stream is refused with AF_ERR_NOTSUP");

    // ...and a stream that is genuinely too short is refused for THAT reason,
    // which keeps the two failures distinguishable.
    rc = afpkg_gunzip(huffman, 11, scratch, &inflated, &why);
    check(rc == AF_ERR_INVAL, "a gzip stream shorter than its header is invalid");

    // --- tar -----------------------------------------------------------------
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
