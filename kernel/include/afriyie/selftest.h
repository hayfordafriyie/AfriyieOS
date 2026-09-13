// SPDX-License-Identifier: MIT
// AfriyieOS — in-kernel self test entry point
//
// The tests themselves live in kernel/core/selftest.c. They run on bare metal at
// every debug boot and report over the serial console in a format CI can grep.

#ifndef AFRIYIE_SELFTEST_H
#define AFRIYIE_SELFTEST_H

// Runs every v0.1 self test.
//
// Prints AF_TEST_OK on success, or AF_TEST_FAIL:<name> for each failure and then
// panics: a failing self test means the kernel is already known to be in a bad
// state, and continuing would hide it.
void af_selftest_run_all(void);

#endif // AFRIYIE_SELFTEST_H
