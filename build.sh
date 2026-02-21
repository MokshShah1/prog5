#!/usr/bin/env bash
set -euo pipefail

CFLAGS="-std=c11 -O2 -Wall -Wextra -Werror -pedantic"

# Build assembler
cc $CFLAGS main_asm.c pass1.c pass2.c helpers.c -o hw5-asm

# Build simulator
cc $CFLAGS main_sim.c machine.c execute.c -o hw5-sim -lm

echo "Built: ./hw5-asm and ./hw5-sim"