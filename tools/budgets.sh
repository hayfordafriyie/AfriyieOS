# SPDX-License-Identifier: MIT
# shellcheck shell=bash

# AfriyieOS — size budgets, in one place
#
# SOURCED, not executed. There is no shebang for that reason: a shebang would
# suggest it can be run, and running it does nothing. The shell directive above
# tells shellcheck what dialect to check it as.
#
# Sourced by tools/build.sh and by .github/workflows/ci.yml. That is the whole
# point of the file existing.
#
# WHY IT EXISTS
# =============
#
# The kernel .text budget used to be written out twice: 131072 in tools/build.sh
# and 65536 in the CI workflow. ADR-011 raised it from 64 KiB to 128 KiB during
# v0.3 and only the local script was updated. CI kept enforcing the old number,
# so a correct kernel would have failed there — and nobody found out, because an
# unrelated failure in an earlier job meant the build-and-boot job never ran.
#
# Two copies of a number that must agree is a bug waiting for a schedule. This is
# the fix: one file, sourced by both.
#
# Raised by ADR-011. The rationale is that the growth is kernel-mode PCI
# enumeration, virtio-blk and boot graphics — code the architecture wants OUT of
# Ring 0. The microkernel core must return to below 64 KiB once drivers and the
# compositor move to user space at v0.6-v0.7, and that return is a stated goal
# rather than a hope.

# Kernel .text, in bytes. Measured by tools/build.sh against kernel.elf.
#
# SC2034 is disabled because these are "unused" only from inside this file: they
# are read by the two things that source it. shellcheck cannot see across files,
# and exporting them instead would leak the values into every child process for
# no reason.
# shellcheck disable=SC2034
AF_TEXT_BUDGET=131072

# The tighter budget the core is expected to return to once drivers leave Ring 0.
# Not enforced yet; recorded so the target is not quietly forgotten.
AF_TEXT_BUDGET_TARGET=65536

# Boot bridge, in bytes. It is a PE32+ UEFI application with no size pressure.
AF_BOOT_BUDGET=524288
