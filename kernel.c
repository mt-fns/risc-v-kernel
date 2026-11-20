#include "kernel.h"
#include "common.h"
#ifdef SELFTEST
#include "selftest.h"
#endif

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef uint64_t size_t;
typedef uint64_t * pagetable_t;

extern char __bss[], __bss_end[], __stack_top[];
extern char __free_ram[], __free_ram_end[];
extern char __kernel_base[];
extern char __etext[]; // end of kernel text
extern char __trampoline[]; 

// user application raw binary content & raw binary size (including bss region)
extern char _binary_shell_bin_start[], _binary_shell_bin_size[], _binary_shell_bin_end[];

// all processes
struct process processes[PROCS_MAX];

// used for yielding
struct process *idle_process;
struct process *running_process;

struct process *proc_a;
struct process *proc_b;

// virtio & queue initialisations
struct virtio_virtq *blk_request_vq;
struct virtio_blk_req *blk_req;
paddr_t blk_req_paddr;
uint64_t blk_capacity;

struct file files[FILES_MAX];
uint8_t disk[DISK_MAX_SIZE];

// free kernely memory
struct freelist kmem = {0, NULL};

// kernel page table
pagetable_t kernel_pagetable;


// forward declarations
void kernel_return(uint64_t sp);

struct sbiret sbi_call(long arg0, long arg1, long arg2, long arg3, long arg4,
                       long arg5, long fid, long eid) {
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a3 __asm__("a3") = arg3;
    register long a4 __asm__("a4") = arg4;
    register long a5 __asm__("a5") = arg5;
    register long a6 __asm__("a6") = fid;
    register long a7 __asm__("a7") = eid;

    __asm__ __volatile__("ecall"
                         : "=r"(a0), "=r"(a1)
                         : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5),
                           "r"(a6), "r"(a7)
                         : "memory");
    return (struct sbiret){.error = a0, .value = a1};
}


void putchar(char ch) {
    sbi_call(ch, 0, 0, 0, 0, 0, 0, 1);
}

long getchar(void) {
    struct sbiret ret = sbi_call(0, 0, 0, 0, 0, 0, 0, 2);
    return ret.error;
}


// System-level reboot/shutdown. Refer to System Reset Extension (RISC-V Supervisor Binary Interface Specification) 
struct sbiret shutdown(uint32_t reset_type, uint32_t reset_reason) {
    return sbi_call(reset_type, reset_reason, 0, 0, 0, 0, 0, 0x53525354);
}

void qemu_exit_pass(void) {
    *(volatile uint32_t *) QEMU_TEST_ADDR = QEMU_TEST_PASS;
    shutdown(SYS_REST_TYPE_SHUTDOWN, SYS_RESET_REASON_NA);
    while (1) {}
}

void qemu_exit_fail(uint32_t code) {
    *(volatile uint32_t *) QEMU_TEST_ADDR = (code << 16) | QEMU_TEST_FAIL;
    shutdown(SYS_REST_TYPE_SHUTDOWN, SYS_RESET_REASON_FAILURE);
    while (1) {}
}


void kfree(void *pa) {
    struct node *freenode;

    if (!is_aligned(pa, PAGE_SIZE) || (char *)pa < __free_ram || (char *)pa >= __free_ram_end) {
        PANIC("kfree");
    }

    // junk fill for catching dangling references
    memset(pa, 1, PAGE_SIZE);

    freenode = (struct node *)pa;
    freenode->next = kmem.head;
    kmem.head = freenode;
    kmem.nfree++;
}

void freerange(void *pa_start, void *pa_end) {
    char *p;
    p = (char *)align_up(pa_start, PAGE_SIZE);

    for (; p + PAGE_SIZE <= (char *)pa_end; p += PAGE_SIZE) {
        kfree(p);
    }
}

// Add free memory to freelist structure
void kernel_init_mem() {
    freerange(__free_ram, (void *)__free_ram_end);
}

// Allocates a page of memory or return 0 if no pages left in the free list
void *kalloc() {
    struct node *freenode;
    freenode = kmem.head;

    if (freenode) {
        kmem.head = freenode->next;
        // junk filled
        memset((char *)freenode, 0, PAGE_SIZE);
        kmem.nfree--;
        return (void *)freenode;
    }

    return 0;
}

uint64_t knumfree() {
    return kmem.nfree;
}

static inline uint64_t read_satp()
{
  uint64_t x;
  __asm__ __volatile__("csrr %0, satp" : "=r"(x));
  return x;
}

