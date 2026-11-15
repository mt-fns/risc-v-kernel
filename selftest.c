#include "selftest.h"
#include "kernel.h"

paddr_t alloc_pages(uint32_t n);

extern char __free_ram[], __free_ram_end[];

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
    paddr_t a = (paddr_t) kalloc();
    paddr_t b = (paddr_t) kalloc();
    paddr_t c = (paddr_t) kalloc();

    TEST_ASSERT(is_aligned(a, PAGE_SIZE));
    TEST_ASSERT(is_aligned(b, PAGE_SIZE));
    TEST_ASSERT(is_aligned(c, PAGE_SIZE));


    uint8_t *p = (uint8_t *) b;

    for (size_t i = 0; i < PAGE_SIZE; i++) {
        TEST_ASSERT(p[i] == 0);
    }

    kfree((void *)a);
    kfree((void *)b);
    kfree((void *)c);

    uint64_t num_pages_on_boot = (((uint64_t) __free_ram_end) - ((uint64_t)__free_ram)) / PAGE_SIZE;

    TEST_ASSERT(knumfree() == num_pages_on_boot);
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
