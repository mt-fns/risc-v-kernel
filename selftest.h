#pragma once
#include "kernel.h"

// Only compiled into kernel-test.elf (built with -DSELFTEST).

// Set by the Makefile so the kernel and `make test` agree on the exact
// string; this fallback only matters if someone builds selftest.c by hand.
#ifndef SELFTEST_PASS_MSG
#define SELFTEST_PASS_MSG "SELFTEST: ALL PASSED"
#endif

// Name of the test currently running, for failure messages.
extern const char *selftest_current;

// Fails the run immediately: prints the test, location and expression text,
// then makes QEMU exit non-zero.
#define TEST_ASSERT(expr)                                                      \
    do {                                                                       \
        if (!(expr)) {                                                         \
            printf("SELFTEST: FAIL %s: %s:%d: TEST_ASSERT(%s)\n",              \
                   selftest_current, __FILE__, __LINE__, #expr);               \
            qemu_exit_fail(1);                                                 \
        }                                                                      \
    } while (0)

struct selftest {
    const char *name;
    void (*fn)(void);
};

__attribute__((noreturn)) void run_selftests(void);