// Returns the address of the PTE that corresponds to vaddr
// if this PTE has not yet been allocated & should_alloc is set, allocates a PTE for the vaddr
// if should_alloc is not set and PTE has not been allocated, return 0
uint64_t walk(pagetable_t pagetable, vaddr_t vaddr, int should_alloc) {
    uint64_t vpn2 = vaddr >> 30; 
    uint64_t *table2 = (uint64_t *) pagetable; 

    // make first layer if it doesnt exit
    if ((table2[vpn2] & PAGE_V) == 0) {
        if (!should_alloc) return 0;
        uint64_t pt_paddr = (uint64_t) kalloc();
        if (!pt_paddr) {
            return 0;
        }

        // allocate pte for second layer page table
        table2[vpn2] = ((pt_paddr / PAGE_SIZE) << 10 | PAGE_V);
    }

    uint64_t vpn1 = ((vaddr << 34) >> 34) >> 21;
    uint64_t *table1 = (uint64_t *) ((table2[vpn2] >> 10) * PAGE_SIZE);

    if ((table1[vpn1] & PAGE_V) == 0) {
        if (!should_alloc) return 0;
        uint64_t pt_paddr = (uint64_t) kalloc();
        if (!pt_paddr) {
            return 0;
        }

        // allocate pte for third layer page table
        table1[vpn1] = ((pt_paddr / PAGE_SIZE) << 10 | PAGE_V);
    }

    uint64_t vpn0 = ((vaddr << 43) >> 43) >> 12;
    uint64_t *table0 = (uint64_t *) (uint64_t) ((table1[vpn1] >> 10) << 12);

    return (uint64_t) &table0[vpn0];
}

// Look up paddr corresponding to vaddr in pagetable
// Returns 0 if not mapped
uint64_t walkaddr(pagetable_t pagetable, vaddr_t vaddr) {
    uint64_t pte_addr = walk(pagetable, vaddr, 0);
    uint64_t pte = *((uint64_t *)pte_addr);


    if (!pte_addr) {
        return 0;
    }
    if ((pte & PAGE_V) == 0) {
        return 0;
    }
    if ((pte & PAGE_V) == 0) {
        return 0;
    }

    paddr_t pa = (((pte) >> 10) << 12);
    return pa;
}

// Creates a first and second layer page table entry mapping vaddr to paddr with flags set in the pte.
// note about the page table: we use a 2-layer page table following RISC-V's Sv39 paging mechanism.
// each page table layer contains the same PTE format (refer to guide for format)
// Returns the newly allocated pte addr on success, 0 on failure
uint64_t map_page(pagetable_t pagetable, vaddr_t vaddr, paddr_t paddr, uint32_t flags) {
    if (!is_aligned(vaddr, PAGE_SIZE)) {
        PANIC("unaligned vaddr %x", vaddr);
    }

    if (!is_aligned(paddr, PAGE_SIZE)) {
        PANIC("unaligned paddr %x", paddr);
    }
    
    // allocate 3 layer page table
    uint64_t *pte_addr = (uint64_t *) walk(pagetable, vaddr, 1);

    // walk has failed allocating a PTE
    if (!pte_addr) {
        return 0;
    }

    *pte_addr = ((paddr >> 12) << 10) | flags | PAGE_V;
    return (uint64_t) pte_addr;
}

// Allocates PTEs for a range of virtual addresses to a range of physical addresses
// Returns 0 on success, -1 if a PTE could not be allocated
int map_pages(pagetable_t pagetable, vaddr_t vaddr, uint64_t size, paddr_t paddr, int perm) {
    if (!is_aligned(vaddr, PAGE_SIZE)) {
        PANIC("unaligned vaddr %x", vaddr);
    }

    if (!is_aligned(paddr, PAGE_SIZE)) {
        PANIC("unaligned paddr %x", paddr);
    }

    if (size == 0) {
        PANIC("mappages: size");
    }

    vaddr_t curr_vaddr = vaddr;
    vaddr_t last_vaddr = curr_vaddr + size;
    paddr_t curr_paddr = paddr;

    while (curr_vaddr < last_vaddr) {
        if (!map_page(pagetable, curr_vaddr, curr_paddr, perm)) {
            return -1;
        }
        curr_vaddr += PAGE_SIZE;
        curr_paddr += PAGE_SIZE;   
    }

    return 0;
}

