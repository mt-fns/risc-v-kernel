QEMU = qemu-system-riscv32

# Path to clang and compiler flags
CC = clang
OBJCOPY = /usr/bin/llvm-objcopy
CFLAGS = -std=c11 -O2 -g3 -Wall -Wextra --target=riscv32-unknown-elf -fuse-ld=lld -fno-stack-protector -ffreestanding -nostdlib

.PHONY: all fs qemu clean

all: shell.elf kernel.elf

# Build the shell
shell.elf: shell.c user.c common.c
	$(CC) $(CFLAGS) -Wl,-Tuser.ld -Wl,-Map=shell.map -o shell.elf shell.c user.c common.c
	$(OBJCOPY) --set-section-flags .bss=alloc,contents -O binary shell.elf shell.bin
	$(OBJCOPY) -I binary -O elf32-littleriscv shell.bin shell.bin.o

# Build the kernel
kernel.elf: kernel.c common.c shell.bin.o
	$(CC) $(CFLAGS) -Wl,-Tkernel.ld -Wl,-Map=kernel.map -o kernel.elf kernel.c common.c shell.bin.o

# Run QEMU
qemu: kernel.elf fs
	$(QEMU) -machine virt -bios default -nographic -serial mon:stdio --no-reboot \
		-d unimp,guest_errors,int,cpu_reset -D qemu.log \
		-drive id=drive0,file=disk.tar,format=raw,if=none \
		-device virtio-blk-device,drive=drive0,bus=virtio-mmio-bus.0 \
		-kernel kernel.elf

# Clean
clean:
	rm -f *.elf *.map shell.bin shell.bin.o disk.tar qemu.log

# Create TAR file for file system
fs:
	cd disk && tar cf ../disk.tar --format=ustar *.txt
