# AfriyieOS Debug Log

Every non-trivial bug, its root cause, and how it was found.

This file is the most valuable document in the repository by `v0.4`, because the
second time you meet a fault you have already solved it once. Write the entry
even when the fix was one character — **especially** when the fix was one
character, because that is the kind of bug you will write again.

## Entry format

```
### YYYY-MM-DD — short title
Milestone:  vX.Y
Symptom:    what was observed
Cause:      the actual root cause, not the first suspicion
Fix:        the change
Found by:   how it was located (bisection? static assert? reading the spec?)
Lesson:     the generalisable part
```

---

## 2026 — Entries from the Zstandard work

### Two orderings in one byte, and neither is the one you assume

**Milestone:** v0.10 (in progress — the decoder is not finished)
**Symptom:** every Zstandard frame containing a COMPRESSED block was refused,
while every frame containing only raw or RLE blocks decoded perfectly. 21 of 42
vectors passed, and the failing 21 were exactly the compressible payloads.
**Cause:** two independent ordering mistakes, found by two rounds of tracing.

**1. The sequence symbol-compression modes byte is laid out from the BOTTOM:**

```
    bits 1-0  reserved, must be zero
    bits 3-2  match length mode
    bits 5-4  offset mode
    bits 7-6  literal length mode
```

I had it top-down, so the literal-length mode was being read out of the reserved
bits. The first symptom was a first block reporting *"a sequence table repeats a
previous one, but there is none"* — a message that is completely true and points
entirely at the wrong place.

The reserved bits being at the BOTTOM is the opposite of every other field in
this format, which is why the assumption that they live at the top went
unexamined.

**2. zstd's Huffman codes are NOT canonical in the usual order.**

I built the literal table the way DEFLATE does — sort by code length ascending,
assign increasing codes — which is what every description of Huffman teaches.
zstd builds it from the WEIGHTS:

```
    weight w  ->  code length (tableLog + 1 - w)
                  occupying 2^(w-1) consecutive entries of a 2^tableLog table

    offsets accumulate for w = 1, 2, 3, ...
```

So the LONGEST codes get the LOWEST table indices. For code lengths (2, 2, 1)
that gives codes `00`, `01` and `1` — where canonical Huffman gives `10`, `11`
and `0`. Both are prefix-free and both work; only one is what the encoder writes.

Found by tracing the literals section, which was failing with "Huffman literal
decoding failed" — a message that came BEFORE the sequences errors that had been
masking it.
**Found by:** tracing, after eight combinations of sequence read orderings all
failed *identically*. That uniformity was the useful signal: it ruled the
orderings out and said the failure was upstream of them, which pointed at the
literals.
**Lesson, and it is the same one three times in this file now:** a format's
ordering conventions are its definition, and a decoder that is right about
everything except one of them produces plausible output or a confident error
message naming the wrong component. The trace is what converts "this is wrong"
into "this field is wrong", and guessing between plausible orderings costs a
round each time.
**Still open:** at least one more bug in the compressed-block path. The decoder
is committed INCOMPLETE and UNWIRED — see the status header in
`libs/libafpkg/zstd_decode.c`. Everything it does get right is listed there, and
it is in no test, so a green run still means what it says.

---

## 2026 — Entries from the package-metadata work

### Excluding the install scripts was necessary and not sufficient

**Milestone:** v0.9c
**Symptom:** none — this is the entry for a hole closed before it could be felt,
recorded because of how it was found.
**Cause:** the previous round fixed a real bug by excluding Alpine's dot-named
metadata from the file list. `.post-install`, `.post-upgrade` and `.trigger` were
no longer offered as files to install. That was right, and it left them reachable
nowhere:

> A package manager that installs busybox and never runs `.post-install` has
> installed a package that does not work — and reports success.

The fix had been recorded as incomplete in `afpkg.c` and in the evidence file at
the time, with the gap named rather than implied. This is the closing of it.
**Fix:** the scripts are collected during the same single walk of the tar that
finds `.PKGINFO`, with their CONTENTS, and exposed through `af_pkg_info_t`. The
test asserts the bytes rather than that a name was recorded — a reader that noted
a script existed without keeping it would pass every structural check and be
useless.

Verified against the real packages: busybox reports
`(post-install, post-upgrade, trigger)`, exactly what Python's `tarfile` sees in
the same file, and ca-certificates reports two of its own. Three of the five
carry none, which matters for a different reason — a reader that invented a
script, or crashed without one, would be wrong there.
**Found by:** writing the previous round's fix down as incomplete. The value of
that note was not in this file; it was in the decision four days later being
"close the named hole" rather than "what was I doing?".
**Lesson:** when a fix removes something from a list, the question to ask
immediately is where it went. Exclusion is a *move*, not a deletion, and the
destination has to exist. The version of this that would have shipped would have
looked completely correct: the file list right, no metadata leaked, every test
green, and every package silently unconfigured.

**STILL NOT DONE, named rather than implied:** the scripts are returned but
nothing RUNS them. An install script is untrusted code from a third party, so
executing one is a capability decision rather than a parsing one, and it needs
the sandboxed-command machinery that arrives with the package manager. A library
that returned script contents and executed them would be making that decision on
the caller's behalf, which is exactly the kind of authority this system is built
not to have.

---

## 2026 — Entries from real-package verification

### Every fixture passed. A real package disproved the format.

**Milestone:** v0.9b
**Symptom:** the first real Alpine package ever fed to the reader reported 11
entries where `tar` reported 13. Two files were missing, and the reason was not
obvious because the two that vanished were `.SIGN.RSA...` and `.PKGINFO` —
metadata, which a reader *should* exclude.
**Cause:** the reader treated each gzip member as its own tar archive and
reported the entries of the last one. The generated fixture was built the same
way — a control tar, then a data tar, each complete — so the fixture agreed with
the code and both were wrong about the format.

A real `.apk` is **one tar split across gzip members at arbitrary offsets**,
including mid-entry. The members are a streaming split so that a signature can be
verified before the payload is read; they are not separate archives.
**Fix:** every member is inflated into consecutive space in the scratch buffer
and the result is walked as a single tar. The fixture was rebuilt to split one tar
at offsets 700 and 1500 — inside an entry's header, and inside file data — which
is a boundary no implementation gets right by accident.
**Found by:** pointing the reader at a package from `dl-cdn.alpinelinux.org`,
which took about two minutes and found in one run what weeks of green fixtures
could not.
**Lesson:** a fixture written by the same person, in the same hour, from the same
understanding as the code, tests the understanding rather than the format. It is
not a wasted artefact — it guards against regressions in what we *do* know — but
it cannot tell us we are wrong. Only an artefact from outside can do that, and the
cheapest one is a real file from the real source.

---

### Metadata is whatever starts with a dot, not the two things we knew about

**Milestone:** v0.9b
**Symptom:** after the split-tar fix, four of five real packages read cleanly and
busybox reported **10 files where it should have had 7**.
**Cause:** the reader excluded exactly `.PKGINFO` and `.SIGN.*` from the install
list — the two kinds of metadata it knew about. busybox-1.36.1-r31.apk carries
three more:

```
.SIGN.RSA.alpine-devel@lists.alpinelinux.org-6165ee59.rsa.pub
.PKGINFO
.post-install
.post-upgrade
.trigger
```

They are Alpine's **install scripts**, and all three were offered as files to
install. That is wrong twice over: they would be written into the user's root
directory as files called `.post-install`, and the scripts' actual purpose — which
is to configure the package after installation — would never happen.
**Fix:** the rule is structural. An entry at the archive ROOT whose name begins
with `.` is control metadata, whatever it is called. The "no directory separator"
part of the test keeps `usr/share/.hidden-config` as payload, and the fixture now
carries one specifically so the exclusion cannot be quietly widened.
**Found by:** the verification tool reporting a nonzero metadata count rather
than a pass/fail, so the leak was visible as a number rather than hidden behind a
green tick.
**Lesson:** naming the special cases you know about guarantees being wrong about
the next one. The version of this rule that was implemented enumerated two kinds
of metadata; the version that works states the shape. When a rule can be written
structurally, writing it enumeratively is a promise to be wrong later — and the
later is always sooner than expected.

**AND THE GAP THIS EXPOSED, named rather than implied:** the install scripts are
now correctly excluded from the file list, but nothing *returns* them. An
installer needs `.post-install` to configure a package, so the file list being
right is not the same as the package being installable. That is a known hole,
recorded in `afpkg.c` beside the function that creates it.

---

## 2026 — Entries from the Alpine work

### The trailer was eight bytes before the end of the wrong thing

**Milestone:** v0.9a
**Symptom:** the first Alpine package ever fed to the reader was refused:

```
FAIL  a generated .apk reads successfully
  reason: gzip trailer ISIZE does not match the decompressed length — the stream is corrupt
```
on a package that `gzip -t` and Python's `gzip` both read perfectly.
**Cause:** `afpkg_gunzip_member` located the gzip trailer eight bytes before the
end of the BUFFER it was handed:

```c
const af_u8 *trailer = data + at + (len - at - 8);
```

That is correct only when the buffer contains exactly one member. It was true for
every .deb — each of a .deb's compressed parts is its own ar member, handed over
as its own buffer — and it is false for the first Alpine package tried, because
an Alpine .apk is several gzip members CONCATENATED in one file with no outer
container at all. The reader looked for the first member's trailer at the end of
the file, found bytes belonging to the second member, and reported a CRC/length
mismatch on a perfectly good package.
**Fix:** stop calculating where the deflate stream ended and be told.
`afpkg_inflate` now reports the input bytes it consumed — `(bits_used + 7) / 8`,
because the bit reader loads bytes eagerly and `br.byte` counts bytes *read*, not
bits *consumed*. The trailer follows that position, and `consumed` is what lets
the caller land on the next member's magic.
**Found by:** a fixture built specifically to have two members. Every .deb test
passed before and after, because a .deb cannot express this case — its members
come from an index rather than from concatenation. The format difference IS the
test.
**Lesson:** a function that infers a boundary from the end of its buffer is
assuming its buffer ends where the structure ends. That assumption is invisible
while every caller happens to satisfy it, and it is exactly what a new container
shape breaks. The fix is always the same shape: the layer that knows where
something ended should say so, rather than the next layer up working it out from
the total length.

---

### Two functions that both parse "a package description"

