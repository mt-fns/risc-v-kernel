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
// extern char _binary_shell_bin_start[], _binary_shell_bin_size[];

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

paddr_t alloc_pages(uint32_t n) {
    static paddr_t next_paddr = (paddr_t) __free_ram;
    paddr_t paddr = next_paddr;
    next_paddr += n * PAGE_SIZE;

    if (next_paddr > (paddr_t) __free_ram_end)
        PANIC("out of memory");

    memset((void *) paddr, 0, n * PAGE_SIZE);
    return paddr;
}

// virtio helpers
// uint32_t virtio_reg_read32(unsigned offset) {
//     return *((volatile uint32_t *) (VIRTIO_BLK_PADDR + offset));
// }

// uint64_t virtio_reg_read64(unsigned offset) {
//     return *((volatile uint64_t *) (VIRTIO_BLK_PADDR + offset));
// }

// void virtio_reg_write32(unsigned offset, uint32_t value) {
//     *((volatile uint32_t *) (VIRTIO_BLK_PADDR + offset)) = value;
// }

// void virtio_reg_fetch_and_or32(unsigned offset, uint32_t value) {
//     virtio_reg_write32(offset, virtio_reg_read32(offset) | value);
// }

// struct virtio_virtq *virtq_init(unsigned index) {
//     // Allocate a region for the virtqueue.
//     paddr_t virtq_paddr = alloc_pages(align_up(sizeof(struct virtio_virtq), PAGE_SIZE) / PAGE_SIZE);
//     struct virtio_virtq *vq = (struct virtio_virtq *) virtq_paddr;
//     vq->queue_index = index;
//     vq->used_index = (volatile uint16_t *) &vq->used.index;
//     // Select the queue: Write the virtqueue index (first queue is 0).
//     virtio_reg_write32(VIRTIO_REG_QUEUE_SEL, index);
//     // Specify the queue size: Write the # of descriptors we'll use.
//     virtio_reg_write32(VIRTIO_REG_QUEUE_NUM, VIRTQ_ENTRY_NUM);
//     // Write the physical page frame number (not physical address!) of the queue.
//     virtio_reg_write32(VIRTIO_REG_QUEUE_PFN, virtq_paddr / PAGE_SIZE);
//     return vq;
// }

// void virtio_blk_init(void) {
//     if (virtio_reg_read32(VIRTIO_REG_MAGIC) != 0x74726976) {
//         PANIC("virtio: invalid magic");
//     } 
//     if (virtio_reg_read32(VIRTIO_REG_VERSION) != 1) {
//         PANIC("virtio: invalid version");
//     }
//     if (virtio_reg_read32(VIRTIO_REG_DEVICE_ID) != VIRTIO_DEVICE_BLK) {
//         PANIC("virtio: invalid device id");
//     }

//     // reset device
//     virtio_reg_write32(VIRTIO_REG_DEVICE_STATUS, 0);

//     // set ACK status bit
//     virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACK);

//     // set DRIVER status bit
//     virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_DRIVER);

//     // set page size (4kb)
//     virtio_reg_write32(VIRTIO_REG_PAGE_SIZE, PAGE_SIZE);

//     // initialise queue for disk read/write requests
//     blk_request_vq = virtq_init(0);

//     // set DRIVER_OK status bit
//     virtio_reg_write32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_DRIVER_OK);

//     // get disk capacity
//     blk_capacity = virtio_reg_read64(VIRTIO_REG_DEVICE_CONFIG + 0) * SECTOR_SIZE;
//     printf("virtio-blk: capacity is %d bytes\n", (int)blk_capacity);

//      // allocate a region to store requests to the device
//     blk_req_paddr = alloc_pages(align_up(sizeof(*blk_req), PAGE_SIZE) / PAGE_SIZE);
//     blk_req = (struct virtio_blk_req *) blk_req_paddr;
// }

// // Notifies the device that there is a new request. `desc_index` is the index
// // of the head descriptor of the new request.
// void virtq_kick(struct virtio_virtq *vq, int desc_index) {
//     vq->avail.ring[vq->avail.index % VIRTQ_ENTRY_NUM] = desc_index;
//     vq->avail.index++;
//     __sync_synchronize();
//     virtio_reg_write32(VIRTIO_REG_QUEUE_NOTIFY, vq->queue_index);
//     vq->last_used_index++;
// }

// // Returns whether there are requests being processed by the device.
// bool virtq_is_busy(struct virtio_virtq *vq) {
//     return vq->last_used_index != *vq->used_index;
// }

