#include "kernel.h"
#include "common.h"
#ifdef SELFTEST
#include "selftest.h"
#endif

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef uint64_t size_t;

extern char __bss[], __bss_end[], __stack_top[];
extern char __free_ram[], __free_ram_end[];
extern char __kernel_base[];

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

paddr_t alloc_pages(uint64_t n) {
    static paddr_t next_paddr = (paddr_t) __free_ram;
    paddr_t paddr = next_paddr;
    next_paddr += n * PAGE_SIZE;

    if (next_paddr > (paddr_t) __free_ram_end)
        PANIC("out of memory");

    memset((void *) paddr, 0, n * PAGE_SIZE);
    return paddr;
}

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

// code must be non-zero: QEMU exits with it.
void qemu_exit_fail(uint32_t code) {
    *(volatile uint32_t *) QEMU_TEST_ADDR = (code << 16) | QEMU_TEST_FAIL;
    shutdown(SYS_REST_TYPE_SHUTDOWN, SYS_RESET_REASON_FAILURE);
    while (1) {}
}

/*
    creates a first and second layer page table entry mapping vaddr to paddr with flags set in the pte.
    note about the page table: we use a 2-layer page table following RISC-V's Sv32 paging mechanism.
    each page table layer contains the same PTE format (refer to guide for format)
*/ 
void map_page(uint64_t *table2, vaddr_t vaddr, paddr_t paddr, uint32_t flags) {
    if (!is_aligned(vaddr, PAGE_SIZE)) {
        PANIC("unaligned vaddr %x", vaddr);
    }

    if (!is_aligned(paddr, PAGE_SIZE)) {
        PANIC("unaligned paddr %x", paddr);
    }

    uint64_t vpn2 = vaddr >> 30; 

    // make first layer if it doesnt exit
    if ((table2[vpn2] & PAGE_V) == 0) {
        uint64_t pt_paddr = alloc_pages(1);

        // allocate pte for second layer page table
        table2[vpn2] = ((pt_paddr / PAGE_SIZE) << 10 | PAGE_V);
    }

    uint64_t vpn1 = ((vaddr << 34) >> 34) >> 21;
    uint64_t *table1 = (uint64_t *) ((table2[vpn2] >> 10) * PAGE_SIZE);

    if ((table1[vpn1] & PAGE_V) == 0) {
        uint64_t pt_paddr = alloc_pages(1);
        // allocate pte for second layer page table
        table1[vpn1] = ((pt_paddr / PAGE_SIZE) << 10 | PAGE_V);
    }

    // add leaf PTE containing PPN to actual paddr into the page table
    uint64_t vpn0 = ((vaddr << 43) >> 43) >> 12;
    uint64_t *table0 = (uint64_t *) (uint64_t) ((table1[vpn1] >> 10) << 12);
    table0[vpn0] = ((paddr >> 12) << 10) | flags | PAGE_V;
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
__attribute__((naked)) void user_entry(void) {
    __asm__ __volatile__(
        "csrw sepc, %[sepc]        \n"
        "csrw sstatus, %[sstatus]  \n"
        "sret                      \n"
        :
        : [sepc] "r" (USER_BASE),
          [sstatus] "r" (SSTATUS_SPIE)
    );
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
        PANIC("no free process slots");

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

    // Map kernel pages
    uint64_t *page_table = (uint64_t *) alloc_pages(1);
    for (paddr_t paddr = (paddr_t) __kernel_base;
         paddr < (paddr_t) __free_ram_end; paddr += PAGE_SIZE)
        map_page(page_table, paddr, paddr, PAGE_R | PAGE_W | PAGE_X);

    // Map user pages
    for (uint64_t off = 0; off < image_size; off += PAGE_SIZE) {
        paddr_t page = alloc_pages(1);

        // Handle the case where the data to be copied is smaller than the
        // page size.
        size_t remaining = image_size - off;
        size_t copy_size = PAGE_SIZE <= remaining ? PAGE_SIZE : remaining;

        // Fill and map the page.
        memcpy((void *) page, image + off, copy_size);
        map_page(page_table, USER_BASE + off, page,
                 PAGE_U | PAGE_R | PAGE_W | PAGE_X);
    }

    proc->pid = i + 1;
    proc->state = PROC_RUNNABLE;
    proc->sp = (uint64_t) sp;
    proc->page_table = page_table;
    return proc;
}


__attribute__((naked))
__attribute__((aligned(4)))
void kernel_entry(void) {
    __asm__ __volatile__(
        /*
            sscratch will contain the top of the currently running process's kernel stack.
            when a trap happens, we swap sp (from the user) & sscratch (top of process's kernel stack)
        */ 
        "csrrw sp, sscratch, sp\n"

        "addi sp, sp, -4 * 64\n" // Allocate space for the trap_frame struct
        "sd ra,  8 * 0(sp)\n"
        "sd gp,  8 * 1(sp)\n"
        "sd tp,  8 * 2(sp)\n"
        "sd t0,  8 * 3(sp)\n"
        "sd t1,  8 * 4(sp)\n"
        "sd t2,  8 * 5(sp)\n"
        "sd t3,  8 * 6(sp)\n"
        "sd t4,  8 * 7(sp)\n"
        "sd t5,  8 * 8(sp)\n"
        "sd t6,  8 * 9(sp)\n"
        "sd a0,  8 * 10(sp)\n"
        "sd a1,  8 * 11(sp)\n"
        "sd a2,  8 * 12(sp)\n"
        "sd a3,  8 * 13(sp)\n"
        "sd a4,  8 * 14(sp)\n"
        "sd a5,  8 * 15(sp)\n"
        "sd a6,  8 * 16(sp)\n"
        "sd a7,  8 * 17(sp)\n"
        "sd s0,  8 * 18(sp)\n"
        "sd s1,  8 * 19(sp)\n"
        "sd s2,  8 * 20(sp)\n"
        "sd s3,  8 * 21(sp)\n"
        "sd s4,  8 * 22(sp)\n"
        "sd s5,  8 * 23(sp)\n"
        "sd s6,  8 * 24(sp)\n"
        "sd s7,  8 * 25(sp)\n"
        "sd s8,  8 * 26(sp)\n"
        "sd s9,  8 * 27(sp)\n"
        "sd s10, 8 * 28(sp)\n"
        "sd s11, 8 * 29(sp)\n"

        "csrr a0, sscratch\n"
        "sd a0, 8 * 30(sp)\n"

        // store the top of the kernel process stack to top after we push everything
        "addi a0, sp, 8 * 31\n"
        "csrw sscratch, a0\n"

        "mv a0, sp\n"
        "call handle_trap\n"

        "ld ra,  8 * 0(sp)\n"
        "ld gp,  8 * 1(sp)\n"
        "ld tp,  8 * 2(sp)\n"
        "ld t0,  8 * 3(sp)\n"
        "ld t1,  8 * 4(sp)\n"
        "ld t2,  8 * 5(sp)\n"
        "ld t3,  8 * 6(sp)\n"
        "ld t4,  8 * 7(sp)\n"
        "ld t5,  8 * 8(sp)\n"
        "ld t6,  8 * 9(sp)\n"
        "ld a0,  8 * 10(sp)\n"
        "ld a1,  8 * 11(sp)\n"
        "ld a2,  8 * 12(sp)\n"
        "ld a3,  8 * 13(sp)\n"
        "ld a4,  8 * 14(sp)\n"
        "ld a5,  8 * 15(sp)\n"
        "ld a6,  8 * 16(sp)\n"
        "ld a7,  8 * 17(sp)\n"
        "ld s0,  8 * 18(sp)\n"
        "ld s1,  8 * 19(sp)\n"
        "ld s2,  8 * 20(sp)\n"
        "ld s3,  8 * 21(sp)\n"
        "ld s4,  8 * 22(sp)\n"
        "ld s5,  8 * 23(sp)\n"
        "ld s6,  8 * 24(sp)\n"
        "ld s7,  8 * 25(sp)\n"
        "ld s8,  8 * 26(sp)\n"
        "ld s9,  8 * 27(sp)\n"
        "ld s10, 8 * 28(sp)\n"
        "ld s11, 8 * 29(sp)\n"
        "ld sp,  8 * 30(sp)\n"
        "sret\n"
    );
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
    __asm__ __volatile__(
        "sfence.vma\n"
        "csrw satp, %[satp]\n"
        "sfence.vma\n"
        "csrw sscratch, %[sscratch]\n"
        :
        : [satp] "r" (SATP_SV39 | ((uint64_t)runnable_process->page_table / PAGE_SIZE)),
          [sscratch] "r" ((uint64_t) &runnable_process->stack[sizeof(runnable_process->stack)])
    );


    uint64_t *next_process_sp = &runnable_process->sp;
    uint64_t *curr_process_sp = &running_process->sp;

    running_process = runnable_process;
    switch_context(curr_process_sp, next_process_sp);
}

void delay(void) {
    for (int i = 0; i < 30000000; i++)
        __asm__ __volatile__("nop"); 
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


int oct2int(char *oct, int len) {
    int dec = 0;
    for (int i = 0; i < len; i++) {
        if (oct[i] < '0' || oct[i] > '7')
            break;

        dec = dec * 8 + (oct[i] - '0');
    }
    return dec;
}

void kernel_main(void) {
    memset(__bss, 0, (size_t) __bss_end - (size_t) __bss);

    // printf("Hello World!");

#ifdef SELFTEST
    WRITE_CSR(stvec, (uint64_t) kernel_entry);
    run_selftests();
#endif

    // point the kernel exception point to the kernel_entry function
    WRITE_CSR(stvec, (uint64_t) kernel_entry);


    idle_process = create_process(NULL, 0);
    idle_process->pid = 0;
    running_process = idle_process;
    create_process(_binary_shell_bin_start, (size_t)_binary_shell_bin_end - (size_t)_binary_shell_bin_start);
    
    yield();
    PANIC("switched to idle process");
    // create_process((uint64_t) proc_a_entry);
    // create_process((uint64_t) proc_b_entry);

    // create_process(_binary_shell_bin_start, (size_t) _binary_shell_bin_size);
    // yield();

    // for (;;);
    // virtio_blk_init();

    // read all sectors from disk to struct files
    // fs_init();

    // char buf[SECTOR_SIZE];
    // read_write_disk(buf, 0, false);
    // printf("first sector: %s\n", buf);

    // strcpy(buf, "hello from kernel!!!\n");
    // read_write_disk(buf, 0, true);

    // idle_process = create_process(NULL, 0);
    // idle_process->pid = 0;
    // running_process = idle_process;

    // // create_process(_binary_shell_bin_start, (size_t) _binary_shell_bin_size);
    // yield();

    // PANIC("switched to idle process");
}

void handle_syscall(struct trap_frame *f) {
    switch (f->a3)
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

void handle_trap(struct trap_frame *f) {
    uint64_t scause = READ_CSR(scause);
    uint64_t stval = READ_CSR(stval);
    uint64_t user_pc = READ_CSR(sepc);

    // trap is a syscall
    if (scause == 8) {
        handle_syscall(f);

        // go to the nex instruction when we return from trap
        user_pc += 4;
        WRITE_CSR(sepc, user_pc);
    } else {
        PANIC("unexpected trap scause=%x, stval=%x, sepc=%x\n", scause, stval, user_pc);
    }
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