**Milestone:** v0.9a (a design note, written while adding the Alpine reader)
**Symptom:** none — this is the decision that avoided one.
**Cause:** Debian's control file and Alpine's `.PKGINFO` look alike. Both are
plain text. Both are a list of fields. The obvious move is one parser with a
couple of flags.

They are not alike where it matters. Debian is `Field: value` with a single space
after the colon, continuation lines indented by one space, and a blank line
terminating a paragraph. Alpine is `key = value` with spaces around the equals,
no continuations, no paragraphs, and `#` comments. Alpine also writes one
`depend` line per dependency rather than a comma-separated list — so a shared
parser that split on commas would turn `so:libc.musl-x86_64.so.1` into two pieces
of nonsense and lose the dependency silently.
**Decision:** two parsers, each about forty lines, each readable against its own
format.
**Lesson:** "these two formats are basically the same" is a claim about their
appearance, and the differences are always in the parts that carry meaning —
delimiters, continuations, how lists are represented. A flag-driven parser for
two formats is a parser that is subtly wrong for one of them, and the wrongness
lands in the data rather than in an error.

---

## 2026 — Entries from the DEFLATE work

### The fixtures were not DEFLATE

**Milestone:** v0.9a
**Symptom:** all sixty stored-block test vectors failed, every one with the same
message:

```
FAIL  random level 0: refused — stored block LEN and NLEN are not ones-complements
```
**Cause:** the bug was in the FIXTURE GENERATOR, not the decompressor.

`zlib.compress(data, level)` does not return a DEFLATE stream. It returns a ZLIB
stream — a 2-byte header and a 4-byte Adler-32 trailer wrapped around the DEFLATE
data. The generator wrapped that in a gzip header as though it were raw DEFLATE,
so the decoder was handed a stream beginning `78 01` and correctly interpreted
`0x78` as a block header: BFINAL=0, BTYPE=00, a stored block — then read the ZLIB
header's remaining byte and the start of the actual data as LEN and NLEN, which
are not ones-complements of each other. It refused, correctly, for a reason that
pointed at the data rather than at the generator.

`zlib.compressobj(level, zlib.DEFLATED, -15)` produces raw DEFLATE. The `-15` is
the whole difference and it is easy to leave out.
**Fix:** the generator uses `compressobj` with `wbits=-15`, and the file now says
why. The decoder was correct throughout and was not changed.
**Found by:** the byte-exact comparison. Every vector failed with an error rather
than producing wrong output, which is the good case — but the reason named the
LEN field, which is four bytes into a stored block, and the first instinct was to
look at the stored-block code. Adding up what the decoder believed it was reading
eventually pointed back at what the generator had written.
**Lesson:** a test that compares against a fixture is testing two things and can
only tell you that they disagree. The fixture is as much a part of the system as
the code, and it deserves the same suspicion — especially when it was written by
the same person in the same hour. The specific trap here is worth remembering:
`zlib.compress` is ZLIB, not DEFLATE, and every `-15` in the documentation exists
because of it.

---

### Two sections of one test shared a variable

**Milestone:** v0.9a
**Symptom:** after the DEFLATE work landed, two unrelated-looking checks failed:

```
FAIL  a Huffman-coded gzip stream is refused with AF_ERR_NOTSUP
FAIL  the control tar holds two entries
```
**Cause:** two separate mistakes.

The first was an OBsolete assertion. That test asserted the absence of a feature
— "Huffman-coded blocks are refused" — which was true and correct when written
and became false the moment the feature arrived. A test written against a
limitation has to be revisited when the limitation is lifted, and nothing
prompts that revisiting except the test failing, which is at least honest.

The second was state clobbering. `test_layers` used one variable, `inflated`, for
the control archive's decompressed length AND for the length produced by two
throwaway streams further down. The obsolete Huffman test overwrote it, so the
tar walk that ran afterwards was handed a length of zero and counted no entries.
The two failures looked unrelated and were one cause.
**Fix:** the throwaway streams write to their own variable, and the obsolete
assertion is replaced by a real one — a corrupt Huffman stream must be REJECTED
rather than decoded into plausible nonsense, which is the property that still
needs defending.
**Found by:** the two failures appearing together, and the realisation that the
second could not possibly be affected by the first unless something was shared.
**Lesson:** a test function with mutable state used across sections is a test
where changing one section can break another, and the failure will point at the
section that ran last. The fix is not to be careful; it is to give each section
its own variables, so the failure points at the line that caused it. And: when a
test asserts that something is NOT supported, it has an expiry date.

---

## 2026 — Entries from the IPC work

### The message structure did not add up to the size it was documented at

**Milestone:** v0.6
**Symptom:** the build failed with a static assertion, on the first compile of
the IPC header:

```
error: static assertion failed: "af_msg_t must stay exactly 64 bytes — the
message IS the register-passing wire format, and growing it silently moves
every IPC onto the slow path"
```
**Cause:** the protocol document has declared `af_msg_t` to be 64 bytes since it
was first written, and its field list sums to 80:

```
label        af_u64                8
cap_count    af_u32                4
_pad         af_u32                4
caps[4]      af_u64               32
words[4]     af_u64               32
                                  -- 
                                  80
```

The document contradicted itself in the same code block, and the contradiction
survived three milestones of review because nobody adds up the fields of a struct
they are reading. It was found within a second of the structure being *implemented*
and asserted.
**Fix:** not to move the target. 64 is the number of argument bytes ARM64 makes
available in registers (`x0`–`x7`), and it is the threshold the fast path is
designed around — 80 would be a 25% overhead on every message for no gain. The
fix is that a capability handle is `af_u32`: an index with a generation packed
into it, per `cap.h`. Storing one as `u64` both wasted four bytes each and
contradicted the capability model the same document describes.
**Found by:** `AF_STATIC_ASSERT(sizeof(af_msg_t) == AF_MSG_SIZE, ...)` — written
because the size IS the design, and it is the kind of invariant that is cheap to
assert and expensive to notice.
**Lesson:** a documented invariant that nothing checks is a comment. The size of
this structure is load-bearing — it decides whether every IPC in the system is one
register copy or a memory round trip — and it lived in prose for three milestones.
The general form: when a number in a document is a *constraint* rather than a
*description*, put it in the code where it can fail a build.

---

### An assertion that could not fail, wrapping a call that would hang

**Milestone:** v0.6
**Symptom:** none — it would have been a boot that stopped partway through with no
error, and I found it by reading my own test before running it.
**Cause:** the first version of the IPC self test contained

```c
check(ipc_recv(ep, &in) == AF_ERR_AGAIN || true,
      "a receive on an empty endpoint with no scheduler context is refused");
```

Two mistakes in one line.

`|| true` makes the condition unconditionally true, so the assertion cannot fail
and tests nothing. And the CALL still runs: `ipc_recv` on an empty endpoint from a
thread that is not the idle thread **blocks**, parking the boot thread forever on
an endpoint nothing would ever send to. The boot would have stopped after
`AF_SCHED_OK` with no panic and no marker — and the last thing printed would have
been a scheduler line, so the investigation would have started in the wrong
subsystem entirely.
**Fix:** removed. The empty-endpoint case is tested where it can actually be
tested — with a real thread, a real sender, and a scheduler — which is the next
section of the same file.
**Found by:** reading the test before running it, because the `|| true` looked
wrong. A test that cannot fail is a smell that leads to the rest.
**Lesson:** `|| true` in an assertion is never a workaround, it is a deletion. And
what remains after deleting the assertion is still executed — so the line was not
harmless, it was an assertion removed *and* a hang left in place. The two mistakes
tend to arrive together, because the `|| true` is usually added to silence a
failure that is trying to report the second one.

---

## 2026 — Entries from the capability work

### The test tried to derive a right the parent did not have

**Milestone:** v0.6
**Symptom:** four failures in the new capability self test, all downstream of one:

```
ERROR cap:   FAIL  a revocable child was derived
ERROR cap:   FAIL  a grandchild was derived
ERROR cap:   FAIL  the chain took two slots
ERROR cap:   FAIL  revoke with the right succeeds
```
**Cause:** the test built its revocation chain by deriving
`AF_RIGHT_READ | AF_RIGHT_REVOKE` from a capability that held only
`AF_RIGHT_READ | AF_RIGHT_WRITE`. `cap_derive` refused, because REVOKE is not a
subset of what the parent held — which is Rule 1, and the entire reason the
function has a subset test.

The code was right and the test was wrong. To test revocation at all the chain
has to start from a capability that *holds* the revoke right, because derivation
cannot confer one.
**Fix:** the chain is built from a separate `root` capability issued with
`READ | REVOKE`, and `cap_a` deliberately keeps its revoke-free rights so the
refusal case can still be tested against it. Both cases now exist in the file,
which is strictly better than before: the broken version tested transitivity and
not the interaction between the two rules.
**Found by:** the positive assertion, and only because it was positive. A test
that checked `cap_derive` returned *something* without checking what would have
passed on a stubbed implementation returning a constant.
**Lesson:** when a test for feature X fails, the first question is whether the
test is exercising X the way X can actually be exercised. Here the failure was
the implementation correctly enforcing a *different* rule the test had not
considered — so the test was not merely wrong, it was incomplete, and the fix
added coverage rather than removing an assertion.

---

## 2026 — Entries from the process work

### Two pieces of code each assumed they owned the same frame

**Milestone:** v0.6
**Symptom:** the new process self test panicked on its own teardown:

```
INFO  proc : reaping pid 3 'test-b': releasing address space 0x1E69000
ERROR pmm  : double free of frame 0x1E76000
AFRIYIEOS KERNEL PANIC
  reason : assertion failed: false — double free of frame 0x1E76000
```
**Cause:** `hal_pt_destroy` releases the frames mapped into the address space it
destroys — that is what "the process owns its memory" means, and it is why
`process_reap` can be one call. The test had allocated `frame_b` with
`pmm_alloc_frame_z`, mapped it into process B, and then freed it again after
reaping B, on the reasoning that the test had allocated it and therefore owned it.

It had allocated it. It did not still own it. Ownership transferred at
`hal_map_page`, and the test was the second of two owners.
**Fix:** the test no longer frees the frame, and the contract is written down in
`hal.h` next to `hal_pt_destroy` — where it should have been before anything
relied on it.
**Found by:** the PMM's double-free assert. Worth noting which direction failed:
this one *did* fail loudly. The reverse mistake — freeing a frame that is still
mapped — leaves a live translation pointing at memory the allocator has handed to
somebody else, and does not fail until something unrelated corrupts weeks later.
**Lesson:** an ownership transfer that is not written down is not a contract, it
is a coincidence that happens to hold while one piece of code is the only caller.
The moment a second caller exists, one of them is wrong and neither can tell.