// // Reads/writes from/to virtio-blk device.
// void read_write_disk(void *buf, unsigned sector, int is_write) {
//     if (sector >= blk_capacity / SECTOR_SIZE) {
//         printf("virtio: tried to read/write sector=%d, but capacity is %d\n",
//               sector, blk_capacity / SECTOR_SIZE);
//         return;
//     }

//     // Construct the request according to the virtio-blk specification.
//     blk_req->sector = sector;
//     blk_req->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
//     if (is_write)
//         memcpy(blk_req->data, buf, SECTOR_SIZE);

//     // Construct the virtqueue descriptors (using 3 descriptors).
//     struct virtio_virtq *vq = blk_request_vq;
//     vq->descs[0].addr = blk_req_paddr;
//     vq->descs[0].len = sizeof(uint32_t) * 2 + sizeof(uint64_t);
//     vq->descs[0].flags = VIRTQ_DESC_F_NEXT;
//     vq->descs[0].next = 1;

//     vq->descs[1].addr = blk_req_paddr + offsetof(struct virtio_blk_req, data);
//     vq->descs[1].len = SECTOR_SIZE;
//     vq->descs[1].flags = VIRTQ_DESC_F_NEXT | (is_write ? 0 : VIRTQ_DESC_F_WRITE);
//     vq->descs[1].next = 2;

//     vq->descs[2].addr = blk_req_paddr + offsetof(struct virtio_blk_req, status);
//     vq->descs[2].len = sizeof(uint8_t);
//     vq->descs[2].flags = VIRTQ_DESC_F_WRITE;

//     // Notify the device that there is a new request.
//     virtq_kick(vq, 0);

//     // Wait until the device finishes processing.
//     while (virtq_is_busy(vq))
//         ;

//     // virtio-blk: If a non-zero value is returned, it's an error.
//     if (blk_req->status != 0) {
//         printf("virtio: warn: failed to read/write sector=%d status=%d\n",
//                sector, blk_req->status);
//         return;
//     }

//     // For read operations, copy the data into the buffer.
//     if (!is_write)
//         memcpy(buf, blk_req->data, SECTOR_SIZE);
// }


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
void map_page(uint32_t *table1, vaddr_t vaddr, paddr_t paddr, uint32_t flags) {
    if (!is_aligned(vaddr, PAGE_SIZE)) {
        PANIC("unaligned vaddr %x", vaddr);
    }

    if (!is_aligned(paddr, PAGE_SIZE)) {
        PANIC("unaligned paddr %x", paddr);
    }


    uint32_t vpn1 = vaddr >> 22; // get first 10 bits to index first layer of the page table

    // table1[vpn1] here is a PTE, if it is invalid we need to create a second layer page table entry
    if ((table1[vpn1] & PAGE_V) == 0) {
        uint32_t pt_paddr = alloc_pages(1);

        // the PPN in paddr is bits 31-12. the PPN in the PTE is bits 31-10 which is why we've shifted by 12 then 10
        table1[vpn1] = ((pt_paddr >> 12) << 10) | PAGE_V;
    }

    uint32_t vpn0 = (vaddr << 10) >> 22;

    // extract PTE containing PPN to actual paddr
    uint32_t *table0 = (uint32_t *) (uint64_t) ((table1[vpn1] >> 10) << 12);
    table0[vpn0] = ((paddr >> 12) << 10) | flags | PAGE_V;
}