__attribute__((naked)) void switch_context(uint64_t *prev_sp,
                                           uint64_t *next_sp) {
    __asm__ __volatile__(
        "addi sp, sp, -13 * 8\n"    // reserve space for 13 4-byte registers on the stack
        "sd ra,  0  * 8(sp)\n"
        "sd s0,  1  * 8(sp)\n"
        "sd s1,  2  * 8(sp)\n"
        "sd s2,  3  * 8(sp)\n"
        "sd s3,  4  * 8(sp)\n"
        "sd s4,  5  * 8(sp)\n"
        "sd s5,  6  * 8(sp)\n"
        "sd s6,  7  * 8(sp)\n"
        "sd s7,  8  * 8(sp)\n"
        "sd s8,  9  * 8(sp)\n"
        "sd s9,  10 * 8(sp)\n"
        "sd s10, 11 * 8(sp)\n"
        "sd s11, 12 * 8(sp)\n"

        "sd sp, (a0)\n"         // save modified stack pointer to the previous process's stack pointer (in the process struct)
        "ld sp, (a1)\n"         // switch actual stack pointer to next_sp here

        "ld ra,  0  * 8(sp)\n"  // set ra to this process's entry function (the ret gets called later on)
        "ld s0,  1  * 8(sp)\n"  
        "ld s1,  2  * 8(sp)\n"
        "ld s2,  3  * 8(sp)\n"
        "ld s3,  4  * 8(sp)\n"
        "ld s4,  5  * 8(sp)\n"
        "ld s5,  6  * 8(sp)\n"
        "ld s6,  7  * 8(sp)\n"
        "ld s7,  8  * 8(sp)\n"
        "ld s8,  9  * 8(sp)\n"
        "ld s9,  10 * 8(sp)\n"
        "ld s10, 11 * 8(sp)\n"
        "ld s11, 12 * 8(sp)\n"
        "addi sp, sp, 13 * 8\n"  // we've popped 13 4-byte registers from the stack
        "ret\n"
    );
}

// switch to user mode (set status register + set return pc to the start of user application)
void user_entry(void) {
    // also make sure to switch to the right user page table
    __asm__ __volatile__(
        "csrw sepc, %[sepc]        \n"
        "csrw sstatus, %[sstatus]  \n"
        :
        : [sepc] "r" (USER_BASE),  
        [sstatus] "r" (SSTATUS_SPIE)
    );

    uint64_t satp = SATP_SV39 | ((uint64_t)running_process->page_table / PAGE_SIZE);

    // kernel_return's offset inside trampsec applied to the TRAMPOLINE mapping
    uint64_t kern_return = TRAMPOLINE + ((uint64_t)kernel_return - (uint64_t)__trampoline);

    // note: we're jumping to kernel_return here but we can't just call the kernel_return function directly
    // if we do, this function does not have a mapping in any user pt which will cause faults
    // instead, since we know that kernel return is inside of the TRAMPOLINE page, we use that vaddr
    // TRAMPOLINE is mapped in every user pt as well as the kernel's
    ((void (*)(uint64_t))kern_return)(satp);
}

void yield(void) {
    struct process *runnable_process = idle_process;
    int i;

    for (i = 0; i < PROCS_MAX; i++) {
        // note: pid 0 is the idle process which is not runnable
        if (processes[i].state == PROC_RUNNABLE && processes[i].pid > 0 && processes[i].pid != running_process->pid) {
            runnable_process = &processes[i];
            break;
        }
    }

    // no other process ready to be run
    if (runnable_process == running_process) {
        return;
    }

    /*
        we set the sscractch register to the top of the next running process's stack.
        this is a convention shared by the switch context code and the trap handler code
        such that when a kernel trap occurs, we know to set sp to the top of the 
        current running process's kernel stack (the sp of the interrupted code is not to be trusted)
    */ 
    // __asm__ __volatile__(
    //     "sfence.vma\n"
    //     "csrw satp, %[satp]\n"
    //     "sfence.vma\n"
    //     "csrw sscratch, %[sscratch]\n"
    //     :
    //     : [satp] "r" (SATP_SV39 | ((uint64_t)runnable_process->page_table / PAGE_SIZE)),
    //       [sscratch] "r" ((uint64_t) &runnable_process->stack[sizeof(runnable_process->stack)])
    // );


    uint64_t *next_process_sp = &runnable_process->sp;
    uint64_t *curr_process_sp = &running_process->sp;

    running_process = runnable_process;
    switch_context(curr_process_sp, next_process_sp);
}