---

### Shared memory is impossible today, and the reason is one line

**Milestone:** v0.6 (found while writing the ownership note above, not by a crash)
**Symptom:** none yet. It would be: process A maps frame F, process B maps frame
F, A exits, F is freed while B is still using it, B exits, double-free assert.
**Cause:** `hal_pt_destroy` **frees** every user leaf frame rather than
**unreferencing** it. With one owner that is exactly right. With two it is wrong
in the worst way, because the first destroy looks like normal operation and the
failure lands on the innocent second process.
**Fix:** not applied here — deliberately recorded instead. `pmm_frame_ref` and
`pmm_frame_unref` already exist, so the change is to unref a leaf instead of
freeing it and to ref before mapping a second time. It is *not* done in this
milestone because it changes frame ownership for every existing caller at the
same time as the process object is being introduced, and two ownership changes in
one step is how a memory bug becomes unattributable.
**Found by:** writing the note that explains why the test was wrong. Asking "what
else does this contract imply?" produced the answer in about a minute, and the
answer was a limitation on the critical path.
**Lesson:** the useful question after fixing an ownership bug is not "is the fix
correct?" but "what else does this rule make impossible?" The rule here —
destroy frees what is mapped — makes sharing impossible, and sharing is how IPC
avoids copying every message through the kernel. It is now written in `hal.h` and
in the compatibility plan rather than waiting to be discovered by the first
person to try a shared buffer.

---

### A process that exited and was never released

**Milestone:** v0.6
**Symptom:** init ran, exited with code 0, its thread was reaped — and its
process stayed in the table forever, holding a pid and an address space. Nothing
failed. The boot test passed, every marker was present, and no assertion fired.
**Cause:** the system call exit path did not go through `process_exit`. It marked
the thread a zombie by hand and called `thread_exit()` directly:

```c
if (s_user_thread != NULL) {
    s_user_thread->state = AF_THREAD_ZOMBIE;   /* terminates the THREAD */
}
thread_exit();
```

That terminates the thread and says nothing about the process. The process stayed
`AF_PROCESS_ALIVE`, so the reaper's condition — reap when the thread count is
zero AND the state is zombie — never became true.
**Fix:** `sys_exit` calls `process_exit(code)`, which sets the exit code, marks
the process a zombie, wakes a waiting parent, and then terminates the thread.
`process_exit` already handled a thread with no process, so nothing needed
checking at the call site. The now-dead `s_user_thread` global was removed with
it — state that is written and never read looks like it means something.
**Found by:** reading the boot log for a line that *should* have been there.
Every other process logged "reaping pid N"; init's did not. Nothing was missing
from the list of things the test checks, which is exactly why this is worth
recording: a leak that passes every assertion is found by reading the evidence,
not by running the tests.
**Lesson:** the manual zombie-marking was there before processes existed, and it
was correct then because there was nothing else to terminate. When the process
object arrived, that line became a second, weaker implementation of what
`process_exit` does — and the weaker one was on the path that mattered. When a
new abstraction subsumes an old line of code, the old line has to be found and
removed, and the way to find it is to grep for the state it manipulates rather
than to trust that the new path is the only one.

---

## 2026 — Entries from the universal-compatibility work

### The detector knew a file was runnable when it was not

**Milestone:** v0.6
**Symptom:** the new format-detection self test failed on its own API:

```
ERROR binfmt: a relocatable object is not executable: expected ELF, got ELF
```

Expected ELF, got ELF. The assertion was
`!af_binfmt_is_executable(info.format)` against a relocatable object, and
`af_binfmt_is_executable(AF_BINFMT_ELF)` answered yes.
**Cause:** two different questions shared one function name.

```
af_binfmt_is_executable(format)   is this FORMAT one a loader can enter?
                                  (yes — ELF is)
"can I run this FILE?"            (no — a relocatable object is not runnable)
```

The detector had correctly set `AF_BINFMT_F_NOEXEC` on the relocatable object.
The predicate ignored it, because it only ever looked at the format. So the
information was right, the flag was right, and the answer a caller would act on
was wrong.
**Fix:** `af_binfmt_can_run(const af_binfmt_info_t *)`, which considers the format,
the flags and whether identification completed. `af_binfmt_is_executable` stays,
now documented as the narrower format-level question.
**Found by:** the positive assertion. A test that checked only "did not crash"
would have passed, and a caller would have handed a relocatable object to the
loader.
**Lesson:** when one function answers two questions that are *almost* the same,
the wrong answer only shows up at the case where they differ — and that case is
usually the one that matters. The tell here was that the detector had already
computed the right answer and put it in a flag nobody consulted.

---

### The test was wrong and the code was right

**Milestone:** v0.6
**Symptom:** three failures, all on the DMG trailer case:

```
ERROR binfmt: DMG by its trailer: expected DMG, got unknown
```
**Cause:** the test built a 1024-byte buffer and wrote `koly` at `512 - 4`. The
UDIF signature is the FIRST field of the last 512-byte trailer block, so it
belongs at offset 512, not 508. The detector looked exactly where the format says
to look, found zeroes, and reported that honestly.
**Fix:** moved the signature four bytes, into the block it was supposed to be
inside.
**Found by:** the failure message naming the detector's own reason — "no known
trailer signature" — which said *where* it looked, not merely that it failed.
**Lesson:** a failing test is a claim about the code and about the test, and the
test is wrong more often than people assume. The reason string on the result
struct is what made that distinguishable from a detector bug in one reading
rather than twenty minutes. Every detector should report where it looked and
what it saw; the cost is a string literal and the payoff is the entire debugging
session.

---

### Two checkers, one of which was stricter than the build

**Milestone:** v0.6
**Symptom:** `compile_check.sh` passed and `link_check.sh` failed on the same
file:

```
FAIL kernel/core/binfmt.c
  error: unused parameter 'data' [-Werror=unused-parameter]
```
**Cause:** `link_check.sh` compiled with `-Wall -Wextra -Werror` and without
`-Wno-unused-parameter`. Both `cmake/flags.cmake` (the real build) and
`compile_check.sh` have it. So the link check was enforcing a policy the project
does not have, and rejecting a kernel that builds.
**Fix:** `link_check.sh` now uses the same warning set as the other two, with the
reason written next to it. The unused parameter was also removed, because it was
a real smell regardless of who was complaining.
**Found by:** running the full suite rather than only the check that had failed
before.
**Lesson:** this is the size-budget bug again, one level down — two copies of one
truth, and the copies disagreeing. `compile_check.sh`'s own header says that if
the flags drift "this tool stops predicting the real build and becomes worse than
useless". The same sentence was true of `link_check.sh` and nobody had written it
there. A checker's value is entirely in agreeing with the thing it predicts.

---

## 2026 — Entries from the CI investigation

### CI had never run the kernel, and one line of Python was why

**Milestone:** found after v0.5, looking at why every run was red
**Symptom:** every push since v0.3 showed a red X. The failing steps were
"Byte-compile the tooling" and "Shell script syntax", and the third job —
"Build and boot (T2/T3)" — was **skipped** on every single run.

```
Host tests, compile and link check (T1): failure
    -> failure   Byte-compile the tooling
Lint: failure
    -> failure   Shell script syntax
Build and boot (T2/T3): skipped
```

**Cause:** five separate problems, stacked so that each one hid the next.

1. **`tools/mkimage.py` did not parse on Python 3.11.**

   ```python
   log(f"     wrote HELLO.TXT ({len(b'Hello from disk\\n')} bytes)")
   ```

   PEP 701 (Python 3.12) lifted the ban on backslashes inside f-string
   expressions. CI pins 3.11; the development machine runs 3.14. It parsed
   locally and failed there with

   ```
   SyntaxError: f-string expression part cannot include a backslash
   ```

   This one line is why the build job never ran. It is the *first* step in the
   *first* job, and a failed job skips everything that `needs:` it.

2. **`shellcheck tools/*.sh` reported 33 findings**, mostly `SC2164` on a bare
   `cd`, and it exits non-zero for notes and infos as well as warnings.

3. **The kernel size step measured a directory.** It ran
   `x86_64-elf-size -A build/x86_64/kernel`, and that path is the CMake target's
   *directory*. The ELF is `kernel.elf`.

4. **The CI budget was 64 KiB while the real budget was 128 KiB.** ADR-011
   raised it at v0.3 in `tools/build.sh`; CI was not updated. The number was
   written out in two places and only one of them changed.

5. **`AF_PREFIX` versus `AF_CROSS_PREFIX`.** The workflow set `AF_PREFIX`; every
   tool reads `AF_CROSS_PREFIX`. So `build_toolchain.sh` installed into
   `$HOME/opt/cross` while the cache saved `$GITHUB_WORKSPACE/opt/cross` — the
   cache could never hit, every run rebuilt binutils and GCC from source, and
   then `PATH` pointed at a directory that was still empty, so the step that
   checked the compiler failed to find the compiler it had just built.

6. **The `.reloc` check read the wrong line.**

   ```
   objdump -h "$EFI" | tail -1 | grep -q '\.reloc'
   ```

   `objdump -h` prints a section as *two* lines when its flag list is long — the
   name/size/offset line, then a continuation line with the flags in it. So
   `tail -1` returns a flags line, never a section name, and the grep could not
   match whichever section happened to be last. The step would have failed on
   every run since it was written, and it never ran, so nobody knew.

7. **The toolchain cache key was stale.** The cache path was
   `$GITHUB_WORKSPACE/opt/cross` while the toolchain installed into
   `$HOME/opt/cross`, so whatever the cache stored was not a toolchain. Once both
   agree on `AF_CROSS_PREFIX`, the old entry under the same key would be restored,
   the build skipped on a reported cache hit, and the next step would fail looking
   for a compiler nothing had unpacked. Bumped to `-v2`.

8. **The boot-bridge check asserted on `file`'s wording**, and `file` describes
   the same image differently depending on its version:

   ```
   dev machine : PE32+ executable for EFI (application), x86-64 ...
   CI runner   : PE32+ executable (EFI application) x86-64 ..., for MS Windows
   ```

   The step grepped for `EFI (application)`, which matches the second and not
   the first. It was written against one machine's `file` and failed on the
   other's. It now reads the PE subsystem field from `objdump -p` and asserts it
   is `0x000a` — `IMAGE_SUBSYSTEM_EFI_APPLICATION`, the actual property that
   makes firmware load the image. `file`'s output is still printed, for humans.

