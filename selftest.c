#include "selftest.h"
#include "kernel.h"

paddr_t alloc_pages(uint32_t n);

// defined in kernel.c (pagetable_t is uint64_t *)
uint64_t walk(uint64_t *pagetable, vaddr_t vaddr, int should_alloc);
uint64_t walkaddr(uint64_t *pagetable, vaddr_t vaddr);
int map_pages(uint64_t *pagetable, vaddr_t vaddr, uint64_t size, paddr_t paddr, int perm);

extern char __kernel_base[];

const char *selftest_current = "(none)";

volatile int selftest_expect_fault;
volatile uint64_t selftest_fault_scause;

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
    uint64_t before = knumfree();
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

    TEST_ASSERT(knumfree() == before);
}


static void test_map_pages(void) {
    const uint64_t npages = 5;
    const int perm = PAGE_R | PAGE_W;
    const vaddr_t va = 8 * 0x200000 + PAGE_SIZE;
    const paddr_t pa = 0x87654000;

    uint64_t before = knumfree();
    uint64_t *root = (uint64_t *) kalloc();
    TEST_ASSERT(root != 0);

    TEST_ASSERT(map_pages(root, va, npages * PAGE_SIZE, pa, perm) == 0);

    for (uint64_t i = 0; i < npages; i++) {
        vaddr_t v = va + i * PAGE_SIZE;
        paddr_t p = pa + i * PAGE_SIZE;

        TEST_ASSERT(walkaddr(root, v) == p);
        // an offset inside the page resolves to the same physical page
        TEST_ASSERT(walkaddr(root, v + 0x123) == p);

        uint64_t *pte = (uint64_t *) walk(root, v, 0);
        TEST_ASSERT(pte != 0);
        TEST_ASSERT(*pte == (((p >> 12) << 10) | perm | PAGE_V));
    }

    // neighbours on either side of the range must stay unmapped
    TEST_ASSERT(walkaddr(root, va - PAGE_SIZE) == 0);
    TEST_ASSERT(walkaddr(root, va + npages * PAGE_SIZE) == 0);

    TEST_ASSERT(knumfree() == before - 3);

    uint64_t *l1 = (uint64_t *) ((root[va >> 30] >> 10) << 12);
    uint64_t *l0 = (uint64_t *) ((l1[(va >> 21) & 0x1ff] >> 10) << 12);
    kfree(l0);
    kfree(l1);
    kfree(root);
    TEST_ASSERT(knumfree() == before);
}

static void test_kernel_text_not_writable(void) {
    // kernel_entry swaps sp with sscratch, so the trap needs a stack to land on
    static uint8_t trap_stack[4096] __attribute__((aligned(16)));
    WRITE_CSR(sscratch, (uint64_t) &trap_stack[sizeof(trap_stack)]);

    selftest_expect_fault = 1;
    __asm__ __volatile__("sb zero, 0(%0)" :: "r"(__kernel_base) : "memory");

    TEST_ASSERT(selftest_expect_fault == 0);    // the store trapped
    TEST_ASSERT(selftest_fault_scause == 15);   // as a store page fault
}

// Add a test: write a function above, then add one line here.
static const struct selftest selftests[] = {
    {"trivial", test_trivial},
    {"string", test_string},
    {"alloc_pages", test_alloc_pages},
    {"map_pages", test_map_pages},
    {"kernel_text_not_writable", test_kernel_text_not_writable},
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