void handle_syscall(struct trap_frame *f) {
    switch (f->a7)
    {
    case SYS_PUTCHAR:
        putchar(f->a0);
        break;
    case SYS_GETCHAR:
        while (1) {
            long ch = getchar();
            if (ch >= 0) {
                f->a0 = ch;
                break;
            }

            // yield to another ready process while waiting for user input
            yield();    
        }
        break;
    case SYS_EXIT:
        printf("process %d exited\n", running_process->pid);
        running_process->state = PROC_EXITED;
        yield();
        PANIC("unreachable");
    default:
        PANIC("unexpected syscall number %x\n", f->a3);
        break;
    }
}

uint64_t handle_trap() {
    struct trap_frame *f = running_process->trapframe;

    uint64_t scause = READ_CSR(scause);
    uint64_t stval = READ_CSR(stval);

    // TODO: check this
    f->epc = READ_CSR(sepc);


    uint64_t user_pc = READ_CSR(sepc);

    // trap is a syscall
    if (scause == 8) {
        handle_syscall(f);

        // go to the nex instruction when we return from trap
        user_pc += 4;
        WRITE_CSR(sepc, user_pc);
#ifdef SELFTEST
    } else if (selftest_expect_fault) {
        // a selftest caused this on purpose: record it and skip the 4-byte instruction
        selftest_expect_fault = 0;
        selftest_fault_scause = scause;
        WRITE_CSR(sepc, user_pc + 4);
#endif
    } else {
        PANIC("unexpected trap scause=%x, stval=%x, sepc=%x\n", scause, stval, user_pc);
    }

    // store user satp in a0 as we return to the kernel_entry
    return SATP_SV39 | ((uint64_t)running_process->page_table / PAGE_SIZE);
}

// pc here points to the entry point of the process
struct process *create_process(const void *image, size_t image_size) {
    // Find an unused process control structure.
    struct process *proc = NULL;
    int i;
    for (i = 0; i < PROCS_MAX; i++) {
        if (processes[i].state == PROC_UNUSED) {
            proc = &processes[i];
            break;
        }
    }

    if (!proc)
        PANIC("create process: no free process slots");

    // Stack callee-saved registers. These register values will be restored in
    // the first context switch in switch_context.
    uint64_t *sp = (uint64_t *) &proc->stack[sizeof(proc->stack)];
    *--sp = 0;                      // s11
    *--sp = 0;                      // s10
    *--sp = 0;                      // s9
    *--sp = 0;                      // s8
    *--sp = 0;                      // s7
    *--sp = 0;                      // s6
    *--sp = 0;                      // s5
    *--sp = 0;                      // s4
    *--sp = 0;                      // s3
    *--sp = 0;                      // s2
    *--sp = 0;                      // s1
    *--sp = 0;                      // s0
    *--sp = (uint64_t) user_entry;          // ra

    uint64_t *page_table = (uint64_t *) kalloc();
    if (!page_table)
        PANIC("create process: no memory");

    // map kernel pages

    // paddr_t paddr = (paddr_t) __kernel_base;
    // size_t size = ((uint64_t) __free_ram_end) - ((uint64_t) __kernel_base);
    // printf("size: %x\n", size);
    // if (map_pages(page_table, paddr, size, paddr, PAGE_R | PAGE_W | PAGE_X) < 0) {
    //     PANIC("kernel map pages");
    // }

    // allocate trap frame & set kernel states
    if ((proc->trapframe = (struct trap_frame *)kalloc()) == 0) {
        PANIC("create process: allocating trap frame");
    }

    proc->trapframe->kernel_satp = read_satp();
    // point to the top (base) of the kernel stack
    proc->trapframe->kernel_sp = (uint64_t)&proc->stack[sizeof(proc->stack)];
    proc->trapframe->kernel_trap = (uint64_t)handle_trap;

    // map user pages
    for (uint64_t off = 0; off < image_size; off += PAGE_SIZE) {
        paddr_t page = (paddr_t) kalloc();
        if (!page) 
            PANIC("create process: no memory");
        
        // handle the case where the data to be copied is smaller than the
        // page size
        size_t remaining = image_size - off;
        size_t copy_size = PAGE_SIZE <= remaining ? PAGE_SIZE : remaining;
    
        // Fill and map the page.
        memcpy((void *) page, image + off, copy_size);
        if (!map_page(page_table, USER_BASE + off, page,
                      PAGE_U | PAGE_R | PAGE_W | PAGE_X)) {
            PANIC("create process: user map pages");
        }
    }