**A PATTERN WORTH NAMING.** Eight faults, and four of them are the same mistake:
a check whose *implementation detail* differs between the development machine
and the runner.

```
1  Python 3.14 accepts f-strings 3.11 rejects
2  shellcheck 0.11 calls it SC2329, shellcheck 0.9 calls it SC2317
3  64 KiB locally, 128 KiB in CI — two copies of one number
8  `file` says "for EFI (application)" here, "(EFI application)" there
```

None of these is a bug in the kernel. All four are checks that *passed locally*
and could not pass remotely, and each one cost a full CI round trip to find. The
common shape: the check was written against an observation of the local
environment rather than against a property of the artifact. The remedy applied
here is the same in three of the four cases — assert a *value*
(`0x000a`, a shared variable, an interpreter's own verdict) rather than a
*description* of one.

**Fix:** all eight.

* `mkimage.py` names the bytes once and uses the name in the f-string.
* `tools/pycompat.py` — a new check that makes this class of failure visible
  locally. See "the first fix for that did not work" below.
* The scratch scripts were dealt with rather than silenced. Twelve of the
  nineteen shell scripts in `tools/` had `/mnt/c/code/acs/AfriyieOS` hardcoded —
  they were development conveniences from this one machine, committed to a
  public repository, and they accounted for most of the lint noise. Three were
  deleted as one-off debugging for finished milestones, two were duplicates of a
  third, and the remaining seven were rewritten as portable tools using
  `REPO_ROOT`. The genuine false positives got a `disable` directive **with the
  reason written next to it**.
* The size budget moved to `tools/budgets.sh`, sourced by both `build.sh` and
  the workflow. Two copies of a number that must agree is a bug with a schedule.
* `ci.yml` uses `AF_CROSS_PREFIX`, the name the tools already used.

**Found by:** asking the API which steps failed instead of guessing from the red
X, then reproducing each step locally. The log made problems 1 and 2 immediate;
problems 3, 4 and 5 were found by reading the build job against the tree it
actually produces, because that job had never once run and there was no log to
read.

**Lesson:** a job that is *skipped* is not a job that passed, and a pipeline
where the first step fails every time is a pipeline with no coverage at all —
the red X was accurate and had stopped carrying information. Two narrower
lessons, both of which cost real time here:

* **Fix the first failure and look again.** Problems 3, 4 and 5 were all behind
  problem 1. The instinct after fixing it is to push and see green; the correct
  move is to assume the next hidden failure exists, and go looking for it.
* **A version split between the development machine and CI is a class of bug,
  not an instance.** `compileall` cannot catch it, because compiling with a newer
  interpreter accepts everything the newer interpreter accepts.

---

### The first version of that guard did not work

**Milestone:** same investigation
**Symptom:** none — the guard reported success on a file containing the exact
line it was written to reject.
**Cause:** the first `tools/pycompat.py` used
`ast.parse(source, feature_version=(3, 11))`, on the reasonable assumption that
asking the parser for an older grammar would reject newer syntax.

It does not. `feature_version` reverts grammar *productions*; it does not revert
the *tokenizer*, and PEP 701 was a tokenizer change. Fed a file with a backslash
inside an f-string expression, it reported zero problems.

**Fix:** a real interpreter at the target version when one is available
(`python3.11`, or `AF_PY_MIN`), which is the complete check and is what CI gets
for free; plus, on 3.12+, a targeted tokenizer scan for backslashes inside
f-string replacement fields, which is where the FSTRING_START / FSTRING_MIDDLE /
FSTRING_END tokens make the literal parts distinguishable from the expressions.
The script now prints which of the two checks it was able to run, so a partial
run is visibly partial.
**Found by:** running the guard against a file containing the original bad line
before believing it. It said "0 rejected".
**Lesson:** a check that cannot fail on the bug it was written for is worse than
no check, because it is trusted. Every guard needs a positive control — the
failure case, run once, confirmed to fail — and "I tested it" has to mean that
and not "I ran it and it passed", which is the same result a broken check gives
you.

---

## 2025 — Entries from v0.5 development

### The USER bit cannot be used to decide who owns a page table

**Milestone:** v0.5 (found while designing the process address space, before the
first version of it ran)
**Symptom:** would have been a triple fault — no output at all — on the first
instruction fetch after installing a process's page tables.
**Cause:** the first design for "make a process address space" was to *clone the
kernel's* and decide ownership entry by entry, keeping what the kernel owned and
dropping what the old address space owned. The natural rule was the one
`hal_pt_destroy` already used: an entry with the USER bit belongs to the address
space, one without it belongs to the kernel.

That rule is wrong above the leaf level, and it is wrong in both directions:

```
  PML4[0] holds the kernel's identity map (0..3 GiB, no USER)
  PML4[0] also holds user space at 4 GiB (USER set, and the walk sets it
          on EVERY level on the path — so PML4[0] itself gains USER)
```

One entry, then, with two owners. A clone that skipped USER entries would have
dropped the entire kernel identity map, and the process would have been created
successfully and then faulted on its next instruction. A clone that kept them
would have shared the tables, and the process's first user page would have
appeared in the kernel's address space and in every other process's.

Deciding at the leaf level instead — "a leaf frame is the address space's iff it
carries USER" — is correct, and it broke `hal_pt_destroy`, which frees frames at
the PDPT and PD levels without consulting USER at all. Under the new scheme it
freed the kernel's own identity-map frames, and the boot died with

```
WARN  pmm : pmm_free_frame(0x0) ignored: not a managed frame
ERROR pmm : double free of frame 0x2000000
```

**Fix:** stop trying to decide ownership at all. Give user space its own top-level
slot — `AF_USER_PML4_INDEX`, PML4[1], 512 GiB — and make every other top-level
entry the kernel's unconditionally. `hal_pt_create_user` becomes nine lines: copy
every PML4 entry except that one, sharing the kernel's tables **by pointer**. No
copying, no recursion, no ownership rule. `hal_pt_destroy`'s existing rule — skip
non-USER PML4 entries, they are shared kernel tables — was already exactly right
and needed no change; it becomes correct again the moment PML4[0] stops carrying
USER.
**Found by:** a new test that maps a page at 4 GiB into the new address space and
asserts it does not appear in the kernel's. It fired, correctly, and the message
named the real problem: at 4 GiB two address spaces genuinely do share a
top-level entry. Moving the probe into the user region made it pass, and the fact
that moving the *address* was the fix is what showed the address layout was the
thing to change rather than the ownership logic.
**Lesson:** the USER bit is a property of a *page*, propagated up the walk
because the hardware requires it — not a label saying "this table belongs to the
user". Any code that reads it as ownership above the leaf level is reading it
wrong. The design fix was to make ownership structural (a reserved top-level slot)
rather than inferred, because an inferred ownership rule has to be agreed on by
every function that walks the tree, and the ones that disagree produce failures
with no output and no address.

---

### The kernel's address space is not the process's

**Milestone:** v0.5
**Symptom:** the loader refused init's first segment with `ERR_EXIST`:

```
INFO  elf : loading '/INIT.ELF': 10712 bytes read
ERROR vmm : 0x8000000000 is already mapped to 0x1E5A000; refusing to remap to 0x1E6F000
ERROR elf : segment 0: could not map 0x8000000000 (ERR_EXIST)
```
**Cause:** the ring-3 self test's stub and init were both loaded into the
*kernel's* address space — `elf_exec` used `hal_get_page_table()` — so they claimed
the same virtual address. This is the same collision that had been "fixed" twice
before by moving the stub somewhere else: 4 GiB, then 8 GiB, then 512 GiB. Each
move worked, and each one was treating the symptom. The actual statement being
made by the collision was **there is no address space per program**, and moving
addresses does not change that.
**Fix:** `hal_pt_create_user()` gives the program its own root; the thread carries
it in a new `addr_space` field; `switch_to` installs it on every context switch,
so the address space is a property of the running thread rather than of whatever
ran last. `elf_exec` attaches the space and switches to it *before* loading,
because the loader maps into the current address space. When the thread exits, the
zombie is reaped on the kernel's root and the space is destroyed with it.

The scheduler installs the root from the thread field rather than each exit path
restoring it by hand: a path that forgot would leave the kernel running on a dead
process's tables, and the failure would appear in some later, unrelated subsystem.
**Found by:** an `ERR_EXIST` naming both the existing frame and the proposed one,
which made the two claimants identifiable by grep. The lesson arrived later: the
same error at a program's *link* address is not a mapping bug, and the third time
it appeared was the point at which it stopped being a coincidence.
**Lesson:** a fix that works by moving a constant is a fix that will be needed
again. Three separate "fixes" here each made the tests pass — by relocating the
stub to 4 GiB, then 8 GiB, then 512 GiB — and none of them addressed the missing
abstraction. The signal was that the same class of error kept recurring with a
different number in it. Also: `hal_pt_create` returning an address space nobody
can run on is a trap. The v0.2 test created one, mapped into it, and checked the
mapping did not leak — all true, and none of it proved the root could be
*installed*. The new test switches to it, which is the only experiment that
settles the question.

---

## 2025 — Entries from v0.4 development

### A range check is not a mapping check

**Milestone:** v0.4 (found by re-reading the validator against the fault path, not
by a crash — the crash it prevents had not happened yet)
**Symptom:** none observed. `sys_debug_write` checked that the buffer's address was
in the user half, that the length did not wrap, and that the end did not cross into
the kernel half — and then dereferenced it. A program passing a pointer into a hole
between two of its own mappings would have had that pointer read by the kernel, in
kernel mode, and taken a page fault with `CR2` pointing at user memory.
**Cause:** two different questions were being conflated.

```
"is this address in the user half?"   — answered by the range check
"does this address exist?"            — not asked
```

Every check in the function was a comparison against constants. None of them
consulted the page tables, which is where the answer to the second question lives.
The code even said so, in a comment claiming this would be fixed "when processes
proper arrive" — which was wrong twice: the page tables were already reachable
through `hal_query_flags`, and the resulting fault would have been the *kernel's*
to answer for, not the process's.
**Fix:** `user_pages_ok` walks every page the range touches and requires each to be
present, carry the `USER` bit, and be writable if the call writes. The walk starts
at the first page and rounds the end outwards, so a range starting near the end of
a mapped page cannot spill into the next one. `AF_MAX_VALIDATED_PAGES` (16 MiB)
bounds it, because the length is attacker-controlled and a page-table lookup per
page is a denial-of-service vector.
**Found by:** reading the validator next to the page-fault handler while writing
the acceptance test for it. The test — six malformed buffers, each of which must
return an error rather than kill the machine — is what turned "this looks right"
into "this is checked".
**Lesson:** a bounds check made only of comparisons against constants cannot know
anything about what is actually mapped. That is not a refinement to add later; it
is a different question, and until it is asked the kernel is dereferencing a
pointer it was told to trust. The general form: whenever a value from user space is
checked, ask whether the check is about the *value* or about the *memory*, and make
sure it is the second one before the dereference.

---

### The boot test was reading a buffer, not a log

**Milestone:** v0.4
**Symptom:** `AF_EXEC_RAN` was reported `MISS` while the failure report printed, a
few lines further down, the kernel output that contained it:

```
  ok   AF_EXEC_PREPARED
  MISS AF_EXEC_RAN
...
SERIAL OUTPUT
...
init:   6 hostile pointers refused with an error, no kernel fault
initAF_EXEC_RAN
init: all checks passed, exiting
```
The test ran for its full 90-second timeout and never saw a marker that was on disk
by the time it gave up.
**Cause:** the runner used `-serial file:PATH` and polled that file. QEMU writes
the file through a **buffered `FILE`**, and the final partial buffer stays in
QEMU's own memory until the process exits cleanly. `process.terminate()` followed
by `kill()` discards it.

The lost bytes are always the *tail* — which is exactly where the last markers are.
Nothing the kernel printed after `AF_EXEC_RAN` pushed the buffer past a 4 KiB
boundary, so the marker sat in the unflushed remainder on every run. It survived on
earlier runs only because more output followed it; adding six syscall warning lines
above it moved the boundary and the marker fell back into the gap.
**Fix:** test mode now uses `-serial stdio` and reads the pipe on a dedicated
thread, accumulating in memory and writing the log file as a copy for humans rather
than as the source of truth. A pipe has no such buffer: QEMU writes to the
descriptor, the reader drains it continuously, and nothing is lost when the process
is killed. After the process is reaped the reader is joined and the accumulated
text drained before the verdict is computed.
**Found by:** the contradiction itself — a `MISS` printed immediately above its own
counter-evidence. The first hypothesis was a marker split across a line boundary
(the output really did read `initAF_EXEC_RAN`, a missing newline in the test
program, fixed separately); the second was that the log tail was truncated
mid-word, which `file:` buffering explains and nothing else does.
**Lesson:** when a test's verdict disagrees with the evidence it prints, the
harness is the suspect — and a harness that reads a *file another process is
writing* is reading a copy whose completeness is not guaranteed at any moment. The
log had been truncated mid-word for several runs and it went unnoticed because only
the tail was affected and the tail was usually irrelevant. Two smaller lessons came
out of it: a marker printed by one test must never share a line with another test's
output, and "the marker is missing" should always be checked against "is the
marker's *output* missing" before anything else is suspected.

---

### The ELF loader freed the buffer its header pointer pointed into

**Milestone:** v0.4
**Symptom:** the loader read the file correctly and printed every value correctly —
`entry 0x100000000, 3 program header(s), ET_EXEC`, three segments with the right
addresses, sizes and permissions, a 64 KiB stack — and then the kernel entered
ring 3 **at address 0**:

```
INFO  elf :   entry 0x100000000, 3 program header(s), ET_EXEC
...
INFO  elf : entering '/INIT.ELF' at 0x0 in ring 3
INFO  boot: AF_EXEC_PREPARED

EXCEPTION: #PF page fault
  faulting address (CR2): 0x0000000000000000
  rip: 0x0000000000000000   cs=0x1b  (CPL 3)
```
**Cause:** `elf_load` reads the whole image into one `kmalloc`'d buffer and takes
`const elf64_header_t *header = (const elf64_header_t *)image`. At the end of the
segment loop it calls `kfree(image)` — and only *after* that, on the last line
before returning, does it read `*out_entry = header->e_entry`. `header` points
into freed memory.

This did not fault. The heap handed the same block straight back out, the memory
was still mapped, and the read quietly returned whatever the next allocation had
written there. It returned 0.

**Fix:** copy out everything needed from the header into locals *before* the free —
`entry_point`, `phnum`, `phoff`, `phentsize` — and use those. The comment at the
copy explains why, because the obvious reaction on reading it is that the copies
are redundant.
**Found by:** a panic dump whose `rip`, `cr2`, `rax` and every general register
were zero except `rsp`, which was the ELF stack top. A stack pointer the loader
had just set and an instruction pointer of zero is not a paging bug — the kernel
had jumped to a value that came from the loader, and the only value the loader
produced that was wrong was the entry point. The log line above the fault said
`0x0`, which had been printed one line below a correct `0x100000000`.
**Lesson:** in C, a pointer into a heap buffer and the lifetime of that buffer are
two separate facts the compiler does not connect. A use-after-free that returns
zeros is worse than one that faults: it produces a plausible-looking wrong value
that propagates. When a function returns several fields out of a structure it is
about to free, copy them all at the top — and when a cached value and a
freshly-read one disagree, suspect the *second* read rather than the first.

---

### Two ring-3 contexts cannot share one address space

**Milestone:** v0.4
**Symptom:** the loader rejected init's first segment:

```
ERROR vmm : 0x100000000 is already mapped to 0x1E59000; refusing to remap to 0x1E6D000
ERROR elf : segment 0: could not map 0x100000000 (ERR_EXIST)
AFRIYIEOS KERNEL PANIC
  reason : elf: could not load '/INIT.ELF' (ERR_EXIST)
```
**Cause:** the ring-3 self test builds its throwaway stub at 4 GiB, and user
programs are *linked* at 4 GiB (see `libs/libaf/user.lds`). At v0.4 there is
exactly one address space: `hal_get_page_table()` returns the kernel's own root,
and every user context is mapped into it. The stub's code page was still there, so
init's `.text` mapping collided with it.

The mapping layer was right to refuse. Silently overwriting the stub's page would
have replaced a page another live thread was executing from.
**Fix:** move the self test to 8 GiB, and record in `syscall.c` why — the two
windows are now disjoint, and the comment names the real gap rather than implying
it is closed.
**Found by:** the `ERR_EXIST` came with the address, the existing physical frame
and the proposed one, so the collision was visible without any further
instrumentation. The two owners were identified by grepping for the address.
**Lesson:** this is not a bug in the loader or the mapping layer — both behaved
correctly. It is the first symptom of the missing abstraction: **there is no
address space per process at v0.4.** Giving the stub its own window unblocks the
milestone without hiding anything, but nothing may load two programs until
processes arrive at v0.5 with a page table root per process. Any future "already
mapped" error at a program's link address should be read as this, not as a
mapping bug.

---

### A fixed sleep is not a synchronisation primitive

**Milestone:** v0.4
**Symptom:** after the stub and init were given separate address windows, the boot
reached `AF_EXEC_PREPARED` but `AF_USER_OK` went missing — and the stub's second
`Hello from ring 3!` line never appeared, even though it had appeared in every
previous boot.
**Cause:** the boot thread ran the stub on a separate thread and then did
`sched_sleep(20)` before calling `elf_exec`, on the reasoning that 200 ms is far
longer than the stub needs. It is — *on average*. `sched_sleep(20)` returns on a
tick boundary, and on this boot the stub was still mid-run when the boot thread
woke. `elf_exec` then entered ring 3 from the boot thread and never returned, so
the stub thread's exit system call never ran and `AF_USER_OK` was never emitted.
**Fix:** wait for the actual condition instead of a duration — spin on
`thread_by_tid(stub_tid) != NULL`, calling `sched_collect_zombies()` and
`sched_yield()`, with a bounded iteration count that panics rather than hanging.
`thread_by_tid` alone is not enough: reaping is what removes the entry, and the
idle thread that normally reaps never runs while another thread is spinning, so
the waiter has to do it itself or deadlock.
**Found by:** the missing marker. The two-line payload the stub prints before it
exits is what made the truncation obvious — one line instead of two said the
thread had been cut off mid-run, not that its output had been lost.
**Lesson:** "long enough" is a guess about a duration, and a duration is not a
fact about the system. Anything ordered by a sleep will pass on a fast machine and
fail under load. Wait on state, and bound the wait so that "never happens" is a
panic with a message rather than a silent hang.

## 2025 — Entries from v0.3 development

### The virtqueue's used ring was not page-aligned

**Milestone:** v0.3
**Symptom:** every virtio-blk request timed out while waiting for the used ring.
The device was found, its registers were reachable, and reading its capacity from
device-specific configuration returned **135168 sectors — exactly the size of the
disk image we had booted from.** So the device was present and the config path
worked. Requests were submitted and never completed.
**Cause:** the legacy virtqueue layout does not let the driver choose where the
rings go. The device computes it:

```
descriptor table   at the base, 16 bytes x queue_size
available ring     immediately after it
used ring          at the NEXT PAGE BOUNDARY after the available ring
```

The driver placed the used ring immediately after the available ring with no
padding — a reasonable reading of "adjacent", and what the *modern* interface
allows. The device then wrote its completions to a page-aligned address the driver
was not looking at.
**Fix:** compute `used_offset = align_up(desc_bytes + avail_bytes, 4096)` and
allocate enough frames for it. For a 256-descriptor queue, the layout grows from
5648 bytes to 9222 — the padding is a whole page.
**Found by:** QEMU's own virtio tracing, `-trace enable=virtio_*,file=...`, which
showed the disagreement directly:

```
virtio_set_status        val 1     <- driver: ACKNOWLEDGE
virtio_set_status        val 3     <- driver: ACKNOWLEDGE|DRIVER
virtio_set_status        val 11    <- driver: +FEATURES_OK
virtio_set_status        val 15    <- driver: +DRIVER_OK
virtio_queue_notify      n 0       <- driver notifies
virtio_blk_handle_read   sector 0 nsectors 1
virtio_blk_rw_complete   ret 0
virtio_blk_req_complete  status 0  <- the DEVICE finished successfully
(driver spins on used->index forever)
```

**Lesson:** the trace turned "the device is not responding" into "the device
responded and we are not looking in the right place" — two completely different
problems with completely different fixes. When one side of a protocol works
perfectly and the other times out, the fault is almost always a **disagreement
about where something is**, not about whether it happened. Tracing the wire costs
five minutes and replaces hours of guessing; three earlier attempts at this
driver changed code hopefully in exactly the wrong direction.

---

### `FEATURES_OK` was missing from the status handshake

**Milestone:** v0.3
**Symptom:** the same timeout as above, before the layout was corrected.
**Cause:** the driver set `ACKNOWLEDGE`, then `ACKNOWLEDGE|DRIVER`, negotiated
features, set up the queue, and went straight to `DRIVER_OK`. The `FEATURES_OK`
step was absent.
**Fix:** the bit is now set and then **read back** — the device writes it back
only if it accepts the driver's feature set, so an unconditional write would make
the check meaningless. The device rejecting the features is then reported rather
than turning into a silent hang.
**Found by:** reading the specification's initialisation sequence against the
code, while looking for anything else wrong.
**Lesson:** this was a real omission but it was *not* the cause of the timeout —
the layout was. Both were fixed, and the trace is what distinguished them. Fixing
the first one and re-running would have produced the same timeout and a wrong
conclusion about which fix mattered.

---

### The driver laid the rings out for 128 descriptors when the device offered 256

**Milestone:** v0.3
**Symptom:** the same timeout again.
**Cause:** the driver truncated the queue to 128 to save memory, on the reasoning
that "the driver may use fewer descriptors than the device offers". That is true
of the modern interface and **false** of the legacy one, where the device derives
the ring offsets from the size it published.
**Fix:** use exactly the size the device reports, and refuse the device if it
offers more than the driver supports rather than silently truncating.
**Found by:** the device log line, which prints the offered and used sizes side by
side — the numbers disagreed, and the discrepancy was visible before any tracing.
**Lesson:** *log the numbers on both sides of an interface.* This one was in the
output all along; it took a trace to make it look worth reading. Print what the
device said as well as what you did with it.

---

## 2025 — Entries from v0.2 development

### `context_switch` saved `rsp` eight bytes past the return address

**Milestone:** v0.2
**Symptom:** an invalid-opcode fault with `RIP` pointing **inside `.bss`** — a
control transfer to a data address — a few hundred context switches into the
scheduler acceptance test. Nothing looked wrong before it: both threads had been
created, the timer was ticking, the idle thread was running.
**Cause:** the saved stack pointer was off by one slot. `context_switch` ended
with `ret`, which pops the *incoming* thread's return address, so the saved `rsp`
for the outgoing thread was recorded as `rsp + 8` — skipping its own return
address. On resume, `mov rsp, [ctx+CTX_RSP]` followed by `ret` therefore popped
whatever sat in the slot *above* the return address, and jumped there.

The comment justifying it read: *"The return address is on our stack, and `ret`
below will consume it from the incoming stack. So we must pop it ourselves and
record rsp AFTER the pop, otherwise the resumed thread would return into a stale
frame."* Every clause is individually true and the conclusion is wrong. The
outgoing thread's return address must **stay** on its stack, because that is
exactly what its own `ret` will pop when it is next resumed.
**Fix:** `mov [rdi + CTX_RSP], rsp` — no arithmetic. The convention is the one
`call`/`ret` already uses: `rsp` points *at* the return address. Both cases then
work unchanged, including a brand new thread, whose stack has
`thread_trampoline` sitting in exactly that slot.
**Found by:** `tools/resolve_addr.sh` turning the faulting `RIP` into
`kernel_stack_bottom + 0x3858` — a *data* symbol — which meant a corrupted
control transfer rather than a bad instruction. That ruled out the decoder, the
handler and the code being executed, and left the saved context as the only
remaining suspect.
**Lesson:** this is the bug the blueprint warns about in as many words — *"you
will spend 99% of your time debugging context switch register save errors"* —
and it did not present as a register being wrong. It presented as a jump to
nonsense several hundred switches later, in code with no connection to the
scheduler. Two things made it findable: the panic dump prints every register, and
resolving an address to a symbol costs seconds. A wrong-looking-but-plausible
comment is worth less than a test that runs long enough to expose the failure.

---

### The v0.2 acceptance test did not test what it claimed

**Milestone:** v0.2
**Symptom:** after the context-switch fix, both threads ran to completion but the
test failed with *"only 2 A/B transitions in 400 entries — the threads ran
sequentially"*.
**Cause:** the test was wrong, not the scheduler. One worker yielded once per
iteration; the other was meant to be preempted. But 200 iterations of a trivial
loop complete in **microseconds**, far inside one 10 ms time slice, so the second
worker finished its entire workload before the timer ever had a chance to
interrupt it. The scheduler was behaving correctly; the test had never created the
conditions it claimed to be testing.
**Fix:** two separate tests, each of which actually exercises its mechanism.
Both workers now yield, producing a near-strict alternation that is directly
observable (the run shows 201 switches each). Preemption is tested with a thread
that never yields and never sleeps: if the boot thread runs again while that
thread is going, the CPU *must* have been taken away by the timer. It counted
**13.8 million iterations** while the boot thread slept, which is a proof rather
than a correlation — no cooperative mechanism could produce that result.
**Lesson:** a failing test is a claim about the system, and it can itself be the
thing that is wrong. Before changing the code to satisfy a test, check that the
test sets up the conditions it asserts about. The first diagnosis here —
"the scheduler starved them" — pointed at entirely the wrong subsystem, and
acting on it would have meant debugging working code.

---

### A created thread was never put on a run queue

**Milestone:** v0.2
**Symptom:** *"scheduler test: alpha reached 0 of 200 iterations — the scheduler
starved it"*. Both threads existed; neither ever executed.
**Cause:** `thread_create()` built the thread, allocated its stack, set up its
context — and left it in state `CREATED` forever, because nothing put it on a run
queue. `sched_admit()` did not exist.
**Fix:** an explicit admission step, called by whoever creates the thread,
documented with the failure mode spelled out. `thread_create()` does not admit
itself because it holds the thread-table lock and the scheduler has its own lock
ordering.
**Found by:** the thread dump showing two threads stuck in `CREATED` while the
scheduler reported an empty run queue. `thread_dump_all()` earns its keep here.
**Lesson:** "created" and "runnable" are different states, and the gap between
them is invisible unless something prints it. State machines need a dump.

---

### `sched_yield` held a spinlock across a context switch

**Milestone:** v0.2
**Symptom:** the machine stopped dead with no fault and no output, immediately
after the idle thread started.
**Cause:** `sched_yield()` took a spinlock, called `context_switch()`, and
released the lock afterwards. Thread A takes the lock, switches to B; B calls
`sched_yield()`, tries the same lock, and spins forever waiting for a thread that
is not running. `af_spin_lock` also disables interrupts, so the machine stops
taking ticks too.
**Fix:** no lock across a switch. Interrupts are disabled only around the run-queue
mutation and re-enabled *before* the switch, so the machine is taking ticks again
by the time the new thread runs.
**Lesson:** a lock held across a context switch is meaningless, because its holder
is not on a CPU any more. The only correct protection for a critical section that
ends in a switch is disabling interrupts for the part that touches shared state,
and re-enabling them before the switch.

---

### A brand new thread inherited interrupts disabled

**Milestone:** v0.2
**Symptom:** would have been a system that froze the instant a second thread
started — no ticks, no preemption, no output.
**Cause:** a thread entered from inside the timer interrupt starts with `IF`
cleared, because the interrupt gate clears it. A *resumed* thread is fine: it
returns through its own `iretq`, which restores its flags. A thread that has never
run has no earlier state to restore.
**Fix:** `thread_trampoline` executes `sti` before calling the entry function.
**Lesson:** the two ways a thread begins — resumed versus brand new — differ in
exactly the places where the CPU restores state automatically for one and not the
other. Flags, FPU state and segment registers all fall into this category.

---

### The boot context *was* the idle thread

**Milestone:** v0.2
**Symptom:** the acceptance test spun 2000 times without ever yielding, and
reported that the scheduler had starved both worker threads.
**Cause:** `kmain`'s context was adopted as the idle thread. `sched_sleep()` and
`sched_block()` deliberately refuse to act on the idle thread — blocking it would
deadlock the machine — so every sleep in the boot path silently became a no-op.
**Fix:** the two are separate threads. `sched_init()` now takes both, and the idle
thread is created like any other.
**Lesson:** a safety check that silently does nothing is a trap. The guard was
correct; returning quietly instead of reporting made it invisible. There is a
case for an `AF_ASSERT` there, at least in debug builds.

---

### The PMM freed the kernel's own `.bss`

**Milestone:** v0.2
**Symptom:** a general protection fault inside `pmm_alloc_frames`, reading the
bitmap. The register dump showed `rax = 0x1818181818181818` — a fill pattern the
heap self test had just written — as the value of `s_bitmap`.
**Cause:** the boot bridge reports the size of the **flat binary** it copied, and
`objcopy -O binary` does not emit NOBITS sections. So `kernel_phys_size` covered
`.text`, `.rodata` and `.data`, and silently omitted `.bss` — 111 KiB of real
memory that the kernel was using at that moment. `pmm_init` reserved only the
reported range, the free pass released the rest, and the heap then allocated the
kernel's own `.bss` and filled it with a test pattern.
**Fix:** two deliberately redundant ones. `objcopy` now materialises `.bss` with
`--set-section-flags .bss=alloc,load,contents`, so the reported size is honest;
and `pmm_init` independently reserves the kernel's full extent from the linker's
`__kernel_start`/`__kernel_end` symbols, taking whichever figure is larger.
**Found by:** resolving `0x1068fe` with `nm` to `pmm_alloc_frames+0x85`, then
disassembling to see it was `movzbl (%rax),%edx` — a read through the corrupted
`s_bitmap`.
**Lesson:** the crash was three function calls from the cause, which is how
allocator bugs behave. Two things made it findable at all: the panic dump printed
every register, and `tools/resolve_addr.sh` turned an address into a symbol. Also
worth noting: **two complementary fixes, not one.** Either alone would have
worked; the failure mode is silent corruption, and redundancy is cheap here.

---

### The frame bitmap was sized from the highest address in the map

**Milestone:** v0.2
**Symptom:** none — it booted. But the log said `managing 268435456 frames
(1048576 MiB) with 294912 KiB of metadata` on a machine with 2 GiB of RAM, and
only 448 952 frames were free.
**Cause:** `pmm_init` sized the bitmap from `af_boot_info_max_address()`, the
highest address anywhere in the firmware's memory map. Firmware routinely
describes device and reserved windows far above RAM — QEMU's map reaches **1 TiB**
on a 2 GiB machine — so the bitmap covered 1 TiB of address space that can never
be allocated from. 32 MiB of bitmap plus 256 MiB of refcounts: **288 MiB of
metadata, 14% of the machine**, and initialisation loops walking 268 million
frames.
**Fix:** `af_boot_info_max_usable_address()`, which considers only regions the PMM
may ever allocate from. Metadata fell from 288 MiB to 576 KiB, free memory rose
from 448 952 to 522 509 frames — **287 MiB recovered** — and the largest
contiguous free run went from 1713 MiB to 2001 MiB.
**Found by:** reading the log numbers and noticing that "1 TiB" and "2 GiB" were
in the same line. Nothing failed; the figures were simply absurd, and absurd
figures in a boot log are worth stopping for.
**Lesson:** "it works" is not the same as "it is right". A sizing bug that wastes
14% of RAM and makes every scan 500× longer passes every functional test. The
habit that catches it is reading your own output critically rather than just
checking that it appeared.

---

### A header's own last field was overwritten by the offset used to find it

**Milestone:** v0.2
**Symptom:** `heap self test: large allocation reports 16 usable bytes` for a
100 KiB allocation.
**Cause:** large allocations kept their header at the start of the block and
stored, in the four bytes immediately below the returned pointer, the distance
back to that header — so `kfree` could find it with one subtraction. When the
allocation is not alignment-shifted, that distance *is* `sizeof(header)`, and
those same four bytes are where the header's **last field** lives. Storing the
offset overwrote `requested` with the value 16.
**Fix:** the trailer is now at a fixed offset (`sizeof(large_trailer_t)`) before
the user pointer and records the block base directly. No offset field, no
arithmetic that can collide with the thing it describes.
**Found by:** the self test checking `kmalloc_usable_size` against what was
requested. Both figures were in the failure message, which is what made the
"16" immediately recognisable as `sizeof(header)`.
**Lesson:** writing bookkeeping into the very bytes it is meant to describe is
the kind of mistake that compiles cleanly, reads as clever, and destroys data
later. Prefer a fixed offset to a computed one whenever the fixed one is
available.

---

### `compile_check.sh` used `-fsyntax-only` and missed `-Wunused-function`

**Milestone:** v0.2 (tooling)
**Symptom:** the compile check reported all 18 files clean; the real cross build
then failed on `mark_range_used defined but not used`.
**Cause:** `-fsyntax-only` stops after parsing and semantic analysis and never
runs the whole-translation-unit passes that report unused functions and
variables.
**Fix:** the check now compiles to an object with `-c`, exactly as the build
does. It costs a fraction of a second more and predicts the build instead of
approximating it.
**Found by:** the tool disagreeing with the compiler, which is the only thing
that makes a pre-flight check worth having.
**Lesson:** a check that predicts the build must actually perform the build's
work. An approximation that passes when the build fails trains you to ignore it.

---

## 2025 — Entries from v0.1 development

---


### `EFI_SYSTEM_TABLE` was packed, so `ConOut` was read from the wrong offset

**Milestone:** v0.1
**Symptom:** none yet — caught before it ran. It would have been an instant
triple fault on the boot bridge's very first console write, with no output
whatsoever.
**Cause:** two mistakes compounding. The structure was marked `AF_PACKED` *and*
its asserted member offsets were computed for a packed layout. UEFI firmware is
compiled as ordinary C, so its structures use **natural alignment**: after the
24-byte table header and the `u32 FirmwareRevision` there are four bytes of
padding before `ConsoleInHandle`, which puts `ConOut` at offset **64**, not 60.
**Fix:** `AF_PACKED` removed from every structure the firmware produces — packing
is for *our* formats, which we define byte for byte — and every member offset
asserted individually, with the reasoning written at the top of `boot/uefi/efi.h`.
**Found by:** the static asserts firing on the first real compile. This is the
one bug in this log that cost nothing, and it is the entire justification for
writing offset asserts against a specification instead of trusting a struct.
**Lesson:** "packed" and "matches the firmware" are not the same thing. A struct
that describes someone else's memory layout must be laid out the way *their*
compiler lays it out.

---

### `kernel_entry` was nowhere near the start of the kernel image

**Milestone:** v0.1
**Symptom:** the boot bridge printed its banner, loaded the kernel, exited boot
services and jumped — then the machine hung with no further output at all. The
serial log simply stopped.
**Cause:** the boot bridge copies the flat kernel binary to physical `0x100000`
and jumps there; it does not read the ELF header. But `kernel_entry` was defined
in a plain `section .text`, so the linker interleaved it with the C functions and
placed it at `0x107900`. The CPU began executing whatever C function happened to
land first, with no stack and no arguments.
**Fix:** the entry stub moved to a dedicated `.text.boot` section, which the
linker script places before everything else, so `kernel_entry` *is* offset zero of
the image by construction. `tools/link_check.sh` and `tools/build.sh` now assert
`kernel_entry == 0x100000`.
**Found by:** `nm build/x86_64/kernel.elf`, after the serial log went quiet and
nothing else could report anything — the failure is past `ExitBootServices`, so
there is no firmware left to blame.
**Lesson:** the deadliest failure mode is the one past the point where anything
can report it. Two defences: make the invariant structural rather than
conventional, and add a build-time assertion so violating it fails the build
instead of the boot. The `ENTRY()` directive in the linker script was set
correctly the whole time and did nothing — it sets the ELF header field, which
nothing here reads.

---

### The FAT32 root directory cluster was never marked allocated

**Milestone:** v0.1 (tooling)
**Symptom:** firmware booted and reported "No bootable option or device was
found". The partition table was fine. `mtools` said `Fat problem while decoding
2 0`; the Linux `vfat` driver said `can't read superblock`.
**Cause:** cluster 2 is the root directory, and the cluster allocator starts
handing out clusters from 3 — so nothing ever wrote a FAT entry for cluster 2. It
read as `0`, meaning "free cluster", and every real FAT driver therefore saw the
root directory's chain as unallocated.
**Fix:** `FAT[2] = 0x0FFFFFFF` at format time. The root directory is a one-cluster
chain, so that is end-of-chain.
**Found by:** running the image through `mtools` and `parted` — implementations
that share no code with mine.
**Lesson:** **this is the one our own verifier could not catch.** `verify_image.py`
walks the same FAT the writer maintains and happily followed a chain the driver
never agreed to. A format is only verified when a *different implementation*
reads it. Reading your own output back proves your reader and writer agree, which
is not the same claim, and is exactly the claim you need.

---

### Console output between `GetMemoryMap` and `ExitBootServices` invalidates the map key

**Milestone:** v0.1
**Symptom:** `ExitBootServices rejected the map key; refetching`, four times, then
`FATAL: ExitBootServices failed after 4 attempts. Halting.`
**Cause:** `ExitBootServices` takes the `MapKey` from `GetMemoryMap` and refuses
the call if the map changed since. It changes whenever anything allocates or frees
a page — and printing to the UEFI console allocates, because the console driver
grows its own buffers. The diagnostic line `memmap: N regions`, placed between
the two calls, invalidated the key every single time.
**Fix:** ordering, not retrying. The map is fetched and `ExitBootServices` is
called back to back with nothing in between, and the console is only used on the
retry path, where the map is about to be re-read anyway.
**Found by:** the retry loop exhausting itself, which forced the question "why
does the key keep changing?" rather than "why does the call keep failing?".
**Lesson:** a retry loop can hide a deterministic bug forever. When a retry
succeeds eventually, ask why it needed a retry at all. The honest answer here was
"because my own diagnostic output broke the precondition" — and the loop would
have concealed it indefinitely if it had happened to succeed on the second
attempt.

---

### The boot handoff pointer arrived in the wrong register

**Milestone:** v0.1
**Symptom:** `boot_info invalid at the supplied pointer; trying the backup
location at 0x7000` / `recovered boot_info from the backup location`. The system
booted anyway.
**Cause:** the boot bridge is compiled with the Microsoft x64 ABI, because that is
what UEFI uses for `efi_main`. Under that ABI the first integer argument goes in
`rcx`. The kernel's entry stub is SysV and reads `rdi`. So the kernel received a
stale value and fell back to the fixed copy the bridge mirrors at `0x7000`.
**Fix:** the kernel entry function pointer is declared `__attribute__((sysv_abi))`,
so the compiler puts the argument where the kernel actually looks for it and
aligns the stack the way SysV requires.
**Found by:** the kernel's own log line, which was written precisely because the
register path is a single point of failure.
**Lesson:** the fallback path paid for itself on the first boot. It converted a
silent hang into a working boot *and* a diagnostic that named the exact problem —
and then the log line it printed is what identified the bug. Build the recovery
path and make it talk.

---

### `efi_console_print` used one index for two buffers

**Milestone:** v0.1
**Symptom:** garbled console text: `AryeS010(ed EIbo rde` instead of
`AfriyieOS 0.1.0 (Seed) UEFI boot bridge`.
**Cause:** the loop used a single index for both the ASCII input and the UTF-16
output buffer. Translating LF into CR+LF advances the output by two and the input
by one, so the two positions desynchronise after the first newline and every
subsequent line is assembled from the wrong characters.
**Fix:** separate `src` and `i` indices.
**Found by:** reading the garbled output closely enough to recognise the intended
string underneath it. The characters were all *there*, which ruled out a memory
problem and pointed straight at an indexing bug.
**Lesson:** "garbled but recognisable" almost always means an off-by-N in a
copy loop, not corruption. Corruption loses data; a desynchronised index
rearranges it.

---

### CMake passed the kernel's GCC flags to NASM

**Milestone:** v0.1
**Symptom:** `nasm: fatal: unrecognised output format 'freestanding'`.
**Cause:** the flags lived on an `INTERFACE` library attached to the whole target,
and CMake applies those compile options to every compiler it invokes — including
NASM, which has its own option syntax and understands none of them.
**Fix:** the options are wrapped in a `$<COMPILE_LANGUAGE:C>` generator
expression. NASM gets its object format from `CMAKE_ASM_NASM_OBJECT_FORMAT`, and
the `-I` and `-D` arguments it *does* accept arrive from
`target_include_directories`/`target_compile_definitions`, which already emit
valid NASM spellings.
**Lesson:** a per-target flag set is not a per-language flag set. Wrap anything
language-specific in a generator expression the moment a target contains more
than one language.

---

### Quotes placed *inside* `-I` and `-Wl,-T` arguments

**Milestone:** v0.1
**Symptom:** `ld: cannot open linker script file "/path/to/bootx64.lds": No such
file or directory` — with the quotes visible in the message. And earlier,
`fatal error: afriyie/types.h: No such file or directory` for a path that
certainly existed.
**Cause:** writing `-I"${dir}"` inside a CMake `COMMAND` passes the quote
characters through to the compiler as part of the argument. The compiler driver
then hands the linker a filename beginning with a double quote.
**Fix:** quote the *whole* argument — `"-I${dir}"`, `"-Wl,-T,${path}"` — so CMake
quotes it as needed and the quotes never reach the program.
**Lesson:** the error message showing the quotes is the tell. When a tool reports
a path with visible quote marks in it, the quoting was done at the wrong level.

---

### `embed.asm.in` used a literal path instead of the CMake placeholder

**Milestone:** v0.1
**Symptom:** `incbin: unable to get length of file 'kernel.bin'`.
**Cause:** the template's fallback was written as a literal `"kernel.bin"` rather
than `"@AF_KERNEL_BIN@"`, so `configure_file` had nothing to substitute and NASM
fell back to a relative path that does not exist in the build directory.
**Fix:** use the `@AF_KERNEL_BIN@` placeholder, with a comment saying what breaks
without it.
**Found by:** dumping the generated file and seeing the fallback text where the
path should have been.
**Lesson:** when a template does not work, look at what it generated. The failure
was in the input file, but the error was in the tool that consumed it.

---

## Open questions

### `mov` loaded the stack symbol's contents instead of its address

**Milestone:** v0.1
**Symptom:** would have been an immediate triple fault at the first `call`, with
no output whatsoever — the machine simply resets.
**Cause:** `mov rsp, kernel_stack_top` in NASM loads the 8 bytes **stored at**
the address `kernel_stack_top`. That symbol lives in `.bss`, which the very next
instructions are about to zero, so `rsp` would have become `0`.
**Fix:** `lea rsp, [rel kernel_stack_top]` — take the address, do not read it.
**Found by:** reading the entry stub line by line before ever building it.
**Lesson:** NASM's symbol syntax is address-by-value, not address-by-reference.
Every load of a linker-provided symbol needs a deliberate decision: `mov` to read
memory, `lea` to compute an address. This is the single most common first-boot
bug in x86 kernels.

---

### UEFI pixel format mapping was inverted

**Milestone:** v0.1
**Symptom:** would have produced a red/blue swap on every desktop machine —
a splash screen in the wrong colours, with everything else working perfectly.
**Cause:** UEFI names its pixel formats by **byte order in memory**
(`PixelBlueGreenRedReserved8BitPerColor` means memory bytes B, G, R, X). AfriyieOS
names its formats by the **little-endian integer layout** a 32-bit read produces
(`AF_PIXEL_RGBX8888` means the value `0x00RRGGBB`). These are inverses of each
other, and the bridge mapped them the wrong way round.
**Fix:** the mapping is now
`PixelBlueGreenRed → AF_PIXEL_RGBX8888` and
`PixelRedGreenBlue → AF_PIXEL_BGRX8888`, with the integer layout written out
explicitly in `boot_info.h` so the next reader does not have to rediscover it.
The `PixelBitMask` case now derives the layout from the masks instead of
assuming.
**Found by:** writing the comment that was supposed to justify the code, and
finding it did not.
**Lesson:** when two systems name the same thing from opposite ends, write the
integer layout down next to the enum. "BGR" alone is ambiguous; `0x00BBGGRR` is
not.

---

### FAT32 directory writer could not write a long-name chain at a directory boundary

**Milestone:** v0.1 (tooling)
**Symptom:** `FAT32 error: directory has 2 free slots but 3 entries must be
written`. Reproduced by writing 200 files named `file-NNNN.txt` into a single
directory.
**Cause:** the slot collector stopped as soon as it found the `0x00` terminator,
collected everything from that slot to the end of the cluster, and then stopped
extending the chain. A long file name needs two long-name entries plus the real
entry — three slots. When only two were free, there was nowhere to put the third.
**Fix:** the loop now continues until it has **enough slots**, extending the
cluster chain when it runs out, rather than stopping at the first terminator.
**Found by:** the `test_many_files_in_one_directory` host test. It was written
specifically because a single-file image would never have hit it.
**Lesson:** "stop when the terminator is found" and "stop when there is enough
room" are different conditions, and the dif-ference only appears at a boundary.
Boundary-shaped tests find these; happy-path tests do not.

---

### A 32 MiB EFI System Partition cannot be FAT32

**Milestone:** v0.1 (tooling)
**Symptom:** `volume has only 64 480 clusters; FAT32 requires at least 65 525`.
**Cause:** FAT32 is defined as a range: at least 65 525 clusters and at most
`0x0FFFFFF5`. At one 512-byte sector per cluster — the only sensible choice on a
small volume — 32 MiB does not yield enough cluster-sizes-worth of sectors. No
cluster size rescues it, because a larger cluster only reduces the count further.
**Fix:** the builder raises with an explanation. The minimum practical ESP is
about 33.5 MiB at 512-byte clusters, and the default is 64 MiB. The constraint is
documented on `ESP_SIZE_BYTES` so nobody re-derives it.
**Found by:** the `test_geometry_converges_for_a_range_of_sizes` test.
**Lesson:** FAT32 is not "any small FAT volume". The 65 525-cluster floor is a
hard definitional boundary, and the obvious modern default of 4 KiB clusters
makes a 64 MiB volume *invalid*. Small volumes need small clusters.

---

### GPT header CRC verification always failed on a correct image

**Milestone:** v0.1 (tooling)
**Symptom:** the image builder produced an image, and the verifier rejected it:
`header CRC32 matches (stored 0x85944976, computed 0xF8F58709)`.
**Cause:** the verifier's bug, not the writer's. A GPT header CRC is computed
over the header bytes with the CRC32 field itself **zeroed**. Computing it over
the stored bytes — checksum included — can never match.
**Fix:** `gpt_header_crc()` copies the header, zeroes bytes 16..20 and hashes
that.
**Found by:** suspecting the writer first, then reading the specification for the
CRC procedure rather than assuming it was the same as the entry-array CRC (which
*is* computed over the stored bytes, which is exactly why the mistake was easy to
make).
**Lesson:** when a checksum disagrees, check the *verifier* before the producer.
Two different checksums in the same structure can legitimately have different
procedures.

---

### `.gitignore` swallowed the entire kernel source directory

**Milestone:** v0.1
**Symptom:** `git add kernel/core` reported "The following paths are ignored by
one of your .gitignore files: kernel/core".
**Cause:** the ignore file contained a bare `core`, intended for core dumps. A
`.gitignore` pattern without a slash matches a path component at any depth, so it
matched the directory `kernel/core` too.
**Fix:** the core-dump patterns are now explicit (`core.[0-9]*`, `core.*.dmp`),
with a comment explaining why a bare `core` is dangerous here.
**Found by:** the commit failing loudly, which is at least a good failure mode.
**Lesson:** never use a bare directory name as a `.gitignore` pattern when that
name might plausibly be a source directory. `core`, `build`, `target`, `bin` and
`doc` all bite.

---

### PIC remapping is not optional

**Milestone:** v0.1 (caught by design, recorded because it will be met again)
**Symptom:** would be a double fault on the first timer tick.
**Cause:** at reset the 8259 PICs deliver IRQ0..7 as vectors `0x08`..`0x0F`, which
collide exactly with the CPU exception vectors. The first timer interrupt would
be dispatched as `#DF`.
**Fix:** remap the master to `0x20` and the slave to `0x28` during IDT setup.
**Found by:** reading `idt.c` against the exception table it defines.
**Lesson:** the default hardware state is almost never the state you want. Every
remap in the boot path is load-bearing, and worth a comment saying so.

---

## Resolved during the first real boot

Closed out — kept here because the reasoning is worth not re-deriving.

- **`o64 retf` in `cpu.asm`** — ✅ NASM accepted it. The far return reloads `cs`
  correctly, and the GDT is in place before `kmain` runs.
- **OVMF paths across distributions** — ✅ `/usr/share/OVMF/OVMF_CODE_4M.fd` and
  `OVMF_VARS_4M.fd` exist on Ubuntu 24.04. The 4 MiB variants are tried first,
  with the plain names and the edk2 locations as fallbacks.
- **`mov rsp, kernel_stack_top` versus `lea`** — ✅ `lea rsp, [rel ...]` is
  correct and assembles to a fixed 7-byte instruction. Neither form is the bug
  it looks like: NASM treats a bare label as an address and `[label]` as memory,
  so both were "right", but `lea` has a deterministic size, which is what fixed
  the `label changed during code generation` error in `entry.asm`.
- **OVMF's default GOP mode is landscape** — still open for testing purposes.
  Changing it needs a firmware setting rather than a QEMU flag, so the portrait
  splash branch is unverified on screen. The compositor at v0.6 gives us
  resolution control and closes this properly.

## Open questions

Things suspected but not yet confirmed. Move them up into a real entry once they
bite — or delete them once the code proves them harmless.

- **Higher-half kernel** — not enabled at v0.1. When it is enabled in v0.2, the
  failure mode of getting it wrong is an instant triple fault with no output, so
  the switch will be made in a commit that also adds the paging bootstrap. The
  same build-time assertion that caught the entry-point bug should be extended to
  cover the new link address.
- **The COM1 scratch-register probe** — QEMU implements the 16550 scratch
  register, so the probe passes. Real hardware is not guaranteed to, and a UART
  that fails the probe means *silent* loss of all kernel output. Worth revisiting:
  the probe should probably warn loudly rather than silently disabling the
  console.
- **Interrupts stay masked after the PIC remap** — deliberate at v0.1 because
  there are no handlers, but it means `irq_dispatch`'s hardware-IRQ branch is
  untested. v0.3's virtio-blk driver enables the first real IRQ line.
- **`#DF` has no IST stack** — a double fault during the panic path is still a
  triple fault. Needs the TSS, which v0.4 writes.
- **SMP** — the bridge reports `cpu_count = 1` regardless of what the firmware
  says. Real SMP bring-up is v1.2 work; until then `hal_cpu_count()` reads CPUID
  and could disagree with `boot_info`, which nothing currently checks.
