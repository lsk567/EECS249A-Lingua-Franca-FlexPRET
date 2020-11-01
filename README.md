# EECS249A-Lingua-Franca-FlexPRET
This repo aims to produce a deterministic and timing-aware infrastructure by enabling executing LF programs on top of the FlexPRET processor.

## Prerequisites
We recommend using Ubuntu as the development environment and make sure you have the following installed:
1. `riscv64-unknown-elf-gcc`: https://static.dev.sifive.com/dev-tools/riscv64-unknown-elf-gcc-8.3.0-2019.08.0-x86_64-linux-ubuntu14.tar.gz
2. `spike` (RISC-V simulator): https://github.com/riscv/riscv-isa-sim
3. `riscv-pk` (RISC-V proxy kernel): https://github.com/riscv/riscv-pk

Make sure to add these tools to your `PATH`.

## Running C programs on a RISC-V simulator
1. compile the program using `riscv64-unknown-elf-gcc`
```
riscv64-unknown-elf-gcc -o <bin_file> <source_file>
```
2. run the binary using `spike`
```
sh ./spike.sh <bin_file>
```
