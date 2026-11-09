QEMU = qemu-system-riscv32

# bash so recipes can use `set -o pipefail` (dash's /bin/sh can't)
SHELL := /bin/bash

# Path to clang and compiler flags
CC = clang
OBJCOPY = /usr/bin/llvm-objcopy
CFLAGS = -std=c11 -O2 -g3 -Wall -Wextra --target=riscv32-unknown-elf -fuse-ld=lld -fno-stack-protector -ffreestanding -nostdlib

KERNEL_SRCS = kernel.c common.c
KERNEL_HDRS = kernel.h common.h

# Kernel image to boot with `make qemu` / `make qemu-gdb`.
# `make qemu KERNEL=kernel-test.elf` runs the test kernel with no timeout.
KERNEL ?= kernel.elf

QEMUOPTS = -machine virt -bios default -nographic -serial mon:stdio --no-reboot \
	-drive id=drive0,file=disk.tar,format=raw,if=none \
	-device virtio-blk-device,drive=drive0,bus=virtio-mmio-bus.0

# Selftest settings. The pass line is passed into the C build too, so the
# kernel and the check below can't spell it differently.
TEST_TIMEOUT ?= 10
SELFTEST_PASS = SELFTEST: ALL PASSED

.PHONY: all fs qemu qemu-gdb test clean

all: shell.elf kernel.elf

# Build the shell
shell.elf: shell.c user.c common.c
	$(CC) $(CFLAGS) -Wl,-Tuser.ld -Wl,-Map=shell.map -o shell.elf shell.c user.c common.c
	$(OBJCOPY) --set-section-flags .bss=alloc,contents -O binary shell.elf shell.bin
	$(OBJCOPY) -I binary -O elf32-littleriscv shell.bin shell.bin.o

# shell.bin.o is produced by the shell.elf recipe
shell.bin.o: shell.elf ;

# Build the kernel
kernel.elf: $(KERNEL_SRCS) $(KERNEL_HDRS) shell.bin.o
	$(CC) $(CFLAGS) -Wl,-Tkernel.ld -Wl,-Map=kernel.map -o $@ $(KERNEL_SRCS) shell.bin.o

# Build the test kernel: same sources plus selftest.c, with SELFTEST defined.
kernel-test.elf: $(KERNEL_SRCS) $(KERNEL_HDRS) selftest.c selftest.h shell.bin.o
	$(CC) $(CFLAGS) -DSELFTEST -DSELFTEST_PASS_MSG='"$(SELFTEST_PASS)"' \
		-Wl,-Tkernel.ld -Wl,-Map=kernel-test.map -o $@ $(KERNEL_SRCS) selftest.c shell.bin.o

# Run the test kernel. Passes only if QEMU exits 0 *and* the pass line was
# printed; a timeout (exit 124), a panic, or a silent shutdown all fail.
# Output is kept in test.log. 2>&1 matters: OpenSBI uses semihosting as the
# console here, and QEMU writes semihosting output to stderr.
test: kernel-test.elf fs
	@set -o pipefail; \
	timeout -k 2 $(TEST_TIMEOUT) $(QEMU) $(QEMUOPTS) -kernel kernel-test.elf \
		-d unimp,guest_errors -D qemu-test.log </dev/null 2>&1 | tee test.log; \
	status=$$?; \
	if [ $$status -eq 124 ]; then \
		echo "make test: FAIL (timed out after $(TEST_TIMEOUT)s, see test.log)"; exit 1; \
	elif [ $$status -ne 0 ]; then \
		echo "make test: FAIL (qemu exit status $$status, see test.log)"; exit 1; \
	elif ! grep -qF '$(SELFTEST_PASS)' test.log; then \
		echo "make test: FAIL (qemu exited 0 but '$(SELFTEST_PASS)' not printed)"; exit 1; \
	fi; \
	echo "make test: PASS"

# Run QEMU
qemu: shell.elf $(KERNEL) fs
	$(QEMU) $(QEMUOPTS) -d unimp,guest_errors,int,cpu_reset -D qemu.log -kernel $(KERNEL)

# try to generate a unique GDB port
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
# QEMU's gdb stub command line changed in 0.11
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)

# Regenerated every run so symbols follow KERNEL= (phony on purpose).
.PHONY: .gdbinit
.gdbinit: .gdbinit.tmpl-riscv
	sed -e "s/:1234/:$(GDBPORT)/" -e "s/^symbol-file kernel\.elf$$/symbol-file $(KERNEL)/" < $< > $@

qemu-gdb: shell.elf $(KERNEL) fs .gdbinit
	$(QEMU) $(QEMUOPTS) -d unimp,guest_errors,int,cpu_reset -D qemu.log -kernel $(KERNEL) \
		-S $(QEMUGDB)

print-gdbport:
	@echo $(GDBPORT)

clean:
	rm -f *.elf *.map shell.bin shell.bin.o disk.tar qemu.log qemu-test.log test.log

# Create TAR file for file system
fs:
	cd disk && tar cf ../disk.tar --format=ustar *.txt