    // map trampoline page
    if (map_pages(page_table, TRAMPOLINE, PAGE_SIZE, (uint64_t)__trampoline, PAGE_R | PAGE_X) < 0) {
        PANIC("create process: allocating trampoline frame pt");
    }

    // map trap frame
    if (map_pages(page_table, TRAPFRAME, PAGE_SIZE, (uint64_t)(proc->trapframe), PAGE_R | PAGE_W) < 0) {
        PANIC("create process: allocating trap frame pt");
    }

    proc->pid = i + 1;
    proc->state = PROC_RUNNABLE;
    proc->sp = (uint64_t) sp;
    proc->page_table = page_table;

    return proc;
}

void kernel_enable_paging() {
    kernel_pagetable = kalloc();
    if (!kernel_pagetable) 
        PANIC("kernel_enable_paging: unable to allocate kernel pt");

    
    paddr_t kernel_base = (paddr_t) __kernel_base;
    paddr_t kernel_text_end = (paddr_t) __etext;
    paddr_t kernel_phys_stop = (paddr_t) __free_ram_end;

    // map kernel text executable and read-only
    if (map_pages(kernel_pagetable, kernel_base, (uint64_t)(kernel_text_end - kernel_base), kernel_base, PAGE_R | PAGE_X) < 0)
        PANIC("kernel_enable_paging: unable to map kernel pages");

    // map rest of kernel pages
    if (map_pages(kernel_pagetable, kernel_text_end, (uint64_t)(kernel_phys_stop - kernel_text_end), kernel_text_end, PAGE_R | PAGE_W) < 0)
        PANIC("kernel_enable_paging: unable to map kernel pages");

    // map trampoline page in kernel
    if (map_pages(kernel_pagetable, TRAMPOLINE, PAGE_SIZE, (uint64_t)__trampoline, PAGE_R | PAGE_X) < 0) {
        PANIC("kernel_enable_paging: unable to map kernel trampoline page");
    }


    // QEMU test device, used by qemu_exit_pass/qemu_exit_fail
    if (map_pages(kernel_pagetable, QEMU_TEST_ADDR, PAGE_SIZE, QEMU_TEST_ADDR, PAGE_R | PAGE_W) < 0)
        PANIC("kernel_enable_paging: unable to map qemu test device");

    __asm__ __volatile__(
        "sfence.vma\n"
        "csrw satp, %[satp]\n"
        "sfence.vma\n"
        :
        : [satp] "r" (SATP_SV39 | ((uint64_t)kernel_pagetable / PAGE_SIZE))
    );
}

__attribute__((naked))
__attribute__((aligned(4)))
__attribute__((section("trampsec")))
void kernel_entry(void) {
    __asm__ __volatile__(
        // store a0 in scratch register so we can save it in the trap frame later
        "csrw sscratch, a0\n"
        "li a0, %[trapframe]\n"

        "sd ra,  8 * 0(a0)\n"
        "sd gp,  8 * 1(a0)\n"
        "sd tp,  8 * 2(a0)\n"
        "sd t0,  8 * 3(a0)\n"
        "sd t1,  8 * 4(a0)\n"
        "sd t2,  8 * 5(a0)\n"
        "sd t3,  8 * 6(a0)\n"
        "sd t4,  8 * 7(a0)\n"
        "sd t5,  8 * 8(a0)\n"
        "sd t6,  8 * 9(a0)\n"
        "sd a1,  8 * 11(a0)\n"
        "sd a2,  8 * 12(a0)\n"
        "sd a3,  8 * 13(a0)\n"
        "sd a4,  8 * 14(a0)\n"
        "sd a5,  8 * 15(a0)\n"
        "sd a6,  8 * 16(a0)\n"
        "sd a7,  8 * 17(a0)\n"
        "sd s0,  8 * 18(a0)\n"
        "sd s1,  8 * 19(a0)\n"
        "sd s2,  8 * 20(a0)\n"
        "sd s3,  8 * 21(a0)\n"
        "sd s4,  8 * 22(a0)\n"
        "sd s5,  8 * 23(a0)\n"
        "sd s6,  8 * 24(a0)\n"
        "sd s7,  8 * 25(a0)\n"
        "sd s8,  8 * 26(a0)\n"
        "sd s9,  8 * 27(a0)\n"
        "sd s10, 8 * 28(a0)\n"
        "sd s11, 8 * 29(a0)\n"
        "sd sp, 8 * 30(a0)\n"

        // store original a0 value passed in by the user
        "csrr t0, sscratch\n"
        "sd t0,  8 * 10(a0)\n"

        // load kernel states from trapframe
        "ld sp, 8 * 31(a0)\n" // kernel sp
        "ld t0, 8 * 32(a0)\n" // load kernel trap address
        "ld t1, 8 * 33(a0)\n"// kernel page table address (satp)
        
        // wait for any previous memory operations to complete, so that
        // they use the user page table
        "sfence.vma zero, zero\n"
        
        // switch to kernel page table
        "csrw satp, t1\n"

        // flush now-stale user entries from the TLB
        "sfence.vma zero, zero\n"

        // call handle trap
        "mv a0, sp\n"
        "jalr t0\n"

        // return to user mode once the trap has been handled
        // note: since handle_trap returns the running page's satp, 
        // a0 should have the user satp here which is passed in kernel_return
        "j kernel_return\n"
        :
        : [trapframe] "i" (TRAPFRAME)
    );
}

