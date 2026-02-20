HW5 – Tinker Assembler & Simulator (Stage 3)
Name: Moksh Shah
EID: mps2965

Files
hw5-asm.c
hw5-sim.c
test_hw5.c
build.sh
fibonacci.tk
binary_search.tk
matrix_multiplication.tk

Build
gcc -std=c11 -O2 -Wall -Wextra -Werror -pedantic hw5-asm.c -o hw5-asm
gcc -std=c11 -O2 -Wall -Wextra -Werror -pedantic hw5-sim.c -o hw5-sim
gcc -std=c11 -O2 -Wall -Wextra -Werror -pedantic test_hw5.c -o test_hw5

Run Assembler
./hw5-asm program.tk program.tko

Run Simulator
./hw5-sim program.tko

Run Tests
./test_hw5