__attribute__((naked)) void switch_context(uint64_t *prev_sp,
                                           uint64_t *next_sp) {
    __asm__ __volatile__(
        "addi sp, sp, -13 * 4\n"    // reserve space for 13 4-byte registers on the stack
        "sw ra,  0  * 4(sp)\n"
        "sw s0,  1  * 4(sp)\n"
        "sw s1,  2  * 4(sp)\n"
        "sw s2,  3  * 4(sp)\n"
        "sw s3,  4  * 4(sp)\n"
        "sw s4,  5  * 4(sp)\n"
        "sw s5,  6  * 4(sp)\n"
        "sw s6,  7  * 4(sp)\n"
        "sw s7,  8  * 4(sp)\n"
        "sw s8,  9  * 4(sp)\n"
        "sw s9,  10 * 4(sp)\n"
        "sw s10, 11 * 4(sp)\n"
        "sw s11, 12 * 4(sp)\n"

        "sw sp, (a0)\n"         // save modified stack pointer to the previous process's stack pointer (in the process struct)
        "lw sp, (a1)\n"         // switch actual stack pointer to next_sp here

        "lw ra,  0  * 4(sp)\n"  // set ra to this process's entry function (the ret gets called later on)
        "lw s0,  1  * 4(sp)\n"  
        "lw s1,  2  * 4(sp)\n"
        "lw s2,  3  * 4(sp)\n"
        "lw s3,  4  * 4(sp)\n"
        "lw s4,  5  * 4(sp)\n"
        "lw s5,  6  * 4(sp)\n"
        "lw s6,  7  * 4(sp)\n"
        "lw s7,  8  * 4(sp)\n"
        "lw s8,  9  * 4(sp)\n"
        "lw s9,  10 * 4(sp)\n"
        "lw s10, 11 * 4(sp)\n"
        "lw s11, 12 * 4(sp)\n"
        "addi sp, sp, 13 * 4\n"  // we've popped 13 4-byte registers from the stack
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
    struct process *proc = NULL;
    int i = 0;

    for (i = 0; i < PROCS_MAX; i++) {
        if (processes[i].state == PROC_UNUSED) {
            proc = &processes[i];
            break;
        }
    }

    if (i >= PROCS_MAX) {
        PANIC("Ran out of processs slots");
    }

    uint64_t *proc_sp = (uint64_t *) &proc->stack[sizeof(proc->stack)];
    *--proc_sp = 0;                      // s11
    *--proc_sp = 0;                      // s10
    *--proc_sp = 0;                      // s9
    *--proc_sp = 0;                      // s8
    *--proc_sp = 0;                      // s7
    *--proc_sp = 0;                      // s6
    *--proc_sp = 0;                      // s5
    *--proc_sp = 0;                      // s4
    *--proc_sp = 0;                      // s3
    *--proc_sp = 0;                      // s2
    *--proc_sp = 0;                      // s1
    *--proc_sp = 0;                      // s0
    *--proc_sp = (uint64_t) user_entry;          // ra

    // map out kernel memory in every process's page table
    uint32_t *page_table = (uint32_t *) alloc_pages(1);
    for (paddr_t paddr = (paddr_t) __kernel_base; paddr < (paddr_t) __free_ram_end; paddr += PAGE_SIZE) {
        map_page(page_table, paddr, paddr, PAGE_R | PAGE_W | PAGE_X);
    }
    map_page(page_table, VIRTIO_BLK_PADDR, VIRTIO_BLK_PADDR, PAGE_R | PAGE_W); // new
    map_page(page_table, QEMU_TEST_ADDR, QEMU_TEST_ADDR, PAGE_R | PAGE_W); // so PANIC can exit QEMU with paging on

    // map out user pages
    for (uint32_t off = 0; off < image_size; off += PAGE_SIZE) {
        paddr_t page = alloc_pages(1);

        // remaining image size is less than page size
        size_t remaining = image_size - off;
        size_t copy_size = PAGE_SIZE <= remaining ? PAGE_SIZE : remaining;

        memcpy((void *) page, image + off, copy_size);
        map_page(page_table, USER_BASE + off, page, PAGE_U | PAGE_R | PAGE_W | PAGE_X);
    }

    proc->pid = i + 1;
    proc->state = PROC_RUNNABLE;
    proc->sp = (uint64_t) proc_sp;
    proc->page_table = page_table;
    return proc;
}


__attribute__((naked))
__attribute__((aligned(4)))
void kernel_entry(void) {
    __asm__ __volatile__(
        // swap sp and sscratch
        "csrrw sp, sscratch, sp\n"
        
        // store (user) interrupted code state before entering the kernel's trap handler
        "addi sp, sp, -4 * 31\n"
        "sw ra,  4 * 0(sp)\n"
        "sw gp,  4 * 1(sp)\n"
        "sw tp,  4 * 2(sp)\n"
        "sw t0,  4 * 3(sp)\n"
        "sw t1,  4 * 4(sp)\n"
        "sw t2,  4 * 5(sp)\n"
        "sw t3,  4 * 6(sp)\n"
        "sw t4,  4 * 7(sp)\n"
        "sw t5,  4 * 8(sp)\n"
        "sw t6,  4 * 9(sp)\n"
        "sw a0,  4 * 10(sp)\n"
        "sw a1,  4 * 11(sp)\n"
        "sw a2,  4 * 12(sp)\n"
        "sw a3,  4 * 13(sp)\n"
        "sw a4,  4 * 14(sp)\n"
        "sw a5,  4 * 15(sp)\n"
        "sw a6,  4 * 16(sp)\n"
        "sw a7,  4 * 17(sp)\n"
        "sw s0,  4 * 18(sp)\n"
        "sw s1,  4 * 19(sp)\n"
        "sw s2,  4 * 20(sp)\n"
        "sw s3,  4 * 21(sp)\n"
        "sw s4,  4 * 22(sp)\n"
        "sw s5,  4 * 23(sp)\n"
        "sw s6,  4 * 24(sp)\n"
        "sw s7,  4 * 25(sp)\n"
        "sw s8,  4 * 26(sp)\n"
        "sw s9,  4 * 27(sp)\n"
        "sw s10, 4 * 28(sp)\n"
        "sw s11, 4 * 29(sp)\n"

        // store user sp to a0
        "csrr a0, sscratch\n"

        // store user sp in trap frame
        "sw a0, 4 * 30(sp)\n"

        // make sure sscratch still points to the top of the kernel's stack
        "addi a0, sp, 4 * 31\n"
        "csrw sscratch, a0\n"

        // move current sp to a0 to pass in as a parameter (pointer to trap frame) in handle_trap
        "mv a0, sp\n"
        "call handle_trap\n"

        "lw ra,  4 * 0(sp)\n"
        "lw gp,  4 * 1(sp)\n"
        "lw tp,  4 * 2(sp)\n"
        "lw t0,  4 * 3(sp)\n"
        "lw t1,  4 * 4(sp)\n"
        "lw t2,  4 * 5(sp)\n"
        "lw t3,  4 * 6(sp)\n"
        "lw t4,  4 * 7(sp)\n"
        "lw t5,  4 * 8(sp)\n"
        "lw t6,  4 * 9(sp)\n"
        "lw a0,  4 * 10(sp)\n"
        "lw a1,  4 * 11(sp)\n"
        "lw a2,  4 * 12(sp)\n"
        "lw a3,  4 * 13(sp)\n"
        "lw a4,  4 * 14(sp)\n"
        "lw a5,  4 * 15(sp)\n"
        "lw a6,  4 * 16(sp)\n"
        "lw a7,  4 * 17(sp)\n"
        "lw s0,  4 * 18(sp)\n"
        "lw s1,  4 * 19(sp)\n"
        "lw s2,  4 * 20(sp)\n"
        "lw s3,  4 * 21(sp)\n"
        "lw s4,  4 * 22(sp)\n"
        "lw s5,  4 * 23(sp)\n"
        "lw s6,  4 * 24(sp)\n"
        "lw s7,  4 * 25(sp)\n"
        "lw s8,  4 * 26(sp)\n"
        "lw s9,  4 * 27(sp)\n"
        "lw s10, 4 * 28(sp)\n"
        "lw s11, 4 * 29(sp)\n"
        "lw sp,  4 * 30(sp)\n"
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
        : [satp] "r" (SATP_SV32 | ((uint64_t) runnable_process->page_table / PAGE_SIZE)),
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


// void fs_init(void) {
//     for (unsigned sector = 0; sector < sizeof(disk) / SECTOR_SIZE; sector++) {
//         read_write_disk(&disk[sector * SECTOR_SIZE], sector, false);
//     }

//     unsigned off = 0;

//     for (int i = 0; i < FILES_MAX; i++) {
//         struct tar_header *header = (struct tar_header *) &disk[off];

//         if (header->name[0] == '\0') {
//             break;
//         }

//         if (strcmp(header->magic, "ustar") != 0) {
//             PANIC("invalid tar header magic");
//         }

//         int filesz = oct2int(header->size, sizeof(header->size));
//         struct file *file = &files[i];

//         file->in_use = true;
//         strcpy(file->name, header->name);
//         memcpy(file->data, header->data, filesz);
//         file->size = filesz;
//         printf("file: %s, size=%d\n", file->name, file->size);

//         off += align_up(sizeof(struct tar_header) + filesz, SECTOR_SIZE);
//     }
// }

void kernel_main(void) {
    memset(__bss, 0, (size_t) __bss_end - (size_t) __bss);

    printf("Hello World!");

#ifdef SELFTEST
    WRITE_CSR(stvec, (uint32_t) kernel_entry);
    run_selftests();
#endif

    // point the kernel exception point to the kernel_entry function
    WRITE_CSR(stvec, (uint64_t) kernel_entry);
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
    uint32_t scause = READ_CSR(scause);
    uint32_t stval = READ_CSR(stval);
    uint32_t user_pc = READ_CSR(sepc);

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