#include "selftest.h"

paddr_t alloc_pages(uint32_t n);

const char *selftest_current = "(none)";

static void test_trivial(void) {
    TEST_ASSERT(1 == 1);
}

static void test_string(void) {
    char buf[16];

    strcpy(buf, "hello");
    TEST_ASSERT(strcmp(buf, "hello") == 0);
    TEST_ASSERT(strcmp("abc", "abd") < 0);
    TEST_ASSERT(strcmp("abd", "abc") > 0);

    memset(buf, 'x', 4);
    TEST_ASSERT(buf[0] == 'x' && buf[3] == 'x' && buf[4] == 'o');

    char dst[6];
    memcpy(dst, "world", 6);
    TEST_ASSERT(strcmp(dst, "world") == 0);
}

static void test_alloc_pages(void) {
    paddr_t a = alloc_pages(1);
    paddr_t b = alloc_pages(2);
    paddr_t c = alloc_pages(1);

    TEST_ASSERT(is_aligned(a, PAGE_SIZE));
    TEST_ASSERT(b == a + PAGE_SIZE);
    TEST_ASSERT(c == b + 2 * PAGE_SIZE);

    uint8_t *p = (uint8_t *) b;
    for (size_t i = 0; i < 2 * PAGE_SIZE; i++)
        TEST_ASSERT(p[i] == 0);
}

// Add a test: write a function above, then add one line here.
static const struct selftest selftests[] = {
    {"trivial", test_trivial},
    {"string", test_string},
    {"alloc_pages", test_alloc_pages},
};

void run_selftests(void) {
    size_t n = sizeof(selftests) / sizeof(selftests[0]);

    for (size_t i = 0; i < n; i++) {
        selftest_current = selftests[i].name;
        printf("SELFTEST: RUN  %s\n", selftest_current);
        selftests[i].fn();
        printf("SELFTEST: OK   %s\n", selftest_current);
    }

    printf("%s (%d tests)\n", SELFTEST_PASS_MSG, n);
    qemu_exit_pass();
}
