#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""AfriyieOS — catch tooling syntax that the CI interpreter will reject.

THE BUG THIS EXISTS FOR
=======================

CI pins Python 3.11. Development happens on whatever the machine has, which was
3.14. `python -m compileall` on 3.14 compiles anything 3.14 understands, so a
file that only parses on 3.12+ passes every local check and fails CI.

`tools/mkimage.py` contained

    log(f"     wrote HELLO.TXT ({len(b'Hello from disk\\n')} bytes)")

PEP 701 (Python 3.12) lifted the ban on backslashes inside f-string expressions.
Before it, that line is a SyntaxError:

    SyntaxError: f-string expression part cannot include a backslash

It parsed on the development machine. And because that step is the FIRST job in
the workflow, its failure skipped every job after it — the build, the boot test,
the image verification. CI had never once run the kernel. One line of
version-dependent syntax hid the whole pipeline.

WHY NOT `ast.parse(..., feature_version=(3, 11))`
================================================

That was the first attempt, and it does not work. `feature_version` reverts
grammar productions; it does not revert the *tokenizer*, and PEP 701 was a
tokenizer change. Feeding it a file containing the line above was tested and it
reported zero problems. A check that cannot fail on the bug it was written for
is worse than no check, because it is trusted.

WHAT THIS DOES INSTEAD
======================

Two independent checks, and it reports which ones it was able to run:

1. If a real interpreter at the target version is present (`AF_PY_MIN`, or
   `python3.11` on PATH), compile every file with it. This is the only complete
   check — it catches syntax and anything else version-dependent — and it is what
   CI gets for free.

2. Always, on Python 3.12+, tokenize each file and look for a backslash inside
   an f-string *replacement field*. From 3.12 the tokenizer emits FSTRING_START,
   FSTRING_MIDDLE and FSTRING_END, so the literal parts are distinguishable from
   the expressions; a backslash in a literal part is a legal escape, and a
   backslash anywhere else inside the f-string is the construct PEP 701 un-banned.
   This is targeted rather than general, and it is honest about that.

Check 1 alone is sufficient. Check 2 is what makes the failure visible on a
development machine that has only a newer interpreter, which is the situation
that caused the problem.

Usage:
    python3 tools/pycompat.py            # check tools/ and tests/
    python3 tools/pycompat.py -v         # list every file checked
"""

from __future__ import annotations

import argparse
import io
import pathlib
import subprocess
import sys
import tokenize

# The version CI pins. Keep in step with .github/workflows/ci.yml — the point is
# that the two agree, and changing one alone puts this back where it started.
TARGET = (3, 11)

SEARCH_DIRS = ("tools", "tests")


def files_to_check(root: pathlib.Path):
    for directory in SEARCH_DIRS:
        base = root / directory
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*.py")):
            if "__pycache__" not in path.parts:
                yield path


# -----------------------------------------------------------------------------
# Check 1 — a real interpreter at the target version
# -----------------------------------------------------------------------------
def find_target_interpreter() -> str | None:
    import os
    import shutil

    explicit = os.environ.get("AF_PY_MIN")
    if explicit:
        return shutil.which(explicit) or (explicit if os.path.exists(explicit) else None)

    return shutil.which(f"python{TARGET[0]}.{TARGET[1]}")


def compile_with(interpreter: str, files) -> list[str]:
    problems = []
    for path in files:
        result = subprocess.run(
            [interpreter, "-m", "py_compile", str(path)],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            problems.append(f"{path}:\n{result.stderr.strip()}")
    return problems


# -----------------------------------------------------------------------------
# Check 2 — backslash inside an f-string replacement field
# -----------------------------------------------------------------------------
FSTRING_START = getattr(tokenize, "FSTRING_START", None)
FSTRING_END = getattr(tokenize, "FSTRING_END", None)
FSTRING_MIDDLE = getattr(tokenize, "FSTRING_MIDDLE", None)


def backslashes_in_fstrings(path: pathlib.Path) -> list[str]:
    """Returns messages for backslashes found inside f-string expressions.

    Returns an empty list — not an error — when the running interpreter predates
    3.12, because then this construct is already a syntax error that check 1 or
    plain compilation will report. There is nothing extra to add.
    """
    if FSTRING_START is None:
        return []

    source = path.read_text(encoding="utf-8", errors="replace")

    try:
        tokens = list(tokenize.generate_tokens(io.StringIO(source).readline))
    except (tokenize.TokenError, IndentationError, SyntaxError):
        # A file that will not tokenize is reported by compilation; do not
        # duplicate that here with a worse message.
        return []

    problems = []
    depth = 0

    for tok in tokens:
        if tok.type == FSTRING_START:
            depth += 1
            continue
        if tok.type == FSTRING_END:
            depth = max(0, depth - 1)
            continue

        # Inside an f-string, but not in its literal text: this is expression.
        if depth > 0 and tok.type != FSTRING_MIDDLE and "\\" in tok.string:
            problems.append(
                f"{path}:{tok.start[0]}:{tok.start[1]}: backslash in an f-string "
                f"expression — a SyntaxError before Python 3.12 (PEP 701)\n"
                f"    {source.splitlines()[tok.start[0] - 1].strip()}"
            )

    return problems


# -----------------------------------------------------------------------------
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("--root", default=None,
                        help="repository root (defaults to the parent of tools/)")
    args = parser.parse_args()

    root = (pathlib.Path(args.root) if args.root
            else pathlib.Path(__file__).resolve().parent.parent)

    files = list(files_to_check(root))

    print(f"checking Python tooling against {TARGET[0]}.{TARGET[1]} "
          f"(this interpreter is {sys.version_info.major}.{sys.version_info.minor})")

    interpreter = find_target_interpreter()
    problems: list[str] = []

    if interpreter:
        print(f"  check 1: compiling with {interpreter} — COMPLETE")
        problems += compile_with(interpreter, files)
    else:
        print(f"  check 1: python{TARGET[0]}.{TARGET[1]} not found — SKIPPED")
        print("           (set AF_PY_MIN to a real interpreter of that version to")
        print("            enable the complete check; CI always runs it)")

    if FSTRING_START is not None:
        print("  check 2: f-string expression scan — targeted")
        for path in files:
            problems += backslashes_in_fstrings(path)
    else:
        print("  check 2: not needed on this interpreter")

    if args.verbose:
        for path in files:
            print(f"  ok   {path.relative_to(root)}")

    print()
    print("=" * 63)
    if problems:
        print(f"  {len(files)} file(s) checked, {len(problems)} problem(s)")
        print("=" * 63)
        for problem in problems:
            print()
            print(problem)
        print()
        print("These parse on this interpreter but not on the one CI runs.")
        print("Either write the older syntax, or raise TARGET here AND the version")
        print("pinned in .github/workflows/ci.yml — changing only one of the two")
        print("puts the check back where it started.")
        return 1

    print(f"  {len(files)} file(s) checked, none rejected")
    print("=" * 63)
    return 0


if __name__ == "__main__":
    sys.exit(main())
