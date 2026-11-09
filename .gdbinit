set confirm off
symbol-file kernel.elf
target remote 127.0.0.1:26000
set disassemble-next-line auto
set riscv use-compressed-breakpoints yes