__attribute__((naked))
__attribute__((aligned(4)))
__attribute__((section("trampsec")))
void kernel_return(uint64_t satp) {
    __asm__ __volatile__(
        // a0 = user satp
        "fence.i\n"

        // switch to user PT
        "sfence.vma zero, zero\n"
        "csrw satp, a0\n"
        "sfence.vma zero, zero\n"

        "li a0, %[trapframe]\n"

        // restore all but a0 from TRAPFRAME
        "ld ra,  8 * 0(a0)\n"
        "ld gp,  8 * 1(a0)\n"
        "ld tp,  8 * 2(a0)\n"
        "ld t0,  8 * 3(a0)\n"
        "ld t1,  8 * 4(a0)\n"
        "ld t2,  8 * 5(a0)\n"
        "ld t3,  8 * 6(a0)\n"
        "ld t4,  8 * 7(a0)\n"
        "ld t5,  8 * 8(a0)\n"
        "ld t6,  8 * 9(a0)\n"
        "ld a1,  8 * 11(a0)\n"
        "ld a2,  8 * 12(a0)\n"
        "ld a3,  8 * 13(a0)\n"
        "ld a4,  8 * 14(a0)\n"
        "ld a5,  8 * 15(a0)\n"
        "ld a6,  8 * 16(a0)\n"
        "ld a7,  8 * 17(a0)\n"
        "ld s0,  8 * 18(a0)\n"
        "ld s1,  8 * 19(a0)\n"
        "ld s2,  8 * 20(a0)\n"
        "ld s3,  8 * 21(a0)\n"
        "ld s4,  8 * 22(a0)\n"
        "ld s5,  8 * 23(a0)\n"
        "ld s6,  8 * 24(a0)\n"
        "ld s7,  8 * 25(a0)\n"
        "ld s8,  8 * 26(a0)\n"
        "ld s9,  8 * 27(a0)\n"
        "ld s10, 8 * 28(a0)\n"
        "ld s11, 8 * 29(a0)\n"
        "ld sp,  8 * 30(a0)\n"

        // restore user a0
        "ld a0, 8 * 10(a0)\n"

        "sret\n"
        :
        : [trapframe] "i" (TRAPFRAME)
    );
}

void proc_a_entry(void) {
    printf("starting process A\n");
    while (1) {
        putchar('A');
        yield();
    }
}

void proc_b_entry(void) {
    printf("starting process B\n");
    while (1) {
        putchar('B');
        yield();
    }
}

void kernel_main(void) {
    memset(__bss, 0, (size_t) __bss_end - (size_t) __bss);
    kernel_init_mem();
    kernel_enable_paging();

    #ifdef SELFTEST
        WRITE_CSR(stvec, TRAMPOLINE + ((uint64_t)kernel_entry - (uint64_t)__trampoline));
        run_selftests();
    #endif

    // point the kernel exception point to the kernel_entry function
    // TODO: what is this maths
    WRITE_CSR(stvec, TRAMPOLINE + ((uint64_t)kernel_entry - (uint64_t)__trampoline));

    idle_process = create_process(NULL, 0);
    idle_process->pid = 0;
    running_process = idle_process;

    create_process(_binary_shell_bin_start, (size_t)_binary_shell_bin_end - (size_t)_binary_shell_bin_start);

    yield();
    PANIC("switched to idle process");
}

__attribute__((section(".text.boot")))
__attribute__((naked))
void boot(void) {
    __asm__ __volatile__ (
        "mv sp, %[stack_top]\n"
        "j kernel_main\n"
        :
        : [stack_top] "r" (__stack_top)
    );
}