#pragma once
#include "kernel.h"


#ifndef SELFTEST_PASS_MSG
#define SELFTEST_PASS_MSG "SELFTEST: ALL PASSED"
#endif

extern const char *selftest_current;

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
